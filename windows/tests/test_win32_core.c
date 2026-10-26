/* Win32 core slice test: resolver, weak/strong export tables, last error,
 * handle table, the 9 core imports, Encode/DecodePointer and the
 * generated stubs. Guest stack as in
 * the engine's THUNK_SLICES.md. Links with win32_core.c, win32_unsupported.c
 * and the engine's guest_mem.c, host_funcs.c and recomp_glue.c, built with
 * -DRECOMP_GLUE_NO_MAIN. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "quiet_abort.h"
#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "win32_thunk.h"

/* Generated dispatch; this test has no lifted code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

/* ---- Strong slice tables: they must win over core's weak empty ones ---- */

static int g_virtualalloc_calls, g_htons_calls;
static void test_VirtualAlloc(void)
{
    g_virtualalloc_calls++;
    W32_RETURN(4, 0x12340000u);
}
static void test_htons(void)
{
    g_htons_calls++;
    uint32_t v = W32_ARG(0);
    W32_RETURN(1, ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}
const win32_export_t win32_exports_mem[] = {
    {"KERNEL32.dll", "VirtualAlloc", 0, test_VirtualAlloc},
    {0},
};
const win32_export_t win32_exports_net[] = {
    {"WS2_32.dll", NULL, 9, test_htons},
    {0},
};
/* Listed by name only, as the DirectSound slice lists it; LEGOIndy.exe
 * imports it as DSOUND.dll ordinal 11. */
static void test_DirectSoundCreate8(void)
{
    W32_RETURN(3, 0x8DD50001u);
}
const win32_export_t win32_exports_audio[] = {
    {"DSOUND.dll", "DirectSoundCreate8", 0, test_DirectSoundCreate8},
    {0},
};

/* ---- Guest stack ------------------------------------------------------- */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define STR_AREA 0x00FF8000u
#define RET_ADDR 0xDEAD0001u

static uint32_t g_str_next = STR_AREA;
static uint32_t guest_string(const char *s)
{
    uint32_t a = g_str_next;
    strcpy((char *)guest_ptr(a), s);
    g_str_next += (uint32_t)strlen(s) + 1u;
    return a;
}

static recomp_func_t resolve_fn(const char *dll, const char *name, uint16_t ord, int by_ord)
{
    uint32_t va = win32_resolve_import(dll, name, ord, by_ord);
    return va != 0 ? guest_resolve(va) : NULL;
}

/* Push args right to left and the return address, call fn, check that it
 * popped 4 + 4*pop_slots bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call_thunk(recomp_func_t fn, unsigned pop_slots, unsigned nargs,
                           const uint32_t *args)
{
    g_esp = STACK_TOP;
    for (unsigned i = nargs; i-- > 0;) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = RET_ADDR;
    uint32_t start = g_esp;
    g_ebx = 0xB0B0B0B0u, g_esi = 0x51515151u, g_edi = 0xD1D1D1D1u, g_ebp = 0xBEBEBEBEu;
    g_eax = 0xCCCCCCCCu;
    fn();
    CHECK(g_esp == start + 4u + 4u * pop_slots);
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBEBEBEBEu);
    return g_eax;
}

static recomp_func_t core(const char *name)
{
    recomp_func_t fn = resolve_fn("kernel32.DLL", name, 0, 0);
    CHECK(fn != NULL);
    return fn;
}

/* ---- stderr capture ---------------------------------------------------- */

static FILE *g_cap;
static int g_saved_fd;
static void capture_begin(void)
{
    fflush(stderr);
    g_cap = tmpfile();
    g_saved_fd = dup(2);
    dup2(fileno(g_cap), 2);
}
static char *capture_end(void)
{
    static char buf[4096];
    fflush(stderr);
    dup2(g_saved_fd, 2);
    close(g_saved_fd);
    rewind(g_cap);
    size_t n = fread(buf, 1, sizeof buf - 1, g_cap);
    buf[n] = '\0';
    fclose(g_cap);
    return buf;
}

static int count_lines(const char *text, const char *line)
{
    int n = 0;
    size_t len = strlen(line);
    for (const char *p = text; (p = strstr(p, line)) != NULL; p += len)
        n++;
    return n;
}

/* ---- Handle objects ---------------------------------------------------- */

struct test_obj {
    struct w32_object hdr;
    int destroyed;
};
static void test_destroy(struct w32_object *self)
{
    ((struct test_obj *)self)->destroyed++;
}

static void test_handles(void)
{
    struct test_obj a = {{W32_KIND_EVENT, 0, test_destroy}, 0};
    struct test_obj b = {{W32_KIND_FILE, 0, test_destroy}, 0};
    uint32_t ha = w32_handle_new(&a.hdr), hb = w32_handle_new(&b.hdr);
    CHECK(ha != 0 && ha != W32_INVALID_HANDLE_VALUE && hb != 0 && hb != ha);
    CHECK(ha >= W32_HANDLE_BASE && (ha - W32_HANDLE_BASE) % 4u == 0);
    CHECK(a.hdr.refs == 1);
    CHECK(w32_handle_get(ha, W32_KIND_ANY) == &a.hdr);
    CHECK(w32_handle_get(ha, W32_KIND_EVENT) == &a.hdr);
    CHECK(w32_handle_get(ha, W32_KIND_FILE) == NULL);
    CHECK(w32_handle_get(0, W32_KIND_ANY) == NULL);
    CHECK(w32_handle_get(ha + 1u, W32_KIND_ANY) == NULL);

    /* CloseHandle thunk: real handle, then stale, then pseudo-handles. */
    recomp_func_t close_fn = core("CloseHandle");
    uint32_t arg = ha;
    CHECK(call_thunk(close_fn, 1, 1, &arg) == W32_TRUE);
    CHECK(a.destroyed == 1);
    CHECK(w32_handle_get(ha, W32_KIND_ANY) == NULL);
    w32_set_last_error(0);
    CHECK(call_thunk(close_fn, 1, 1, &arg) == W32_FALSE);
    CHECK(w32_last_error() == W32_ERROR_INVALID_HANDLE);
    CHECK(a.destroyed == 1);
    arg = W32_PSEUDO_CURRENT_PROCESS;
    CHECK(call_thunk(close_fn, 1, 1, &arg) == W32_TRUE);
    arg = W32_PSEUDO_CURRENT_THREAD;
    CHECK(call_thunk(close_fn, 1, 1, &arg) == W32_TRUE);

    /* Round robin: the freed index is not reissued next. */
    struct test_obj c = {{W32_KIND_MUTEX, 0, test_destroy}, 0};
    uint32_t hc = w32_handle_new(&c.hdr);
    CHECK(hc != 0 && hc != ha && hc != hb);
    CHECK(w32_handle_close(hb) == W32_TRUE && b.destroyed == 1);
    CHECK(w32_handle_close(hc) == W32_TRUE && c.destroyed == 1);
}

/* ---- Imports ----------------------------------------------------------- */

static void test_last_error(void)
{
    uint32_t code = 0x1234u;
    CHECK(call_thunk(core("SetLastError"), 1, 1, &code) == 0);
    CHECK(call_thunk(core("GetLastError"), 0, 0, NULL) == 0x1234u);
}

static void test_modules(void)
{
    recomp_func_t gmh = core("GetModuleHandleA"), ll = core("LoadLibraryA");
    recomp_func_t gpa = core("GetProcAddress"), fl = core("FreeLibrary");
    uint32_t a;

    a = 0;
    CHECK(call_thunk(gmh, 1, 1, &a) == 0x00400000u);
    a = guest_string("kernel32");
    uint32_t k32 = call_thunk(gmh, 1, 1, &a);
    CHECK(k32 == 0x7F000000u + 0x10000u * 6);
    a = guest_string("D3DX9_41.DLL");
    CHECK(call_thunk(gmh, 1, 1, &a) == 0x7F000000u + 0x10000u * 3);
    a = guest_string("d3dx9_43.dll");
    w32_set_last_error(0);
    CHECK(call_thunk(gmh, 1, 1, &a) == 0 && w32_last_error() == 126u);

    a = guest_string("xinput1_4.dll");
    w32_set_last_error(0);
    CHECK(call_thunk(ll, 1, 1, &a) == 0 && w32_last_error() == 126u);
    a = guest_string("XINPUT1_3");
    CHECK(call_thunk(ll, 1, 1, &a) == 0x7F000000u + 0x10000u * 13);
    a = guest_string("ws2_32.dll");
    uint32_t ws2 = call_thunk(ll, 1, 1, &a);
    CHECK(ws2 == 0x7F000000u + 0x10000u * 11);
    a = ws2;
    CHECK(call_thunk(fl, 1, 1, &a) == W32_TRUE);

    /* GetProcAddress by name: a core entry, the same address the loader binds. */
    uint32_t args[2] = {k32, guest_string("GetLastError")};
    CHECK(call_thunk(gpa, 2, 2, args) ==
          win32_resolve_import("KERNEL32.dll", "GetLastError", 0, 0));
    /* A strong slice table wins; by-ordinal and by-name both find it. */
    args[1] = guest_string("VirtualAlloc");
    CHECK(guest_resolve(call_thunk(gpa, 2, 2, args)) == test_VirtualAlloc);
    args[0] = ws2, args[1] = 9;
    CHECK(guest_resolve(call_thunk(gpa, 2, 2, args)) == test_htons);
    args[1] = guest_string("htons");
    CHECK(guest_resolve(call_thunk(gpa, 2, 2, args)) == test_htons);
    /* Not imported: NULL with ERROR_PROC_NOT_FOUND. */
    args[0] = k32, args[1] = guest_string("NotARealExport");
    w32_set_last_error(0);
    CHECK(call_thunk(gpa, 2, 2, args) == 0 && w32_last_error() == 127u);
    args[0] = 0x00400000u, args[1] = guest_string("GetLastError");
    CHECK(call_thunk(gpa, 2, 2, args) == 0 && w32_last_error() == 127u);
}

/* Generated with the import inventory (win32_unsupported.c). */
const char *win32_inventory_name(const char *dll, uint16_t ordinal);

/* Ordinal imports map back to their inventory names (the boot trace prints
 * them by name, as the golden trace does). */
static void test_inventory_name(void)
{
    const char *n = win32_inventory_name("ws2_32.dll", 115);
    CHECK(n != NULL && strcmp(n, "WSAStartup") == 0);
    n = win32_inventory_name("XINPUT1_3.dll", 2);
    CHECK(n != NULL && strcmp(n, "XInputGetState") == 0);
    n = win32_inventory_name("OLEAUT32.dll", 6);
    CHECK(n != NULL && strcmp(n, "SysFreeString") == 0);
    CHECK(win32_inventory_name("KERNEL32.dll", 1) == NULL); /* imported by name */
    CHECK(win32_inventory_name("WS2_32.dll", 999) == NULL);
    n = win32_inventory_name("DSOUND.dll", 11);
    CHECK(n != NULL && strcmp(n, "DirectSoundCreate8") == 0);
}

/* LEGO Indiana Jones: an ordinal import finds a slice entry listed by its
 * inventory name, and d3dx9_35 resolves through the d3dx9_41 entries. */
static void test_indy_resolution(void)
{
    recomp_func_t fn = resolve_fn("dsound.dll", "", 11, 1);
    CHECK(fn == test_DirectSoundCreate8);
    uint32_t args[4] = {0, 0x00F10000u, 0, 0};
    CHECK(fn != NULL && call_thunk(fn, 3, 3, args) == 0x8DD50001u);
    /* by name too (GetProcAddress) */
    CHECK(resolve_fn("DSOUND.dll", "DirectSoundCreate8", 0, 0) == test_DirectSoundCreate8);

    /* No d3d slice here: both names bind the same d3dx9_41 stub. */
    recomp_func_t m35 = resolve_fn("d3dx9_35.dll", "D3DXMatrixMultiply", 0, 0);
    CHECK(m35 != NULL && m35 == resolve_fn("d3dx9_41.dll", "D3DXMatrixMultiply", 0, 0));
    /* Only in d3dx9_35's inventory rows: its own stub, 16 bytes. */
    fn = resolve_fn("d3dx9_35.dll", "D3DXMatrixLookAtLH", 0, 0);
    capture_begin();
    CHECK(fn != NULL && call_thunk(fn, 4, 4, args) == 0);
    char *out = capture_end();
    CHECK(count_lines(out, "UNSUPPORTED d3dx9_35.dll!D3DXMatrixLookAtLH: not implemented") == 1);
    CHECK(win32_resolve_import("d3dx9_43.dll", "D3DXMatrixMultiply", 0, 0) == 0);

    /* The new DLLs have module handles after LEGO Harry Potter's 14. */
    uint32_t a = guest_string("d3dx9_35");
    CHECK(call_thunk(core("GetModuleHandleA"), 1, 1, &a) == 0x7F000000u + 0x10000u * 15);
    a = guest_string("WINMM.DLL");
    CHECK(call_thunk(core("LoadLibraryA"), 1, 1, &a) == 0x7F000000u + 0x10000u * 17);
}

/* EncodePointer/DecodePointer resolve through GetProcAddress and round-trip. */
static void test_encode_pointer(void)
{
    const char *names[] = {"EncodePointer", "DecodePointer"};
    uint32_t args[2] = {0x7F000000u + 0x10000u * 6, 0};
    for (size_t i = 0; i < 2; i++) {
        args[1] = guest_string(names[i]);
        CHECK(guest_resolve(call_thunk(core("GetProcAddress"), 2, 2, args)) == core(names[i]));
    }
    uint32_t p = 0x00501234u;
    uint32_t e = call_thunk(core("EncodePointer"), 1, 1, &p);
    CHECK(call_thunk(core("DecodePointer"), 1, 1, &e) == p);
}

static void test_resolver(void)
{
    /* Strong tables over stubs, by name and by ordinal. */
    recomp_func_t fn = resolve_fn("kernel32.dll", "VirtualAlloc", 0, 0);
    CHECK(fn == test_VirtualAlloc);
    uint32_t args[4] = {0, 0x10000u, 0x3000u, 4u};
    CHECK(call_thunk(fn, 4, 4, args) == 0x12340000u && g_virtualalloc_calls == 1);
    fn = resolve_fn("WS2_32.dll", "", 9, 1);
    CHECK(fn == test_htons);
    args[0] = 0x1234u;
    CHECK(call_thunk(fn, 1, 1, args) == 0x3412u);

    /* Not in the inventory: 0 and one UNSUPPORTED line. */
    capture_begin();
    uint32_t va = win32_resolve_import("KERNEL32.dll", "NotARealExport", 0, 0);
    char *out = capture_end();
    CHECK(va == 0);
    CHECK(count_lines(out, "UNSUPPORTED KERNEL32.dll!NotARealExport: not in the import inventory") == 1);
}

static void test_stubs(void)
{
    /* stdcall stub: D3DXMatrixMultiply pops 12 bytes. Two calls, one line. */
    recomp_func_t fn = resolve_fn("d3dx9_41.dll", "D3DXMatrixMultiply", 0, 0);
    CHECK(fn != NULL);
    uint32_t args[3] = {1, 2, 3};
    capture_begin();
    uint32_t r1 = call_thunk(fn, 3, 3, args);
    uint32_t r2 = call_thunk(fn, 3, 3, args);
    char *out = capture_end();
    CHECK(r1 == 0 && r2 == 0);
    CHECK(count_lines(out, "UNSUPPORTED d3dx9_41.dll!D3DXMatrixMultiply: not implemented "
                           "(guest return 0xDEAD0001)\n") == 1);
    CHECK(count_lines(out, "UNSUPPORTED ") == 1);

    /* cdecl stub: pops only the return address. */
    fn = resolve_fn("X3DAudio1_6.dll", "X3DAudioInitialize", 0, 0);
    capture_begin();
    CHECK(call_thunk(fn, 0, 3, args) == 0);
    out = capture_end();
    CHECK(count_lines(out, "UNSUPPORTED X3DAudio1_6.dll!X3DAudioInitialize: not implemented") == 1);

    /* by-ordinal stub with no slice entry: XINPUT1_3 #2 (XInputGetState, 8 bytes). */
    fn = resolve_fn("XINPUT1_3.dll", "", 2, 1);
    capture_begin();
    CHECK(call_thunk(fn, 2, 2, args) == 0);
    out = capture_end();
    CHECK(count_lines(out, "UNSUPPORTED XINPUT1_3.dll!XInputGetState: not implemented") == 1);
}

/* MS_VC_EXCEPTION (SetThreadName) returns normally and logs the name once. */
static void test_thread_name(void)
{
    recomp_func_t fn = core("RaiseException");
    uint32_t info = guest_string("THREADNAME_INFO_");
    uint32_t name = guest_string("Render");
    MEM32(info) = 0x1000u, MEM32(info + 4) = name, MEM32(info + 8) = 0x1234u, MEM32(info + 12) = 0;
    uint32_t args[4] = {0x406D1388u, 0, 4, info};
    setenv("RECOMP_STARTUP_VERBOSE", "1", 1);
    capture_begin();
    call_thunk(fn, 4, 4, args);
    call_thunk(fn, 4, 4, args);
    char *out = capture_end();
    CHECK(count_lines(out, "SetThreadName 0x00001234 \"Render\"\n") == 1);
    CHECK(strstr(out, "UNSUPPORTED") == NULL);

    /* dwThreadID -1: the calling thread, so the host thread gets the name. */
    MEM32(info + 4) = guest_string("Worker"), MEM32(info + 8) = 0xFFFFFFFFu;
    capture_begin();
    call_thunk(fn, 4, 4, args);
    out = capture_end();
    unsetenv("RECOMP_STARTUP_VERBOSE");
    CHECK(count_lines(out, "SetThreadName 0xFFFFFFFF \"Worker\"\n") == 1);
    char host[64] = "";
    pthread_getname_np(pthread_self(), host, sizeof host);
    CHECK(strcmp(host, "Worker") == 0);
}

/* RaiseException / RtlUnwind report and abort. Run fn in a child with
 * fs base fs_base; its stderr must start with the UNSUPPORTED line, hold
 * exactly one, and contain every string in want (NULL-terminated). */
static void test_seh_abort(const char *name, const uint32_t *args, uint32_t fs_base,
                           const char *const *want)
{
    recomp_func_t fn = core(name);
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        g_fs_base = fs_base;
        call_thunk(fn, 4, 4, args);
        _exit(0);
    }
    close(pipefd[1]);
    char buf[4096];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof buf - 1 && (n = read(pipefd[0], buf + used, sizeof buf - 1 - used)) > 0)
        used += (size_t)n;
    buf[used] = '\0';
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(CHILD_ABORTED(st));
    CHECK(strncmp(buf, "UNSUPPORTED KERNEL32.dll!", 25) == 0);
    CHECK(count_lines(buf, "UNSUPPORTED ") == 1);
    for (; *want != NULL; want++) {
        if (strstr(buf, *want) == NULL) {
            fprintf(stderr, "missing: %s\nin: %s", *want, buf);
            g_fail = 1;
        }
    }
}

