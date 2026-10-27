/* Win32 mem slice test: every one of the 57 thunks pops 4 + 4 * nargs and
 * keeps ebx/esi/edi/ebp, plus the behaviour of the heap, virtual memory,
 * environment, code page, string, locale and version thunks. Guest stack as
 * in the engine's THUNK_SLICES.md. Links with win32_mem.c and the engine's
 * guest_mem.c, guest_heap.c, dlmalloc.c, guest_call.c, host_funcs.c and
 * recomp_glue.c (-DRECOMP_GLUE_NO_MAIN); not with win32_core.c, whose last
 * error is replaced by the doubles below. */
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
#include "guest_heap.h"
#include "host_funcs.h"
#include "win32_thunk.h"

/* Generated dispatch; this test has no lifted code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* Doubles for the core slice. */
static __thread uint32_t t_last_error;
uint32_t w32_last_error(void)
{
    return t_last_error;
}
void w32_set_last_error(uint32_t code)
{
    t_last_error = code;
}

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

/* ---- Calling every thunk ------------------------------------------------ */

/* nargs = "Args bytes" / 4 in notes/slice_owners.md. */
static const struct {
    const char *name;
    int nargs;
} k_nargs[] = {
    {"CompareStringA", 6},       {"CompareStringW", 6},
    {"CreatePipe", 4},           {"CreateProcessA", 10},
    {"DebugBreak", 0},           {"EnumSystemLocalesA", 2},
    {"ExitProcess", 1},          {"ExpandEnvironmentStringsA", 3},
    {"FatalAppExitA", 2},        {"FreeEnvironmentStringsA", 1},
    {"FreeEnvironmentStringsW", 1}, {"GetACP", 0},
    {"GetCPInfo", 2},            {"GetCommandLineA", 0},
    {"GetCurrentProcess", 0},    {"GetCurrentProcessId", 0},
    {"GetDateFormatA", 6},       {"GetEnvironmentStrings", 0},
    {"GetEnvironmentStringsW", 0}, {"GetExitCodeProcess", 2},
    {"GetLocaleInfoA", 4},       {"GetLocaleInfoW", 4},
    {"GetOEMCP", 0},             {"GetProcessHeap", 0},
    {"GetStartupInfoA", 1},      {"GetStringTypeA", 5},
    {"GetStringTypeW", 4},       {"GetTimeFormatA", 6},
    {"GetUserDefaultLCID", 0},   {"GetUserDefaultLangID", 0},
    {"GetUserGeoID", 1},         {"GetVersionExA", 1},
    {"HeapAlloc", 3},            {"HeapCreate", 3},
    {"HeapDestroy", 1},          {"HeapFree", 3},
    {"HeapReAlloc", 4},          {"HeapSize", 3},
    {"IsDebuggerPresent", 0},    {"IsProcessorFeaturePresent", 1},
    {"IsValidCodePage", 1},
    {"IsValidLocale", 2},        {"LCMapStringA", 6},
    {"LCMapStringW", 6},         {"MultiByteToWideChar", 6},
    {"OutputDebugStringA", 1},   {"OutputDebugStringW", 1},
    {"SetEnvironmentVariableA", 2}, {"SetUnhandledExceptionFilter", 1},
    {"TerminateProcess", 2},     {"UnhandledExceptionFilter", 1},
    {"VerSetConditionMask", 4},  {"VerifyVersionInfoA", 4},
    {"VirtualAlloc", 4},         {"VirtualFree", 3},
    {"VirtualQuery", 3},         {"WideCharToMultiByte", 8},
};
#define N_THUNKS (sizeof k_nargs / sizeof k_nargs[0])
static int g_called[N_THUNKS];

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define RET_ADDR 0xCAFEF00Du

static size_t thunk_index(const char *name)
{
    for (size_t i = 0; i < N_THUNKS; i++)
        if (strcmp(k_nargs[i].name, name) == 0)
            return i;
    fprintf(stderr, "no nargs row for %s\n", name);
    abort();
}

static recomp_func_t thunk(const char *name)
{
    for (const win32_export_t *e = win32_exports_mem; e->name != NULL; e++)
        if (strcmp(e->dll, "KERNEL32.dll") == 0 && strcmp(e->name, name) == 0)
            return e->fn;
    fprintf(stderr, "win32_exports_mem has no %s\n", name);
    abort();
}

/* Call a thunk with its inventory argument count and check the stdcall pop
 * and the callee-saved registers. args holds at least nargs values. */
