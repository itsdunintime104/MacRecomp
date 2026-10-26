/* Boot driver: does what the Windows loader does for the game's PE32 image,
 * then runs the lifted PE entry point on the host main thread. The pattern follows
 * the engine's startup.c (guest memory, image, dispatch, main guest stack,
 * entry through recomp_lookup), with the PE loader and the Win32 slices in
 * place of the Mach-O loader and the POSIX thunks. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>

#include "d3dx_guest.h"
#include "frame_trace.h"
#include "game_hooks.h"
#include "recomp_types.h"
#include "diag.h"
#include "guest_call.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "pe_load.h"
#include "win32_fs.h"
#include "win32_thunk.h"
#include "win32_ui.h"

int recomp_dispatch_init(void);
/* Generated with the import inventory (win32_unsupported.c). */
const char *win32_inventory_name(const char *dll, uint16_t ordinal);

/* Main guest stack: the engine's main-stack range (guest_call.h). */
#define MAIN_STACK_LO 0xFD000000u
#define MAIN_STACK_HI 0xFE000000u
/* Return address under the entry point. Never mapped. */
#define ENTRY_RET_SENTINEL 0xFFFFF000u

#define PE_SCN_MEM_EXECUTE 0x20000000u

/* GAME_SAVE_NAME: the save folder under ~/Library/Application Support.
 * windows/build/game.mk sets it from the game's APP_SAVE_NAME. */
#ifndef GAME_SAVE_NAME
#error "GAME_SAVE_NAME is not set (windows/build/game.mk sets it from APP_SAVE_NAME)"
#endif

/* ---- Import call trace (HP_TRACE_IMPORTS=N) ------------------------------
 * Format of notes/golden_trace.md "Logging spec": one line per call for the
 * first N calls,
 *   IMP <n> t=<tid> <DLL>!<Func>(<a0> ... <aK-1>) ret <RET> -> <EAX>
 * and `THR <tid> <name>` for each MSVC SetThreadName exception (0x406D1388).
 * n is taken at entry; tid is the TIB's thread id (what GetCurrentThreadId
 * returns); K is the number of argument slots the thunk popped, measured
 * from g_esp (0 for a cdecl import). Every IAT slot gets a trampoline, and so
 * does every non-zero address GetProcAddress returns, so calls through
 * function pointers are traced too. Each traced function needs its own
 * trampoline, because a thunk has no context argument. */
#define TRACE_SLOTS 512
#define TRACE_MAX_ARGS 16
#define TIB_TID 0x24u
#define MS_VC_EXCEPTION 0x406D1388u
static recomp_func_t g_trace_fn[TRACE_SLOTS];
static char g_trace_name[TRACE_SLOTS][80];
static unsigned g_trace_count;
static long g_trace_limit;
static _Atomic long g_trace_calls;
static pthread_mutex_t g_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_slot_getprocaddress = -1, g_slot_getmodulehandle = -1, g_slot_loadlibrary = -1,
           g_slot_raiseexception = -1;

/* Module handle -> DLL name, from traced GetModuleHandleA/LoadLibraryA. */
#define TRACE_MODULES 32
static struct { uint32_t handle; char name[48]; } g_modules[TRACE_MODULES];
static unsigned g_module_count;

static uint32_t trace_tid(void)
{
    return g_fs_base != 0 ? MEM32(g_fs_base + TIB_TID) : 0;
}

static void guest_cstr(uint32_t a, char *out, size_t size)
{
    size_t i = 0;
    for (; a != 0 && i + 1 < size && MEM8(a + i) != 0; i++)
        out[i] = (char)MEM8(a + i);
    out[i] = '\0';
}

static uint32_t trace_wrap(recomp_func_t fn, const char *name, int *slot);

