/* Win32 net slice test: the 22 WS2_32 thunks of win32_net.c, found through
 * win32_exports_net by ordinal. Guest stack as in the engine's
 * THUNK_SLICES.md. Does not link win32_core.c: the last error is a double
 * below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "win32_thunk.h"

/* Generated dispatch; this test has no lifted code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* ---- Core doubles ------------------------------------------------------ */

static _Thread_local uint32_t t_last_error;
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

/* ---- Guest stack ------------------------------------------------------- */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define DATA_AREA 0x00FF8000u
#define RET_ADDR 0xDEAD0001u

/* Push args right to left and the return address, call fn, check that it
 * popped 4 + 4*nargs bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call_thunk(recomp_func_t fn, unsigned nargs, const uint32_t *args)
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
    CHECK(g_esp == start + 4u + 4u * nargs);
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBEBEBEBEu);
    return g_eax;
}

static recomp_func_t ord(uint16_t n)
{
    for (const win32_export_t *e = win32_exports_net; e->dll != NULL; e++)
        if (e->ordinal == n)
            return e->fn;
    fprintf(stderr, "FAIL: no entry for WS2_32 ordinal %u\n", n);
    exit(1);
}

#define CALL(o, ...)                                                       \
    call_thunk(ord(o), sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t), \
               (const uint32_t[]){__VA_ARGS__})
#define CALL0(o) call_thunk(ord(o), 0, NULL)

static uint32_t guest_string(uint32_t a, const char *s)
{
    strcpy((char *)guest_ptr(a), s);
    return a;
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

static int count(const char *text, const char *s)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    return n;
}

/* ---- Tests ------------------------------------------------------------- */

/* ordinal -> nargs, from notes/win32_imports.md ("Args bytes popped" / 4). */
static const struct {
    uint16_t ordinal;
    unsigned nargs;
} k_rows[] = {
    {1, 3},  {2, 3},  {3, 1},  {4, 3},  {8, 1},   {9, 1},   {10, 3},  {11, 1},
    {12, 1}, {13, 2}, {15, 1}, {16, 4}, {17, 6},  {19, 4},  {20, 6},  {21, 5},
    {23, 3}, {52, 1}, {57, 2}, {111, 0}, {115, 2}, {116, 0},
};
#define NROWS (sizeof k_rows / sizeof k_rows[0])

static void test_table_and_stack(void)
{
    unsigned n = 0;
    for (const win32_export_t *e = win32_exports_net; e->dll != NULL; e++) {
        CHECK(strcmp(e->dll, "WS2_32.dll") == 0 && e->name == NULL && e->fn != NULL);
        n++;
    }
    CHECK(n == NROWS);

    /* Every thunk pops its arguments and keeps the callee-saved registers.
     * Arguments point at a zeroed scratch area, so string and buffer
     * arguments are valid and empty. */
    memset(guest_ptr(DATA_AREA), 0, 0x1000);
    capture_begin();
    for (unsigned i = 0; i < NROWS; i++) {
        uint32_t args[6];
        for (unsigned a = 0; a < 6; a++)
            args[a] = DATA_AREA;
        if (k_rows[i].ordinal == 57)
            args[1] = 64; /* gethostname namelen */
        call_thunk(ord(k_rows[i].ordinal), k_rows[i].nargs, args);
    }
    /* socket was reported by test_sockets; a third call prints nothing. */
    CHECK(count(capture_end(), "UNSUPPORTED") == 0);
}

static void test_startup(void)
{
    uint32_t wsa = DATA_AREA;
    memset(guest_ptr(wsa), 0xAA, 400);
    CHECK(CALL(115, 0x0202u, wsa) == 0);
    const uint8_t *d = guest_ptr(wsa);
    CHECK(MEM16(wsa + 0) == 0x0202u);
    CHECK(MEM16(wsa + 2) == 0x0202u);
    CHECK(strcmp((const char *)d + 4, "WinSock 2.0") == 0);
    CHECK(strcmp((const char *)d + 261, "Running") == 0);
    CHECK(MEM32(wsa + 396) == 0);
    CHECK(CALL0(116) == 0);
}

static void test_byte_order(void)
{
    CHECK(CALL(9, 0x1234u) == 0x3412u);
    CHECK(CALL(9, 0xFFFF1234u) == 0x3412u); /* u_short: upper half ignored */
    CHECK(CALL(15, CALL(9, 51000u)) == 51000u);
    CHECK(CALL(8, 0x11223344u) == 0x44332211u);
    CHECK(CALL(8, CALL(8, 0xDEADBEEFu)) == 0xDEADBEEFu);
}

