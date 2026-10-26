/* COM slice test: guest objects and vtables (com_guest.c), GUID helpers, the
 * class registry and the 7 ole32/oleaut32 imports (win32_com.c). Guest stack
 * as in the engine's THUNK_SLICES.md. Links com_guest.c, win32_com.c and the
 * engine's guest_mem.c, guest_heap.c, dlmalloc.c, host_funcs.c and
 * recomp_glue.c; not win32_core.c. */
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
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "com_guest.h"
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

/* ---- Guest stack ------------------------------------------------------- */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define DATA_AREA 0x00FF8000u
#define RET_ADDR 0xDEAD0001u

static uint32_t g_data_next = DATA_AREA;
static uint32_t guest_data(const void *p, uint32_t n)
{
    uint32_t a = g_data_next;
    memcpy(guest_ptr(a), p, n);
    g_data_next += (n + 15u) & ~15u;
    return a;
}

static uint32_t guest_guid(const com_guid_t *g)
{
    uint8_t b[16];
    b[0] = (uint8_t)g->d1, b[1] = (uint8_t)(g->d1 >> 8);
    b[2] = (uint8_t)(g->d1 >> 16), b[3] = (uint8_t)(g->d1 >> 24);
    b[4] = (uint8_t)g->d2, b[5] = (uint8_t)(g->d2 >> 8);
    b[6] = (uint8_t)g->d3, b[7] = (uint8_t)(g->d3 >> 8);
    memcpy(b + 8, g->d4, 8);
    return guest_data(b, 16);
}

static recomp_func_t com_export(const char *name, uint16_t ordinal)
{
    for (const win32_export_t *e = win32_exports_com; e->dll != NULL; e++)
        if (ordinal != 0 ? e->ordinal == ordinal && e->name == NULL
                         : e->name != NULL && strcmp(e->name, name) == 0)
            return e->fn;
    CHECK(!"export missing");
    return NULL;
}

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

/* ---- A fake interface -------------------------------------------------- */

static uint32_t g_seen_this, g_seen_arg;
static void m_qi(void) { W32_RETURN(3, COM_E_NOINTERFACE); }
static void m_addref(void) { W32_RETURN(1, 2); }
static void m_release(void) { W32_RETURN(1, 1); }
/* HRESULT Method(this, DWORD) */
static void m_method(void)
{
    g_seen_this = W32_ARG(0);
    g_seen_arg = W32_ARG(1);
    W32_RETURN(2, 0x1234u);
}
static const recomp_func_t g_methods[] = {m_qi, m_addref, m_release, m_method};
static const com_interface_t g_iface = {"ITest", g_methods, 4};

/* Call slot k of obj as lifted code does: push, then RECOMP_ICALL on the
 * address read from the vtable. */
static uint32_t call_method(uint32_t obj, uint32_t k, uint32_t arg)
{
    g_esp = STACK_TOP;
    g_esp -= 4, MEM32(g_esp) = arg;
    g_esp -= 4, MEM32(g_esp) = obj;
    g_esp -= 4, MEM32(g_esp) = RET_ADDR;
    uint32_t start = g_esp;
    {
        RECOMP_REGS_LOCAL;
        RECOMP_ICALL(MEM32(MEM32(obj) + 4u * k));
    }
    CHECK(g_esp == start + 12u);
    return g_eax;
}

/* Run com_guest_self(obj) in a child; it must abort with the bad-object line. */
static void check_self_aborts(uint32_t obj)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(fds[1], 2);
        g_esp = STACK_TOP - 4u;
        MEM32(g_esp) = RET_ADDR;
        com_guest_self(obj);
        _exit(0);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char buf[512] = {0}, want[128];
    ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    CHECK(CHILD_ABORTED(status));
    snprintf(want, sizeof want, "UNSUPPORTED com: bad object 0x%08X (guest return 0x%08X)\n",
             obj, RET_ADDR);
    CHECK(n > 0 && strcmp(buf, want) == 0);
}

static void test_objects(void)
{
    int host_a, host_b;
    uint32_t a = com_guest_new(&g_iface, &host_a), b = com_guest_new(&g_iface, &host_b);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(MEM32(a) == MEM32(b));
    CHECK(com_guest_self(a) == &host_a && com_guest_self(b) == &host_b);
    CHECK(com_guest_iface(a) == &g_iface);
    uint32_t vtbl = MEM32(a);
    for (uint32_t k = 0; k < g_iface.n_methods; k++)
        CHECK(guest_resolve(MEM32(vtbl + 4u * k)) == g_methods[k]);

    CHECK(call_method(b, 3, 0x77u) == 0x1234u);
    CHECK(g_seen_this == b && g_seen_arg == 0x77u);

    /* Freed: the 8 bytes are zeroed and the object no longer resolves. */
    com_guest_free(b);
    CHECK(com_guest_iface(b) == NULL);
    check_self_aborts(b);

    /* Forged: a copy of a live object at another address. */
    uint32_t forged = guest_malloc(8);
    MEM32(forged) = MEM32(a);
    MEM32(forged + 4u) = MEM32(a + 4u);
    CHECK(com_guest_iface(forged) == NULL);
    check_self_aborts(forged);
    check_self_aborts(0);
    guest_free(forged);
    com_guest_free(a);
}

/* ---- GUIDs and CoCreateInstance ---------------------------------------- */