static void trace_call(unsigned k)
{
    long n = g_trace_calls++;
    if (n >= g_trace_limit) {
        g_trace_fn[k]();
        return;
    }
    uint32_t esp0 = g_esp, ret = MEM32(esp0), tid = trace_tid(), a[TRACE_MAX_ARGS];
    /* Read argument slots only below the thread's stack base (TIB +4): the
     * first calls run near the top of the stack, and above it is unmapped. */
    uint32_t base = g_fs_base != 0 ? MEM32(g_fs_base + 4u) : esp0 + 4u;
    for (unsigned i = 0; i < TRACE_MAX_ARGS; i++) {
        uint32_t slot = esp0 + 4u + 4u * i;
        a[i] = slot + 4u <= base ? MEM32(slot) : 0;
    }
    if ((int)k == g_slot_raiseexception && a[0] == MS_VC_EXCEPTION && a[2] >= 3 && a[3] != 0 &&
        MEM32(a[3]) == 0x1000u) {
        char name[64];
        uint32_t who = MEM32(a[3] + 8);
        guest_cstr(MEM32(a[3] + 4), name, sizeof name);
        fprintf(stderr, "THR %04X %s\n", who == 0xFFFFFFFFu ? tid : who, name);
    }
    g_trace_fn[k]();
    uint32_t eax = g_eax;
    if ((int)k == g_slot_getprocaddress && eax != 0 && (a[1] >> 16) != 0) {
        char dll[48] = "?", proc[64], full[120];
        pthread_mutex_lock(&g_trace_lock);
        for (unsigned i = 0; i < g_module_count; i++)
            if (g_modules[i].handle == a[0])
                snprintf(dll, sizeof dll, "%s", g_modules[i].name);
        pthread_mutex_unlock(&g_trace_lock);
        guest_cstr(a[1], proc, sizeof proc);
        snprintf(full, sizeof full, "%s!%s", dll, proc);
        recomp_func_t fn = guest_resolve(eax);
        if (fn != NULL)
            g_eax = eax = trace_wrap(fn, full, NULL);
    } else if (((int)k == g_slot_getmodulehandle || (int)k == g_slot_loadlibrary) && eax != 0 &&
               a[0] != 0) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_module_count < TRACE_MODULES) {
            g_modules[g_module_count].handle = eax;
            guest_cstr(a[0], g_modules[g_module_count].name, sizeof g_modules[0].name);
            g_module_count++;
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    uint32_t popped = g_esp - esp0 >= 4u ? (g_esp - esp0 - 4u) / 4u : 0;
    unsigned nargs = popped < TRACE_MAX_ARGS ? popped : TRACE_MAX_ARGS;
    char args[TRACE_MAX_ARGS * 9 + 1];
    size_t off = 0;
    args[0] = '\0';
    for (unsigned i = 0; i < nargs; i++)
        off += (size_t)snprintf(args + off, sizeof args - off, i ? " %08X" : "%08X", a[i]);
    fprintf(stderr, "IMP %ld t=%04X %s(%s) ret %08X -> %08X\n", n, tid, g_trace_name[k], args, ret,
            eax);
}

/* k is three octal digits, so 0##k is the slot index 0..511. */
#define T1(k) static void trace_##k(void) { trace_call(0##k); }
#define T8(k) T1(k##0) T1(k##1) T1(k##2) T1(k##3) T1(k##4) T1(k##5) T1(k##6) T1(k##7)
#define T64(k) T8(k##0) T8(k##1) T8(k##2) T8(k##3) T8(k##4) T8(k##5) T8(k##6) T8(k##7)
T64(0) T64(1) T64(2) T64(3) T64(4) T64(5) T64(6) T64(7)
#define R1(k) trace_##k,
#define R8(k) R1(k##0) R1(k##1) R1(k##2) R1(k##3) R1(k##4) R1(k##5) R1(k##6) R1(k##7)
#define R64(k) R8(k##0) R8(k##1) R8(k##2) R8(k##3) R8(k##4) R8(k##5) R8(k##6) R8(k##7)
static const recomp_func_t g_trampolines[TRACE_SLOTS] = {
    R64(0) R64(1) R64(2) R64(3) R64(4) R64(5) R64(6) R64(7)};