static void test_addresses(void)
{
    CHECK(CALL(11, guest_string(DATA_AREA, "127.0.0.1")) == 0x0100007Fu);
    CHECK(CALL(11, guest_string(DATA_AREA, "255.255.255.255")) == 0xFFFFFFFFu);
    CHECK(CALL(11, guest_string(DATA_AREA, "not an address")) == 0xFFFFFFFFu);
    CHECK(CALL(11, 0u) == 0xFFFFFFFFu);

    uint32_t s = CALL(12, 0x3C48870Au);
    CHECK(s != 0 && strcmp((const char *)guest_ptr(s), "10.135.72.60") == 0);
    CHECK(CALL(12, 0x0100007Fu) == s); /* one buffer per thread */
    CHECK(strcmp((const char *)guest_ptr(s), "127.0.0.1") == 0);

    uint32_t name = DATA_AREA + 0x100;
    CHECK(CALL(57, name, 255u) == 0);
    CHECK(strcmp((const char *)guest_ptr(name), "MAC") == 0);
    w32_set_last_error(0);
    CHECK(CALL(57, name, 2u) == 0xFFFFFFFFu);
    CHECK(CALL0(111) == 10014u);

    w32_set_last_error(0);
    CHECK(CALL(52, guest_string(DATA_AREA, "ukknulp-iportal.intl.warnerbros.com")) == 0);
    CHECK(CALL0(111) == 11001u);

    /* The game's call: its own host name, then inet_ntoa of the address. */
    uint32_t h = CALL(52, name);
    CHECK(h != 0 && strcmp((const char *)guest_ptr(MEM32(h)), "MAC") == 0);
    CHECK(MEM32(MEM32(h + 4)) == 0);
    CHECK(MEM16(h + 8) == 2 && MEM16(h + 10) == 4);
    uint32_t list = MEM32(h + 12);
    CHECK(MEM32(list + 4) == 0);
    s = CALL(12, MEM32(MEM32(list)));
    CHECK(strcmp((const char *)guest_ptr(s), "127.0.0.1") == 0);
    CHECK(CALL(52, guest_string(DATA_AREA, "LOCALHOST")) == h); /* one hostent per thread */
    CHECK(strcmp((const char *)guest_ptr(MEM32(h)), "localhost") == 0);
    CHECK(CALL(52, 0u) == h);
}

static void test_sockets(void)
{
    capture_begin();
    w32_set_last_error(0);
    uint32_t s = CALL(23, 2u, 1u, 6u);
    uint32_t e1 = CALL0(111);
    uint32_t s2 = CALL(23, 2u, 2u, 17u);
    char *out = capture_end();
    CHECK(s == 0xFFFFFFFFu && s2 == 0xFFFFFFFFu);
    CHECK(e1 == 10050u);
    CHECK(count(out, "UNSUPPORTED WS2_32.dll!socket: networking disabled "
                     "(af 2 type 1 proto 6, guest return 0xDEAD0001)\n") == 1);
    CHECK(count(out, "UNSUPPORTED") == 1);

    w32_set_last_error(0);
    CHECK(CALL(3, s) == 0xFFFFFFFFu);
    CHECK(CALL0(111) == 10038u);

    /* Every other SOCKET call: SOCKET_ERROR / INVALID_SOCKET + WSAENOTSOCK. */
    static const uint16_t k_sock_ords[] = {1, 2, 4, 10, 13, 16, 17, 19, 20, 21};
    for (unsigned i = 0; i < sizeof k_sock_ords / sizeof k_sock_ords[0]; i++) {
        uint32_t args[6] = {s, DATA_AREA, 16u, 0u, DATA_AREA, DATA_AREA};
        unsigned nargs = 0;
        for (unsigned r = 0; r < NROWS; r++)
            if (k_rows[r].ordinal == k_sock_ords[i])
                nargs = k_rows[r].nargs;
        w32_set_last_error(0);
        CHECK(call_thunk(ord(k_sock_ords[i]), nargs, args) == 0xFFFFFFFFu);
        CHECK(CALL0(111) == 10038u);
    }
}

int main(void)
{
    if (guest_mem_init() != 0) {
        fprintf(stderr, "guest_mem_init failed\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    test_sockets(); /* first: its socket calls are the process's first */
    test_table_and_stack();
    test_startup();
    test_byte_order();
    test_addresses();
    if (g_fail)
        return 1;
    printf("test_win32_net: all %zu thunks OK\n", NROWS);
    return 0;
}