/* Guest structures for the exception tests. */
#define EH_AREA 0x00FD0000u

static void test_exceptions(void)
{
    /* TIB with a two-record SEH chain. */
    uint32_t tib = EH_AREA, rec0 = EH_AREA + 0x100, rec1 = EH_AREA + 0x110;
    MEM32(tib) = rec0;
    MEM32(rec0) = rec1, MEM32(rec0 + 4) = 0x00501100u;
    MEM32(rec1) = 0xFFFFFFFFu, MEM32(rec1 + 4) = 0x00501200u;

    /* ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor. */
    uint32_t ti = EH_AREA + 0x200, cta = EH_AREA + 0x220, ct = EH_AREA + 0x240;
    uint32_t td = EH_AREA + 0x280, params = EH_AREA + 0x300;
    MEM32(ti + 12) = cta;
    MEM32(cta) = 1, MEM32(cta + 4) = ct;
    MEM32(ct) = 0, MEM32(ct + 4) = td;
    MEM32(td) = 0x00C0FFEEu, MEM32(td + 4) = 0;
    strcpy((char *)guest_ptr(td + 8), ".?AVbad_alloc@std@@");
    MEM32(params) = 0x19930520u, MEM32(params + 4) = 0x00EE0000u, MEM32(params + 8) = ti;

    const char *chain[] = {
        "SEH chain: [0] record 0x00FD0100 handler 0x00501100\n",
        "SEH chain: [1] record 0x00FD0110 handler 0x00501200\n",
        "SEH chain: end\n",
    };
    uint32_t throw_args[4] = {0xE06D7363u, 1, 3, params};
    const char *want_throw[] = {
        "UNSUPPORTED KERNEL32.dll!RaiseException: SEH dispatch not implemented (code "
        "0xE06D7363, flags 0x00000001, nargs 3, C++ throw .?AVbad_alloc@std@@, guest return "
        "0xDEAD0001)\n",
        chain[0], chain[1], chain[2], NULL};
    test_seh_abort("RaiseException", throw_args, tib, want_throw);

    /* Rethrow: ThrowInfo 0. */
    uint32_t params_re = EH_AREA + 0x340;
    MEM32(params_re) = 0x19930520u, MEM32(params_re + 4) = 0, MEM32(params_re + 8) = 0;
    uint32_t rethrow_args[4] = {0xE06D7363u, 1, 3, params_re};
    const char *want_rethrow[] = {"nargs 3, C++ rethrow, guest return 0xDEAD0001)\n", chain[2],
                                  NULL};
    test_seh_abort("RaiseException", rethrow_args, tib, want_rethrow);

    /* Any other code: its parameters. */
    uint32_t params_av = EH_AREA + 0x380;
    MEM32(params_av) = 0, MEM32(params_av + 4) = 0x12345678u;
    uint32_t av_args[4] = {0xC0000005u, 0, 2, params_av};
    const char *want_av[] = {"(code 0xC0000005, flags 0x00000000, nargs 2, args [0x00000000 "
                             "0x12345678], guest return 0xDEAD0001)\n",
                             chain[0], NULL};
    test_seh_abort("RaiseException", av_args, tib, want_av);

    /* RtlUnwind with the C++ exception record; then without a record or TIB. */
    uint32_t rec = EH_AREA + 0x400;
    MEM32(rec) = 0xE06D7363u, MEM32(rec + 4) = 2, MEM32(rec + 8) = 0;
    MEM32(rec + 12) = 0x00501300u, MEM32(rec + 16) = 3;
    MEM32(rec + 20) = 0x19930520u, MEM32(rec + 24) = 0x00EE0000u, MEM32(rec + 28) = ti;
    uint32_t unwind_args[4] = {rec0, 0x00401000u, rec, 0};
    const char *want_unwind[] = {
        "UNSUPPORTED KERNEL32.dll!RtlUnwind: SEH dispatch not implemented (code 0xE06D7363, "
        "flags 0x00000002, address 0x00501300, nargs 3, C++ throw .?AVbad_alloc@std@@, target "
        "frame 0x00FD0100, target ip 0x00401000, guest return 0xDEAD0001)\n",
        chain[0], chain[1], chain[2], NULL};
    test_seh_abort("RtlUnwind", unwind_args, tib, want_unwind);
    uint32_t unwind_null[4] = {0, 0, 0, 0};
    const char *want_null[] = {"(exception record 0x00000000, target frame 0x00000000",
                               "SEH chain: no TIB (fs base 0)\n", NULL};
    test_seh_abort("RtlUnwind", unwind_null, 0, want_null);
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    test_handles();
    test_last_error();
    test_modules();
    test_encode_pointer();
    test_inventory_name();
    test_indy_resolution();
    test_resolver();
    test_stubs();
    test_thread_name();

    test_exceptions();

    if (g_fail) {
        fprintf(stderr, "test_win32_core: FAILED\n");
        return 1;
    }
    printf("test_win32_core: all checks passed\n");
    return 0;
}