/* The guest address of fn's trampoline, made on first use; fn's own
 * address when the pool is full. *slot (if given) gets the slot or -1. */
static uint32_t trace_wrap(recomp_func_t fn, const char *name, int *slot)
{
    if (slot != NULL)
        *slot = -1;
    pthread_mutex_lock(&g_trace_lock);
    unsigned k = 0;
    while (k < g_trace_count && g_trace_fn[k] != fn)
        k++;
    if (k == g_trace_count) {
        if (k == TRACE_SLOTS) {
            pthread_mutex_unlock(&g_trace_lock);
            fprintf(stderr, "pe_boot: trace pool full, %s not traced\n", name);
            return host_func_va(fn);
        }
        g_trace_fn[k] = fn;
        snprintf(g_trace_name[k], sizeof g_trace_name[k], "%s", name);
        g_trace_count++;
    }
    pthread_mutex_unlock(&g_trace_lock);
    if (slot != NULL)
        *slot = (int)k;
    return host_func_va(g_trampolines[k]);
}

/* HPRECOMP_FRAME_TRACE: every import through a timed trampoline. */
static uint32_t resolve_timed(const char *dll, const char *name, uint16_t ordinal, int by_ordinal)
{
    uint32_t va = win32_resolve_import(dll, name, ordinal, by_ordinal);
    recomp_func_t fn = va != 0 ? guest_resolve(va) : NULL;
    if (fn == NULL)
        return va;
    char full[120];
    if (by_ordinal)
        snprintf(full, sizeof full, "%s!#%u", dll, ordinal);
    else
        snprintf(full, sizeof full, "%s!%s", dll, name);
    return ft_wrap(fn, full);
}

static uint32_t resolve_traced(const char *dll, const char *name, uint16_t ordinal,
                               int by_ordinal)
{
    uint32_t va = win32_resolve_import(dll, name, ordinal, by_ordinal);
    recomp_func_t fn = va != 0 ? guest_resolve(va) : NULL;
    if (fn == NULL)
        return va;
    char full[120];
    const char *named = by_ordinal ? win32_inventory_name(dll, ordinal) : NULL;
    if (named != NULL) /* golden traces name ordinal imports too */
        snprintf(full, sizeof full, "%s!%s", dll, named);
    else if (by_ordinal)
        snprintf(full, sizeof full, "%s!#%u", dll, ordinal);
    else
        snprintf(full, sizeof full, "%s!%s", dll, name);
    int k;
    uint32_t traced = trace_wrap(fn, full, &k);
    if (k >= 0 && !by_ordinal) {
        if (strcmp(name, "GetProcAddress") == 0)
            g_slot_getprocaddress = k;
        else if (strcmp(name, "GetModuleHandleA") == 0)
            g_slot_getmodulehandle = k;
        else if (strcmp(name, "LoadLibraryA") == 0)
            g_slot_loadlibrary = k;
        else if (strcmp(name, "RaiseException") == 0)
            g_slot_raiseexception = k;
    }
    return traced;
}

/* The redirected save root (make_save_dir). */
static char g_save_dir[1024];

/* Default game hook (game_hooks.h): no game-only boot step. */
__attribute__((weak)) int game_boot_hook(const char *save_dir, pe_import_resolver_t *resolve)
{
    (void)save_dir;
    (void)resolve;
    return 0;
}

/* Executable section bounds from the mapped PE headers, for
 * RECOMP_ICALL_IS_CODE. */