static const com_guid_t g_iid_iunknown = {0x00000000u, 0x0000, 0x0000,
                                          {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const com_guid_t g_clsid_fake = {0x12345678u, 0x9ABC, 0xDEF0,
                                        {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static const com_guid_t g_clsid_missing = {0x2B02E3B6u, 0xD1D6, 0x4BB6,
                                           {0x9D, 0x73, 0x55, 0xB2, 0x0A, 0x76, 0x3D, 0x33}};

static int g_factory_calls;
static com_guid_t g_factory_iid;
static int g_fake_host;
static uint32_t fake_factory(const com_guid_t *iid, uint32_t *out)
{
    g_factory_calls++;
    g_factory_iid = *iid;
    *out = com_guest_new(&g_iface, &g_fake_host);
    return COM_S_OK;
}

static void test_guids(void)
{
    char s[39];
    com_guid_str(&g_iid_iunknown, s);
    CHECK(strcmp(s, "{00000000-0000-0000-C000-000000000046}") == 0);
    com_guid_t r = com_guid_read(guest_guid(&g_clsid_fake));
    CHECK(com_guid_eq(&r, &g_clsid_fake));
    CHECK(!com_guid_eq(&r, &g_clsid_missing));
    com_guid_str(&r, s);
    CHECK(strcmp(s, "{12345678-9ABC-DEF0-0123-456789ABCDEF}") == 0);
}

static void test_create_instance(void)
{
    recomp_func_t create = com_export("CoCreateInstance", 0);
    com_register_class(&g_clsid_fake, "FakeClass", fake_factory);
    uint32_t iid = guest_guid(&g_iid_iunknown), ppv = guest_data("\xAA\xAA\xAA\xAA", 4);

    uint32_t args[5] = {guest_guid(&g_clsid_fake), 0, 1, iid, ppv};
    CHECK(call_thunk(create, 5, args) == COM_S_OK);
    CHECK(g_factory_calls == 1 && com_guid_eq(&g_factory_iid, &g_iid_iunknown));
    CHECK(com_guest_self(MEM32(ppv)) == &g_fake_host);
    com_guest_free(MEM32(ppv));

    args[1] = 0x00500000u;
    CHECK(call_thunk(create, 5, args) == 0x80040110u);
    CHECK(g_factory_calls == 1);

    args[0] = guest_guid(&g_clsid_missing), args[1] = 0;
    capture_begin();
    MEM32(ppv) = 0xAAAAAAAAu;
    uint32_t hr1 = call_thunk(create, 5, args), out1 = MEM32(ppv);
    MEM32(ppv) = 0xAAAAAAAAu;
    uint32_t hr2 = call_thunk(create, 5, args), out2 = MEM32(ppv);
    char *log = capture_end();
    CHECK(hr1 == COM_REGDB_E_CLASSNOTREG && hr2 == COM_REGDB_E_CLASSNOTREG);
    CHECK(out1 == 0 && out2 == 0);
    CHECK(count_lines(log, "UNSUPPORTED ole32.dll!CoCreateInstance: CLSID "
                           "{2B02E3B6-D1D6-4BB6-9D73-55B20A763D33} IID "
                           "{00000000-0000-0000-C000-000000000046} (guest return 0xDEAD0001)\n") ==
          1);
}

/* ---- CoInitialize and friends ------------------------------------------ */

static void *init_other_thread(void *p)
{
    (void)p;
    uint32_t reserved = 0;
    uint32_t hr = call_thunk(com_export("CoInitialize", 0), 1, &reserved);
    return (void *)(uintptr_t)hr;
}

static void test_init(void)
{
    uint32_t args[8] = {0};
    CHECK(call_thunk(com_export("CoInitialize", 0), 1, args) == COM_S_OK);
    CHECK(call_thunk(com_export("CoInitialize", 0), 1, args) == COM_S_FALSE);
    args[1] = 2; /* COINIT_APARTMENTTHREADED */
    CHECK(call_thunk(com_export("CoInitializeEx", 0), 2, args) == COM_S_FALSE);

    pthread_t t;
    void *hr = NULL;
    CHECK(pthread_create(&t, NULL, init_other_thread, NULL) == 0);
    pthread_join(t, &hr);
    CHECK((uintptr_t)hr == COM_S_OK);

    for (int i = 0; i < 3; i++)
        call_thunk(com_export("CoUninitialize", 0), 0, NULL);
    args[1] = 0;
    CHECK(call_thunk(com_export("CoInitializeEx", 0), 2, args) == COM_S_OK);
    call_thunk(com_export("CoUninitialize", 0), 0, NULL);

    CHECK(call_thunk(com_export("CoSetProxyBlanket", 0), 8, args) == COM_S_OK);
}

/* ---- BSTR -------------------------------------------------------------- */

static void test_bstr(void)
{
    static const uint16_t hello[] = {'H', 'i', '!', 0};
    uint32_t src = guest_data(hello, sizeof hello);
    recomp_func_t alloc = com_export(NULL, 2), release = com_export(NULL, 6);
    uint32_t b = call_thunk(alloc, 1, &src);
    CHECK(b != 0 && b != src);
    CHECK(MEM32(b - 4u) == 6u);
    CHECK(memcmp(guest_ptr(b), hello, sizeof hello) == 0);
    uint32_t null = 0;
    CHECK(call_thunk(alloc, 1, &null) == 0);
    call_thunk(release, 1, &b);
    call_thunk(release, 1, &null);
    /* The freed block is reused: the prefix was the block start. */
    CHECK(call_thunk(alloc, 1, &src) == b);
}

int main(void)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    test_objects();
    test_guids();
    test_create_instance();
    test_init();
    test_bstr();
    if (g_fail) {
        printf("test_com_guest: FAILED\n");
        return 1;
    }
    printf("test_com_guest: all checks passed\n");
    return 0;
}