static uint32_t call(const char *name, const uint32_t *args)
{
    size_t idx = thunk_index(name);
    int n = k_nargs[idx].nargs;
    uint32_t sp = STACK_HI - 0x1000u - 4u * (uint32_t)n;
    MEM32(sp) = RET_ADDR;
    for (int i = 0; i < n; i++)
        MEM32(sp + 4u + 4u * (uint32_t)i) = args[i];
    g_esp = sp;
    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
    thunk(name)();
    if (g_esp != sp + 4u + 4u * (uint32_t)n) {
        fprintf(stderr, "FAIL %s: esp rose by %u, want %u\n", name, g_esp - sp, 4u + 4u * (uint32_t)n);
        g_fail = 1;
    }
    if (g_ebx != 0x11111111u || g_esi != 0x22222222u || g_edi != 0x33333333u || g_ebp != 0x44444444u) {
        fprintf(stderr, "FAIL %s: callee-saved register changed\n", name);
        g_fail = 1;
    }
    g_called[idx] = 1;
    return g_eax;
}

#define A(...) ((const uint32_t[]){__VA_ARGS__})

/* ---- Guest data helpers ------------------------------------------------ */

static uint32_t gbuf(uint32_t n)
{
    uint32_t a = guest_malloc(n);
    CHECK(a != 0);
    memset(guest_ptr(a), 0xEE, n);
    return a;
}

static uint32_t gstr(const char *s)
{
    uint32_t a = gbuf((uint32_t)strlen(s) + 1);
    strcpy(guest_ptr(a), s);
    return a;
}

static uint32_t gwstr(const uint16_t *s, uint32_t n_with_nul)
{
    uint32_t a = gbuf(2 * n_with_nul);
    memcpy(guest_ptr(a), s, 2 * n_with_nul);
    return a;
}

static const char *cstr(uint32_t a)
{
    return guest_ptr(a);
}

/* ---- Heap -------------------------------------------------------------- */

static void test_heap(void)
{
    uint32_t h = call("GetProcessHeap", NULL);
    CHECK(h != 0);
    CHECK(call("GetProcessHeap", NULL) == h);

    /* Dirty a block, free it, and check that a zeroing allocation of the
     * same size comes back zeroed. */
    uint32_t d = call("HeapAlloc", A(h, 0, 100));
    CHECK(d != 0 && d % 16 == 0);
    memset(guest_ptr(d), 0xAB, 100);
    CHECK(call("HeapFree", A(h, 0, d)) == W32_TRUE);
    uint32_t p = call("HeapAlloc", A(h, 0x8, 100));
    CHECK(p != 0 && p % 16 == 0);
    int zero = 1;
    for (uint32_t i = 0; i < 100; i++)
        zero &= MEM8(p + i) == 0;
    CHECK(zero);
    CHECK(call("HeapSize", A(h, 0, p)) == 100);

    for (uint32_t i = 0; i < 100; i++)
        MEM8(p + i) = (uint8_t)i;
    CHECK(call("HeapReAlloc", A(h, 0x10, p, 200)) == 0); /* in place only: cannot grow */
    uint32_t q = call("HeapReAlloc", A(h, 0x8, p, 5000));
    CHECK(q != 0 && q % 16 == 0);
    int same = 1, tail_zero = 1;
    for (uint32_t i = 0; i < 100; i++)
        same &= MEM8(q + i) == (uint8_t)i;
    for (uint32_t i = 100; i < 5000; i++)
        tail_zero &= MEM8(q + i) == 0;
    CHECK(same);
    CHECK(tail_zero);
    CHECK(call("HeapSize", A(h, 0, q)) == 5000);
    CHECK(call("HeapReAlloc", A(h, 0x10, q, 40)) == q);
    CHECK(call("HeapSize", A(h, 0, q)) == 40);
    CHECK(call("HeapFree", A(h, 0, q)) == W32_TRUE);
    CHECK(call("HeapFree", A(h, 0, 0)) == W32_TRUE);

    uint32_t h1 = call("HeapCreate", A(0, 0x1000, 0));
    uint32_t h2 = call("HeapCreate", A(0, 0x1000, 0));
    CHECK(h1 != 0 && h2 != 0 && h1 != h2 && h1 != h);
    uint32_t b = call("HeapAlloc", A(h1, 0, 32));
    CHECK(b != 0 && call("HeapSize", A(h1, 0, b)) == 32);
    CHECK(call("HeapFree", A(h1, 0, b)) == W32_TRUE);
    CHECK(call("HeapDestroy", A(h1)) == W32_TRUE);
    CHECK(call("HeapDestroy", A(h2)) == W32_TRUE);
}

/* ---- Virtual memory ---------------------------------------------------- */

struct mbi {
    uint32_t base, alloc_base, alloc_prot, size, state, prot, type;
};

static struct mbi query(uint32_t addr)
{
    uint32_t out = gbuf(28);
    CHECK(call("VirtualQuery", A(addr, out, 28)) == 28);
    struct mbi m;
    memcpy(&m, guest_ptr(out), sizeof m);
    guest_free(out);
    return m;
}