static void set_code_range(uint32_t image_base)
{
    uint32_t nt = image_base + MEM32(image_base + 0x3Cu);
    uint32_t nsect = MEM16(nt + 6u);
    uint32_t sect = nt + 24u + MEM16(nt + 20u);
    uint32_t lo = 0, hi = 0;
    for (uint32_t i = 0; i < nsect; i++) {
        uint32_t s = sect + 40u * i;
        if ((MEM32(s + 36u) & PE_SCN_MEM_EXECUTE) == 0)
            continue;
        uint32_t va = image_base + MEM32(s + 12u), end = va + MEM32(s + 8u);
        if (lo == 0 || va < lo)
            lo = va;
        if (end > hi)
            hi = end;
    }
    g_xbox_code_lo = lo;
    g_xbox_code_hi = hi;
    fprintf(stderr, "pe_boot: code 0x%08X-0x%08X\n", lo, hi);
}

/* atexit handler: runs when the game quits through ExitProcess or
 * TerminateProcess on itself (win32_mem.c, both call exit) or returns from
 * its entry point (main returns). The fatal-signal handler leaves with _exit,
 * so a crashed run writes no stamp. */
static void stamp_on_exit(void)
{
    d3dx_game_cache_stamp(g_save_dir);
}

static int make_save_dir(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (home == NULL)
        return -1;
    snprintf(out, size, "%s/Library/Application Support/" GAME_SAVE_NAME, home);
    /* A fresh HOME has no Library folders yet. */
    if (w32_fs_mkdir_p(out) != 0) {
        fprintf(stderr, "pe_boot: mkdir %s: %s\n", out, strerror(errno));
        return -1;
    }
    return 0;
}

int pe_boot_run(const char *exe_path, const char *game_dir)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0)
        return 1;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base;

    const char *trace = getenv("HP_TRACE_IMPORTS");
    g_trace_limit = trace != NULL ? atol(trace) : 0;
    char *save_dir = g_save_dir;
    if (make_save_dir(save_dir, sizeof g_save_dir) != 0)
        return 1;
    pe_import_resolver_t resolve = g_trace_limit > 0 ? resolve_traced
                                   : ft_enabled()    ? resolve_timed
                                                     : win32_resolve_import;
    if (game_boot_hook(save_dir, &resolve) != 0)
        return 1;
    pe_image_t img;
    if (pe_load_image(exe_path, resolve, &img) != 0)
        return 1;
    fprintf(stderr, "pe_boot: image 0x%08X-0x%08X entry 0x%08X unbound %u\n", img.image_base,
            img.image_end, img.entry_va, img.unbound_imports);
    set_code_range(img.image_base);

    /* With no cache yet, the game creates one during this run, made by this
     * build's compiler: stamp it when the game quits, or the next start would
     * see a cache with no stamp and mark it stale. */
    if (d3dx_game_cache_check(save_dir) == 0)
        atexit(stamp_on_exit);
    w32_fs_set_game_dir(game_dir);
    w32_fs_set_save_dir(save_dir);
    w32_ui_set_window_flags(SDL_WINDOW_VULKAN);

    recomp_dispatch_init();
    diag_init();

    if (mprotect(guest_ptr(MAIN_STACK_LO), MAIN_STACK_HI - MAIN_STACK_LO,
                 PROT_READ | PROT_WRITE) != 0) {
        perror("pe_boot: map main stack");
        return 1;
    }
    g_esp = MAIN_STACK_HI - 16u;
    /* Windows starts a process with x87 precision 53-bit (0x027F). */
    g_fp_control_word = 0x027F;
    g_mxcsr = 0x1F80;
    w32_tib_attach(MAIN_STACK_HI, MAIN_STACK_LO);

    recomp_func_t entry = recomp_lookup(img.entry_va);
    if (entry == NULL) {
        fprintf(stderr, "pe_boot: entry 0x%08X is not recompiled\n", img.entry_va);
        return 3;
    }
    g_esp -= 4;
    MEM32(g_esp) = ENTRY_RET_SENTINEL;
    g_ebp = 0;
    g_seh_ebp = 0;
    fprintf(stderr, "pe_boot: calling entry 0x%08X\n", img.entry_va);
    entry();
    fprintf(stderr, "pe_boot: entry returned eax=0x%08X\n", g_eax);
    return (int)g_eax;
}