static void test_virtual(void)
{
    const uint32_t RES = 0x2000, COM = 0x1000, RW = 0x04;
    uint32_t base = call("VirtualAlloc", A(0, 0x30000, RES, RW));
    CHECK(base != 0 && base % 0x10000 == 0 && base >= 0x02000000u && base < 0x20000000u);
    struct mbi m = query(base);
    CHECK(m.base == base && m.alloc_base == base && m.alloc_prot == RW && m.size == 0x30000 &&
          m.state == RES && m.prot == 0 && m.type == 0x20000);

    CHECK(call("VirtualAlloc", A(base + 0x5000, 0x2000, COM, RW)) == base + 0x5000);
    CHECK(MEM32(base + 0x5000) == 0 && MEM32(base + 0x6FFC) == 0);
    MEM32(base + 0x5000) = 0x12345678u;
    MEM32(base + 0x6FFC) = 0x9ABCDEF0u;
    m = query(base + 0x5800);
    CHECK(m.base == base + 0x5000 && m.alloc_base == base && m.size == 0x2000 && m.state == COM &&
          m.prot == RW && m.type == 0x20000);
    m = query(base);
    CHECK(m.state == RES && m.size == 0x5000);
    m = query(base + 0x7000);
    CHECK(m.state == RES && m.size == 0x29000);

    /* Taken base, outside the arena, too large. */
    t_last_error = 0;
    CHECK(call("VirtualAlloc", A(base, 0x1000, RES, RW)) == 0 && t_last_error == 87);
    t_last_error = 0;
    CHECK(call("VirtualAlloc", A(0x00400000u, 0x1000, RES | COM, RW)) == 0 && t_last_error == 87);
    t_last_error = 0;
    CHECK(call("VirtualAlloc", A(0, 0x40000000u, RES, RW)) == 0 && t_last_error == 8);
    t_last_error = 0;
    CHECK(call("VirtualAlloc", A(0, 0x1000, RES, 0x104)) == 0 && t_last_error == 87); /* PAGE_GUARD */

    /* Decommit one page and commit it again: it reads zero, its neighbour
     * keeps its data. */
    CHECK(call("VirtualFree", A(base + 0x5000, 0x1000, 0x4000)) == W32_TRUE);
    CHECK(query(base + 0x5000).state == RES);
    CHECK(call("VirtualAlloc", A(base + 0x5000, 0x1000, COM, RW)) == base + 0x5000);
    CHECK(MEM32(base + 0x5000) == 0);
    CHECK(MEM32(base + 0x6FFC) == 0x9ABCDEF0u);

    CHECK(call("VirtualFree", A(base + 0x1000, 0, 0x8000)) == W32_FALSE); /* not the base */
    CHECK(call("VirtualFree", A(base, 0, 0x8000)) == W32_TRUE);
    m = query(base);
    CHECK(m.state == 0x10000 && m.prot == 0x01 && m.alloc_base == 0);

    /* A requested free base is honoured; reserve+commit at once. */
    uint32_t want = 0x10000000u;
    CHECK(call("VirtualAlloc", A(want + 0x1234, 0x1000, RES | COM, RW)) == want);
    m = query(want);
    CHECK(m.state == COM && m.size == 0x3000); /* 0x10000000 up to the page holding 0x10002233 */
    MEM32(want + 0x2FFC) = 7;
    CHECK(call("VirtualFree", A(want, 0, 0x8000)) == W32_TRUE);

    /* NULL address with MEM_COMMIT alone reserves too. */
    uint32_t c = call("VirtualAlloc", A(0, 0x1800, COM, RW));
    CHECK(c != 0 && MEM32(c + 0x17FC) == 0);
    CHECK(call("VirtualFree", A(c, 0, 0x8000)) == W32_TRUE);

    /* Outside the arena: the heap is one committed region. */
    m = query(GUEST_HEAP_BASE + 0x100);
    CHECK(m.state == COM && m.alloc_base == GUEST_HEAP_BASE);
}

/* ---- Environment ------------------------------------------------------- */

static void test_environment(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char ncpu[64];
    snprintf(ncpu, sizeof ncpu, "NUMBER_OF_PROCESSORS=%ld", cpus > 8 ? 8 : cpus);
    const char *want[] = {
        "PATH=C:\\Windows\\system32;C:\\Windows",
        "SystemRoot=C:\\Windows",
        "TEMP=C:\\Users\\Player\\AppData\\Local\\Temp",
        "TMP=C:\\Users\\Player\\AppData\\Local\\Temp",
        "USERPROFILE=C:\\Users\\Player",
        "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
        "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
        "COMPUTERNAME=MAC",
        "USERNAME=Player",
        "OS=Windows_NT",
        ncpu,
    };
    const size_t n_want = sizeof want / sizeof want[0];

    uint32_t blk = call("GetEnvironmentStrings", NULL);
    CHECK(blk != 0);
    uint32_t at = blk;
    size_t i = 0;
    for (; MEM8(at) != 0; i++) {
        CHECK(i < n_want && strcmp(cstr(at), want[i]) == 0);
        at += (uint32_t)strlen(cstr(at)) + 1;
    }
    CHECK(i == n_want);
    CHECK(MEM8(at) == 0 && MEM8(at - 1) == 0); /* double NUL */
    uint32_t len_a = at - blk + 1;

    uint32_t wblk = call("GetEnvironmentStringsW", NULL);
    CHECK(wblk != 0);
    int same = 1;
    for (uint32_t k = 0; k < len_a; k++)
        same &= MEM16(wblk + 2 * k) == MEM8(blk + k);
    CHECK(same);
    CHECK(call("FreeEnvironmentStringsA", A(blk)) == W32_TRUE);
    CHECK(call("FreeEnvironmentStringsW", A(wblk)) == W32_TRUE);

    CHECK(call("SetEnvironmentVariableA", A(gstr("FOO"), gstr("bar"))) == W32_TRUE);
    blk = call("GetEnvironmentStrings", NULL);
    int found = 0;
    for (at = blk; MEM8(at) != 0; at += (uint32_t)strlen(cstr(at)) + 1) {
        found |= strcmp(cstr(at), "FOO=bar") == 0;
        CHECK(strncmp(cstr(at), "HOME=", 5) != 0); /* never the host environment */
    }
    CHECK(found);
    call("FreeEnvironmentStringsA", A(blk));

    uint32_t src = gstr("%foo%-%SystemRoot%-%NOPE%-50%");
    const char *expect = "bar-C:\\Windows-%NOPE%-50%";
    uint32_t need = (uint32_t)strlen(expect) + 1;
    CHECK(call("ExpandEnvironmentStringsA", A(src, 0, 0)) == need);
    uint32_t dst = gbuf(64);
    CHECK(call("ExpandEnvironmentStringsA", A(src, dst, 64)) == need);
    CHECK(strcmp(cstr(dst), expect) == 0);

    CHECK(call("SetEnvironmentVariableA", A(gstr("FOO"), 0)) == W32_TRUE);
    CHECK(call("ExpandEnvironmentStringsA", A(gstr("%FOO%"), dst, 64)) == 6);
    CHECK(strcmp(cstr(dst), "%FOO%") == 0);
}

/* ---- Code pages -------------------------------------------------------- */

static void test_codepages(void)
{
    CHECK(call("GetACP", NULL) == 1252);
    CHECK(call("GetOEMCP", NULL) == 437);
    CHECK(call("IsValidCodePage", A(1252)) == W32_TRUE);
    CHECK(call("IsValidCodePage", A(437)) == W32_TRUE);
    CHECK(call("IsValidCodePage", A(65001)) == W32_TRUE);
    CHECK(call("IsValidCodePage", A(932)) == W32_FALSE);
    uint32_t info = gbuf(20);
    CHECK(call("GetCPInfo", A(1252, info)) == W32_TRUE);
    CHECK(MEM32(info) == 1 && MEM8(info + 4) == '?' && MEM8(info + 5) == 0 && MEM8(info + 6) == 0);
    CHECK(call("GetCPInfo", A(437, info)) == W32_TRUE && MEM32(info) == 1);
    CHECK(call("GetCPInfo", A(932, info)) == W32_FALSE);

    /* 1252, NUL-terminated (-1 includes the NUL). */
    uint32_t s = gstr("A\x80\xE9");
    uint32_t w = gbuf(32);
    CHECK(call("MultiByteToWideChar", A(1252, 0, s, (uint32_t)-1, 0, 0)) == 4);
    t_last_error = 0;
    CHECK(call("MultiByteToWideChar", A(0, 0, s, (uint32_t)-1, w, 3)) == 0 && t_last_error == 122);
    CHECK(call("MultiByteToWideChar", A(0, 0, s, (uint32_t)-1, w, 16)) == 4);
    CHECK(MEM16(w) == 'A' && MEM16(w + 2) == 0x20AC && MEM16(w + 4) == 0xE9 && MEM16(w + 6) == 0);
    uint32_t back = gbuf(32);
    CHECK(call("WideCharToMultiByte", A(1252, 0, w, (uint32_t)-1, 0, 0, 0, 0)) == 4);
    CHECK(call("WideCharToMultiByte", A(1252, 0, w, (uint32_t)-1, back, 32, 0, 0)) == 4);
    CHECK(memcmp(guest_ptr(back), "A\x80\xE9", 4) == 0);

    /* Explicit length: no NUL is added. */
    MEM16(w + 4) = 0xEEEE;
    CHECK(call("MultiByteToWideChar", A(1252, 0, s, 2, w, 16)) == 2);
    CHECK(MEM16(w + 4) == 0xEEEE);

    /* Unmappable character: default char and lpUsedDefaultChar. */
    static const uint16_t cjk[] = {'x', 0x4E2D, 0};
    uint32_t wc = gwstr(cjk, 3), used = gbuf(4);
    CHECK(call("WideCharToMultiByte", A(1252, 0, wc, (uint32_t)-1, back, 32, 0, used)) == 3);
    CHECK(strcmp(cstr(back), "x?") == 0 && MEM32(used) == W32_TRUE);

    /* CP437 has its own upper half. */
    uint32_t oem = gstr("\x82\xB0");
    CHECK(call("MultiByteToWideChar", A(437, 0, oem, (uint32_t)-1, w, 16)) == 3);
    CHECK(MEM16(w) == 0xE9 && MEM16(w + 2) == 0x2591);
    CHECK(call("WideCharToMultiByte", A(1, 0, w, (uint32_t)-1, back, 32, 0, 0)) == 3);
    CHECK(memcmp(guest_ptr(back), "\x82\xB0", 3) == 0);

    /* UTF-8 with a 2-, 3- and 4-byte sequence. */
    const char *u8 = "h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    uint32_t su = gstr(u8);
    CHECK(call("MultiByteToWideChar", A(65001, 0, su, (uint32_t)-1, 0, 0)) == 6);
    CHECK(call("MultiByteToWideChar", A(65001, 0, su, (uint32_t)-1, w, 16)) == 6);
    CHECK(MEM16(w) == 'h' && MEM16(w + 2) == 0xE9 && MEM16(w + 4) == 0x20AC &&
          MEM16(w + 6) == 0xD83D && MEM16(w + 8) == 0xDE00 && MEM16(w + 10) == 0);
    CHECK(call("WideCharToMultiByte", A(65001, 0, w, (uint32_t)-1, 0, 0, 0, 0)) == 11);
    CHECK(call("WideCharToMultiByte", A(65001, 0, w, (uint32_t)-1, back, 32, 0, 0)) == 11);
    CHECK(strcmp(cstr(back), u8) == 0);
    CHECK(call("MultiByteToWideChar", A(65001, 0, su, 3, w, 16)) == 2);
    t_last_error = 0;
    CHECK(call("WideCharToMultiByte", A(65001, 0, w, 2, back, 32, 0, used)) == 0 && t_last_error == 87);
    t_last_error = 0;
    CHECK(call("MultiByteToWideChar", A(65001, 0x8, gstr("\xC3"), 1, w, 16)) == 0 &&
          t_last_error == 1113);
    CHECK(call("MultiByteToWideChar", A(65001, 0, gstr("\xC3"), 1, w, 16)) == 1 && MEM16(w) == 0xFFFD);
    t_last_error = 0;
    CHECK(call("MultiByteToWideChar", A(932, 0, s, (uint32_t)-1, w, 16)) == 0 && t_last_error == 87);
}

/* ---- Strings and locale ------------------------------------------------ */

static void test_strings(void)
{
    uint32_t a = gstr("Hello"), b = gstr("hELLO");
    CHECK(call("CompareStringA", A(0x409, 1, a, (uint32_t)-1, b, (uint32_t)-1)) == 2);
    CHECK(call("CompareStringA", A(0x409, 0, a, (uint32_t)-1, b, (uint32_t)-1)) == 1);
    CHECK(call("CompareStringA", A(0x409, 1, gstr("abc"), 3, gstr("ABD"), 3)) == 1);
    CHECK(call("CompareStringA", A(0x409, 0, gstr("abcd"), (uint32_t)-1, gstr("abc"), (uint32_t)-1)) == 3);
    CHECK(call("CompareStringA", A(0x409, 1, gstr("\xE9t\xE9"), (uint32_t)-1, gstr("\xC9T\xC9"),
                                   (uint32_t)-1)) == 2);
    static const uint16_t w1[] = {'a', 0xE9, 0}, w2[] = {'A', 0xC9, 0};
    uint32_t g1 = gwstr(w1, 3), g2 = gwstr(w2, 3);
    CHECK(call("CompareStringW", A(0x409, 1, g1, (uint32_t)-1, g2, (uint32_t)-1)) == 2);
    CHECK(call("CompareStringW", A(0x409, 0, g1, (uint32_t)-1, g2, (uint32_t)-1)) == 3);

    uint32_t src = gstr("abc\xE9\xFF"), dst = gbuf(16);
    CHECK(call("LCMapStringA", A(0x409, 0x200, src, (uint32_t)-1, 0, 0)) == 6);
    CHECK(call("LCMapStringA", A(0x409, 0x200, src, (uint32_t)-1, dst, 16)) == 6);
    CHECK(strcmp(cstr(dst), "ABC\xC9\xFF") == 0);
    CHECK(call("LCMapStringA", A(0x409, 0x100, gstr("ABC\xC9"), 4, dst, 16)) == 4);
    CHECK(memcmp(guest_ptr(dst), "abc\xE9", 4) == 0);
    CHECK(call("LCMapStringW", A(0x409, 0x200, g1, (uint32_t)-1, dst, 8)) == 3);
    CHECK(MEM16(dst) == 'A' && MEM16(dst + 2) == 0xC9 && MEM16(dst + 4) == 0);
    t_last_error = 0;
    CHECK(call("LCMapStringW", A(0x409, 0x400, g1, (uint32_t)-1, dst, 8)) == 0 && t_last_error == 87);

    uint32_t types = gbuf(16);
    CHECK(call("GetStringTypeA", A(0x409, 1, gstr("Az9 \t."), 6, types)) == W32_TRUE);
    CHECK(MEM16(types) == 0x381);      /* A: upper alpha xdigit */
    CHECK(MEM16(types + 2) == 0x302);  /* z: lower alpha */
    CHECK(MEM16(types + 4) == 0x284);  /* 9: digit xdigit */
    CHECK(MEM16(types + 6) == 0x248);  /* space */
    CHECK(MEM16(types + 8) == 0x268);  /* tab */
    CHECK(MEM16(types + 10) == 0x210); /* punct */
    CHECK(call("GetStringTypeW", A(1, g2, (uint32_t)-1, types)) == W32_TRUE);
    CHECK(MEM16(types) == 0x381 && MEM16(types + 2) == 0x301 && MEM16(types + 4) == 0x220);

    uint32_t buf = gbuf(64);
    CHECK(call("GetLocaleInfoA", A(0x409, 0x0E, buf, 64)) == 2 && strcmp(cstr(buf), ".") == 0);
    CHECK(call("GetLocaleInfoA", A(0x409, 0x1004, buf, 64)) == 5 && strcmp(cstr(buf), "1252") == 0);
    CHECK(call("GetLocaleInfoA", A(0x409, 0x2A, buf, 64)) == 7 && strcmp(cstr(buf), "Monday") == 0);
    CHECK(call("GetLocaleInfoA", A(0x409, 0x30 | 0x80000000u, buf, 64)) == 7 &&
          strcmp(cstr(buf), "Sunday") == 0);
    CHECK(call("GetLocaleInfoW", A(0, 0x01, 0, 0)) == 5);
    CHECK(call("GetLocaleInfoW", A(0x409, 0x01, buf, 8)) == 5);
    CHECK(MEM16(buf) == '0' && MEM16(buf + 2) == '4' && MEM16(buf + 6) == '9' && MEM16(buf + 8) == 0);
    t_last_error = 0;
    CHECK(call("GetLocaleInfoA", A(0x409, 0x0E, buf, 1)) == 0 && t_last_error == 122);
    t_last_error = 0;
    CHECK(call("GetLocaleInfoA", A(0x409, 0x7777, buf, 64)) == 0 && t_last_error == 87);
}

static uint32_t g_cb_calls;
static char g_cb_text[16];
static void test_locale_cb(void)
{
    g_cb_calls++;
    snprintf(g_cb_text, sizeof g_cb_text, "%s", cstr(W32_ARG(0)));
    W32_RETURN(1, W32_TRUE);
}

static void test_locale(void)
{
    CHECK(call("GetUserDefaultLangID", NULL) == 1031); /* HPRECOMP_LANGID */
    CHECK(call("GetUserDefaultLCID", NULL) == 1031);
    CHECK(call("IsValidLocale", A(0x409, 2)) == W32_TRUE);
    CHECK(call("IsValidLocale", A(1031, 2)) == W32_TRUE);
    CHECK(call("IsValidLocale", A(0x40C, 2)) == W32_FALSE);
    CHECK(call("EnumSystemLocalesA", A(host_func_va(test_locale_cb), 2)) == W32_TRUE);
    CHECK(g_cb_calls == 1 && strcmp(g_cb_text, "00000409") == 0);
    CHECK(call("GetUserGeoID", A(16)) == 244);

    /* SYSTEMTIME 2026-10-24 (wDayOfWeek deliberately wrong) 13:05:09. */
    uint32_t st = gbuf(16);
    static const uint16_t t[8] = {2026, 10, 0, 24, 13, 5, 9, 0};
    memcpy(guest_ptr(st), t, sizeof t);
    uint32_t out = gbuf(64);
    CHECK(call("GetDateFormatA", A(0x409, 0, st, 0, 0, 0)) == 11);
    CHECK(call("GetDateFormatA", A(0x409, 0, st, 0, out, 64)) == 11 && strcmp(cstr(out), "10/24/2026") == 0);
    CHECK(call("GetDateFormatA", A(0x409, 2, st, 0, out, 64)) != 0 &&
          strcmp(cstr(out), "Saturday, October 24, 2026") == 0);
    CHECK(call("GetDateFormatA", A(0x409, 0, st, gstr("ddd MMM yy 'on' d"), out, 64)) != 0 &&
          strcmp(cstr(out), "Sat Oct 26 on 24") == 0);
    CHECK(call("GetDateFormatA", A(0x409, 0, 0, 0, out, 64)) != 0); /* host clock */
    CHECK(call("GetTimeFormatA", A(0x409, 0, st, 0, out, 64)) == 11 && strcmp(cstr(out), "1:05:09 PM") == 0);
    CHECK(call("GetTimeFormatA", A(0x409, 0, st, gstr("HH:mm"), out, 64)) == 6 &&
          strcmp(cstr(out), "13:05") == 0);
    CHECK(call("GetTimeFormatA", A(0x409, 0, 0, 0, out, 64)) != 0);
    t_last_error = 0;
    CHECK(call("GetTimeFormatA", A(0x409, 0, st, 0, out, 5)) == 0 && t_last_error == 122);
}

/* ---- Process and version ----------------------------------------------- */

static uint64_t cond_mask(uint64_t mask, uint32_t type, uint32_t cond)
{
    uint32_t lo = call("VerSetConditionMask", A((uint32_t)mask, (uint32_t)(mask >> 32), type, cond));
    return (uint64_t)g_edx << 32 | lo;
}

static uint32_t verify(uint32_t major, uint32_t minor, uint32_t sp, uint64_t mask)
{
    uint32_t vi = gbuf(156);
    memset(guest_ptr(vi), 0, 156);
    MEM32(vi) = 156;
    MEM32(vi + 4) = major;
    MEM32(vi + 8) = minor;
    MEM16(vi + 148) = (uint16_t)sp;
    return call("VerifyVersionInfoA", A(vi, 0x02 | 0x01 | 0x20, (uint32_t)mask, (uint32_t)(mask >> 32)));
}

static void test_process(void)
{
    CHECK(strcmp(cstr(call("GetCommandLineA", NULL)), "\"C:\\Game\\RecompTest.exe\" -windowed") == 0);
    uint32_t si = gbuf(68);
    call("GetStartupInfoA", A(si));
    int zero = 1;
    for (uint32_t i = 4; i < 68; i++)
        zero &= MEM8(si + i) == 0;
    CHECK(MEM32(si) == 68 && zero);
    CHECK(call("GetCurrentProcess", NULL) == W32_PSEUDO_CURRENT_PROCESS);
    CHECK(call("GetCurrentProcessId", NULL) == (uint32_t)getpid());
    CHECK(call("IsDebuggerPresent", NULL) == W32_FALSE);
    uint32_t code = gbuf(4);
    CHECK(call("GetExitCodeProcess", A(W32_PSEUDO_CURRENT_PROCESS, code)) == W32_TRUE && MEM32(code) == 259);
    CHECK(call("TerminateProcess", A(0xFF800004u, 3)) == W32_FALSE);

    t_last_error = 0;
    CHECK(call("CreatePipe", A(0, 0, 0, 0)) == W32_FALSE && t_last_error == 5);
    t_last_error = 0;
    CHECK(call("CreateProcessA", A(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) == W32_FALSE && t_last_error == 5);

    CHECK(call("SetUnhandledExceptionFilter", A(0x00401234u)) == 0);
    CHECK(call("SetUnhandledExceptionFilter", A(0x00405678u)) == 0x00401234u);

    call("OutputDebugStringA", A(gstr("hello from A")));
    static const uint16_t ws[] = {'w', 'i', 'd', 'e', ' ', 0xE9, '\n', 0};
    call("OutputDebugStringW", A(gwstr(ws, 8)));

    uint32_t vi = gbuf(156);
    MEM32(vi) = 148;
    CHECK(call("GetVersionExA", A(vi)) == W32_TRUE);
    /* Windows 8: what an unmanifested exe sees on 8.1 and later. */
    CHECK(MEM32(vi + 4) == 6 && MEM32(vi + 8) == 2 && MEM32(vi + 12) == 9200 && MEM32(vi + 16) == 2);
    CHECK(strcmp(cstr(vi + 20), "") == 0);
    CHECK(MEM16(vi + 148) == 0xEEEE); /* past the 148-byte struct: untouched */
    MEM32(vi) = 156;
    CHECK(call("GetVersionExA", A(vi)) == W32_TRUE);
    CHECK(MEM16(vi + 148) == 0 && MEM16(vi + 150) == 0 && MEM8(vi + 154) == 1);
    MEM32(vi) = 100;
    CHECK(call("GetVersionExA", A(vi)) == W32_FALSE);

    /* VER_GREATER_EQUAL (3) on major, minor and service pack major. */
    uint64_t m = cond_mask(0, 0x02, 3);
    CHECK(m == 3u << 3);
    m = cond_mask(m, 0x01, 3);
    m = cond_mask(m, 0x20, 3);
    CHECK(m == ((3u << 3) | 3u | (3u << 15)));
    CHECK(cond_mask(0, 0x80, 1) == 1ull << 21);
    CHECK(verify(5, 1, 2, m) == W32_TRUE);  /* XP SP2 or later */
    CHECK(verify(6, 1, 1, m) == W32_TRUE);  /* 7 SP1 or later */
    CHECK(verify(6, 2, 0, m) == W32_TRUE);  /* 8 or later */
    CHECK(verify(6, 1, 9, m) == W32_TRUE);  /* minor 2 > 1 decides */
    t_last_error = 0;
    CHECK(verify(6, 3, 0, m) == W32_FALSE && t_last_error == 1150); /* 8.1 or later */
    t_last_error = 0;
    CHECK(verify(6, 2, 1, m) == W32_FALSE && t_last_error == 1150);

    /* The fixed CPU of recomp_cpuid: SSE and SSE2 yes, SSE3 and the FDIV
     * errata no. */
    CHECK(call("IsProcessorFeaturePresent", A(10)) == W32_TRUE);
    CHECK(call("IsProcessorFeaturePresent", A(6)) == W32_TRUE);
    CHECK(call("IsProcessorFeaturePresent", A(13)) == W32_FALSE);
    CHECK(call("IsProcessorFeaturePresent", A(0)) == W32_FALSE);
}

/* ---- Thunks that do not return ----------------------------------------- */

/* Run one thunk in a child; returns its wait status and its stderr. */
static int run_child(const char *name, const uint32_t *args, char *err, size_t errsize)
{
    int fds[2];
    if (pipe(fds) != 0)
        abort();
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(fds[1], 2);
        close(fds[0]);
        call(name, args);
        _exit(99);
    }
    close(fds[1]);
    size_t got = 0;
    ssize_t r;
    while (got + 1 < errsize && (r = read(fds[0], err + got, errsize - 1 - got)) > 0)
        got += (size_t)r;
    err[got] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    g_called[thunk_index(name)] = 1;
    return status;
}

static void test_noreturn(void)
{
    char err[512];
    int st = run_child("ExitProcess", A(7), err, sizeof err);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    st = run_child("TerminateProcess", A(W32_PSEUDO_CURRENT_PROCESS, 9), err, sizeof err);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
    st = run_child("DebugBreak", NULL, err, sizeof err);
    CHECK(CHILD_ABORTED(st));
    CHECK(strstr(err, "UNSUPPORTED KERNEL32.dll!DebugBreak: breakpoint (guest return 0xCAFEF00D)") != NULL);
    st = run_child("FatalAppExitA", A(0, gstr("disk on fire")), err, sizeof err);
    CHECK(CHILD_ABORTED(st));
    CHECK(strstr(err, "UNSUPPORTED KERNEL32.dll!FatalAppExitA: disk on fire (guest return 0xCAFEF00D)") != NULL);
    uint32_t rec = gbuf(80), ep = gbuf(8);
    MEM32(rec) = 0xC0000005u;
    MEM32(rec + 12) = 0x00456789u;
    MEM32(ep) = rec;
    MEM32(ep + 4) = 0;
    st = run_child("UnhandledExceptionFilter", A(ep), err, sizeof err);
    CHECK(CHILD_ABORTED(st));
    CHECK(strstr(err, "UNSUPPORTED KERNEL32.dll!UnhandledExceptionFilter: exception 0xC0000005 at "
                      "0x00456789 (guest return 0xCAFEF00D)") != NULL);
}

int main(void)
{
    /* Read once by the slice, before its first call. */
    setenv("HPRECOMP_ARGS", "-windowed", 1);
    setenv("HPRECOMP_LANGID", "1031", 1);

    if (guest_mem_init() != 0)
        return 1;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base;
    if (guest_heap_init() != 0)
        return 1;
    if (mprotect(guest_ptr(STACK_LO), STACK_HI - STACK_LO, PROT_READ | PROT_WRITE) != 0)
        return 1;

    size_t rows = 0;
    for (const win32_export_t *e = win32_exports_mem; e->name != NULL; e++, rows++)
        CHECK(e->fn != NULL && e->ordinal == 0 && thunk_index(e->name) < N_THUNKS);
    CHECK(rows == 57 && N_THUNKS == 57);

    test_heap();
    test_virtual();
    test_environment();
    test_codepages();
    test_strings();
    test_locale();
    test_process();
    test_noreturn();

    for (size_t i = 0; i < N_THUNKS; i++) {
        if (!g_called[i]) {
            fprintf(stderr, "FAIL: %s never called\n", k_nargs[i].name);
            g_fail = 1;
        }
    }
    printf("%s: %zu thunks, %zu exported\n", g_fail ? "FAIL" : "PASS", N_THUNKS, rows);
    return g_fail;
}
