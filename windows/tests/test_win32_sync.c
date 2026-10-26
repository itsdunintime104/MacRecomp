/* Thunk test: the Win32 sync slice (runtime/src/win32_sync.c). Links no
 * win32_core.c: the handle table and last error below are minimal doubles. */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "quiet_abort.h"
#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "guest_call.h"
#include "win32_thunk.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Guest layout inside the mapped 0x00F00000-0x01000000 window. */
#define G_CELL 0x00F00000u    /* scratch words */
#define G_HANDLES 0x00F00040u /* handle arrays for the multi-object waits */
#define G_CS 0x00F00080u      /* 24-byte CRITICAL_SECTION */
#define G_CS2 0x00F000C0u     /* never initialised */
#define G_COUNTER 0x00F00100u
#define G_FT 0x00F00140u      /* FILETIME */
#define G_FT2 0x00F00148u
#define G_ST 0x00F00180u      /* SYSTEMTIME */
#define G_TZI 0x00F00200u     /* TIME_ZONE_INFORMATION, 172 bytes */
#define G_NAME 0x00F00300u
#define MAIN_TOP 0x01000000u
#define MAIN_LIMIT 0x00FF0000u

#define INFINITE 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102u
#define WAIT_ABANDONED_0 0x80u
#define STILL_ACTIVE 259u
#define N_ITERS 100000u

/* ---- Doubles of the core functions -------------------------------------- */

static __thread uint32_t t_err;
static struct w32_object *g_tab[256];
static pthread_mutex_t g_tab_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t w32_last_error(void) { return t_err; }
void w32_set_last_error(uint32_t code) { t_err = code; }

uint32_t w32_handle_new(struct w32_object *obj)
{
    obj->refs = 1;
    pthread_mutex_lock(&g_tab_lock);
    for (uint32_t i = 0; i < 256; i++) {
        if (g_tab[i] == NULL) {
            g_tab[i] = obj;
            pthread_mutex_unlock(&g_tab_lock);
            return W32_HANDLE_BASE + 4u * i;
        }
    }
    pthread_mutex_unlock(&g_tab_lock);
    return 0;
}

struct w32_object *w32_handle_get(uint32_t h, enum w32_kind kind)
{
    uint32_t i = (h - W32_HANDLE_BASE) / 4u;
    if (h < W32_HANDLE_BASE || (h & 3u) != 0 || i >= 256)
        return NULL;
    pthread_mutex_lock(&g_tab_lock);
    struct w32_object *obj = g_tab[i];
    pthread_mutex_unlock(&g_tab_lock);
    return obj != NULL && (kind == W32_KIND_ANY || obj->kind == kind) ? obj : NULL;
}

uint32_t w32_handle_close(uint32_t h)
{
    uint32_t i = (h - W32_HANDLE_BASE) / 4u;
    struct w32_object *obj = NULL;
    if (h >= W32_HANDLE_BASE && i < 256) {
        pthread_mutex_lock(&g_tab_lock);
        obj = g_tab[i];
        g_tab[i] = NULL;
        pthread_mutex_unlock(&g_tab_lock);
    }
    if (obj == NULL)
        return W32_FALSE;
    if (__atomic_sub_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL) == 0)
        obj->destroy(obj);
    return W32_TRUE;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* ---- Calling thunks ----------------------------------------------------- */

/* Argument counts from notes/slice_owners.md (bytes popped / 4). */
static const struct { const char *name; int nargs; } g_expect[] = {
    {"CompareFileTime", 2}, {"CreateEventA", 4}, {"CreateMutexA", 3},
    {"CreateSemaphoreA", 4}, {"CreateThread", 6}, {"DeleteCriticalSection", 1},
    {"EnterCriticalSection", 1}, {"ExitThread", 1}, {"FileTimeToLocalFileTime", 2},
    {"FlsAlloc", 1}, {"FlsFree", 1}, {"FlsGetValue", 1}, {"FlsSetValue", 2},
    {"InitializeCriticalSectionAndSpinCount", 2},
    {"FileTimeToSystemTime", 2}, {"GetCurrentThread", 0}, {"GetCurrentThreadId", 0},
    {"GetExitCodeThread", 2}, {"GetProcessAffinityMask", 3}, {"GetSystemTime", 1},
    {"GetSystemTimeAsFileTime", 1}, {"GetTickCount", 0}, {"GetTimeZoneInformation", 1},
    {"InitializeCriticalSection", 1}, {"InterlockedCompareExchange", 3},
    {"InterlockedDecrement", 1}, {"InterlockedExchange", 2}, {"InterlockedIncrement", 1},
    {"LeaveCriticalSection", 1}, {"QueryPerformanceCounter", 1},
    {"QueryPerformanceFrequency", 1}, {"ReleaseMutex", 1}, {"ReleaseSemaphore", 3},
    {"ResetEvent", 1}, {"ResumeThread", 1}, {"SetEvent", 1}, {"SetProcessAffinityMask", 2},
    {"SetThreadAffinityMask", 2}, {"SetThreadExecutionState", 1}, {"SetThreadPriority", 2},
    {"Sleep", 1}, {"SystemTimeToFileTime", 2}, {"TlsAlloc", 0}, {"TlsFree", 1},
    {"TlsGetValue", 1}, {"TlsSetValue", 2}, {"TryEnterCriticalSection", 1},
    {"WaitForMultipleObjects", 4}, {"WaitForMultipleObjectsEx", 5},
    {"WaitForSingleObject", 2},
};
#define N_EXPECT (sizeof g_expect / sizeof g_expect[0])
static _Atomic int g_called[N_EXPECT];

static int expect_index(const char *name)
{
    for (unsigned i = 0; i < N_EXPECT; i++)
        if (strcmp(g_expect[i].name, name) == 0)
            return (int)i;
    return -1;
}

static recomp_func_t thunk(const char *name)
{
    for (const win32_export_t *e = win32_exports_sync; e->dll != NULL; e++)
        if (strcmp(e->name, name) == 0)
            return e->fn;
    fprintf(stderr, "FAIL: no thunk %s\n", name);
    exit(1);
}

/* Call a thunk the way lifted code does: args at esp+4, then the return
 * address push. Checks the stdcall pop and the callee-saved registers. */
static uint32_t call(const char *name, int n, const uint32_t *args)
{
    int k = expect_index(name);
    CHECK(k >= 0 && g_expect[k].nargs == n);
    recomp_func_t f = thunk(name);
    uint32_t sp = g_esp;
    for (int i = n - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0x12345678u);
    uint32_t before = g_esp;
    g_ebx = 0xB0B0B0B0u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xEBEBEBEBu;
    f();
    if (g_esp != before + 4u + 4u * (uint32_t)n) {
        fprintf(stderr, "FAIL %s: esp +%u, want +%u\n", name, g_esp - before, 4u + 4u * n);
        exit(1);
    }
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xEBEBEBEBu);
    g_called[k] = 1;
    g_esp = sp;
    return g_eax;
}

#define CALL(name, ...) call(name, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})
#define CALL0(name) call(name, 0, NULL)

/* ---- Guest thread start routines (stdcall, one argument) ---------------- */

static uint32_t g_tls_idx, g_gate, g_mutex, g_main_tib;
static _Atomic int g_thread_tls_ok, g_thread_tib_ok, g_thread_not_owner, g_thread_try_busy;

static uint32_t start_arg(void)
{
    return MEM32(g_esp + 4);
}

static void start_return(uint32_t code)
{
    g_eax = code;
    g_esp += 8;
}

/* Wait for g_gate, check this thread's TIB and TLS, return arg ^ 0xA5A5. */
static void start_gate(void)
{
    uint32_t arg = start_arg();
    uint32_t fs = g_fs_base;
    g_thread_tib_ok = fs != 0 && fs != g_main_tib && MEM32(fs + 0x18) == fs &&
                      MEM32(fs) == 0xFFFFFFFFu && MEM32(fs + 0x24) == CALL0("GetCurrentThreadId") &&
                      MEM32(fs + 4) > g_esp && MEM32(fs + 8) < g_esp;
    g_thread_tls_ok = CALL("TlsGetValue", g_tls_idx) == 0; /* fresh thread */
    CHECK(CALL("WaitForSingleObject", g_gate, INFINITE) == 0);
    start_return(arg ^ 0xA5A5u);
}

/* ExitThread from inside the routine: never returns here. */
static void start_exit(void)
{
    uint32_t arg = start_arg();
    PUSH32(g_esp, arg);
    PUSH32(g_esp, 0x12345678u);
    thunk("ExitThread")();
    CHECK(!"ExitThread returned");
}

/* Record the x87 control word and MXCSR the thread starts with. */
static uint32_t g_thread_cw, g_thread_mxcsr;
static void start_fpu(void)
{
    g_thread_cw = g_fp_control_word;
    g_thread_mxcsr = g_mxcsr;
    start_return(0);
}

/* N_ITERS locked read-modify-writes of G_COUNTER. */
static void start_count(void)
{
    for (uint32_t i = 0; i < N_ITERS; i++) {
        CALL("EnterCriticalSection", G_CS);
        uint32_t v = MEM32(G_COUNTER);
        MEM32(G_COUNTER) = v + 1u;
        CALL("LeaveCriticalSection", G_CS);
    }
    start_return(0);
}

/* Take g_mutex and exit while holding it. */
static void start_hold(void)
{
    CHECK(CALL("WaitForSingleObject", g_mutex, INFINITE) == 0);
    start_return(0);
}

/* Probe g_mutex, which the main thread holds. */
static void start_probe_mutex(void)
{
    g_thread_not_owner = CALL("ReleaseMutex", g_mutex) == W32_FALSE && w32_last_error() == 288u;
    start_return(0);
}

/* Probe G_CS, which the main thread holds. */
static void start_probe_cs(void)
{
    g_thread_try_busy = CALL("TryEnterCriticalSection", G_CS) == W32_FALSE;
    start_return(0);
}

/* A plain host thread (no CreateThread): attach twice, record the TIB. */
struct host_probe {
    uint32_t tib, self, tid, tid2, base, limit;
};

static _Atomic int g_host_arrived, g_host_expect;

static void *host_thread(void *p)
{
    struct host_probe *r = p;
    CHECK(g_fs_base == 0);
    w32_host_thread_attach();
    CHECK(g_fp_control_word == 0x027F && g_mxcsr == 0x1F80); /* as Windows starts a thread */
    uint32_t tib = g_fs_base;
    w32_host_thread_attach();
    CHECK(g_fs_base == tib);
    r->tib = tib;
    r->self = MEM32(g_fs_base + 0x18); /* fs:[0x18] */
    r->tid = CALL0("GetCurrentThreadId");
    r->tid2 = CALL0("GetCurrentThreadId");
    r->base = MEM32(tib + 4);
    r->limit = MEM32(tib + 8);
    CHECK(MEM32(tib + 0x24) == r->tid);
    CHECK(r->limit < g_esp && g_esp <= r->base);
    g_host_arrived++;
    while (g_host_arrived < g_host_expect) /* keep the TIB alive until all arrive */
        sched_yield();
    return NULL;
}

/* FLS: a thread sets its value; the slot's callback gets it at exit. */
static uint32_t g_fls_idx;
static _Atomic uint32_t g_fls_cb_value, g_fls_cb_calls;
static void fls_callback(void)
{
    g_fls_cb_value = MEM32(g_esp + 4);
    g_fls_cb_calls++;
    g_esp += 8; /* stdcall, one argument */
}

static void start_gate_free(void)
{
    start_return(0);
}

static void start_fls(void)
{
    uint32_t arg = start_arg();
    CHECK(CALL("FlsGetValue", g_fls_idx) == 0); /* fresh thread */
    CHECK(CALL("FlsSetValue", g_fls_idx, arg) == W32_TRUE);
    start_return(0);
}

static uint32_t new_thread(void (*fn)(void), uint32_t arg, uint32_t flags)
{
    uint32_t h = CALL("CreateThread", 0, 0, host_func_va(fn), arg, flags, G_CELL);
    CHECK(h != 0 && MEM32(G_CELL) >= 0x100u);
    return h;
}

/* ---- Tests -------------------------------------------------------------- */

static void test_table(void)
{
    unsigned n = 0;
    for (const win32_export_t *e = win32_exports_sync; e->dll != NULL; e++, n++) {
        CHECK(strcmp(e->dll, "KERNEL32.dll") == 0 && e->ordinal == 0 && e->fn != NULL);
        CHECK(expect_index(e->name) >= 0);
    }
    CHECK(n == 50 && N_EXPECT == 50);
}

static void test_tib_and_tls(void)
{
    uint32_t fs = g_fs_base, tid = CALL0("GetCurrentThreadId");
    g_main_tib = fs;
    CHECK(fs != 0 && tid >= 0x100u && CALL0("GetCurrentThreadId") == tid);
    CHECK(MEM32(fs + 0x00) == 0xFFFFFFFFu);
    CHECK(MEM32(fs + 0x04) == MAIN_TOP && MEM32(fs + 0x08) == MAIN_LIMIT);
    CHECK(MEM32(fs + 0x18) == fs);
    CHECK(MEM32(fs + 0x20) == (uint32_t)getpid() && MEM32(fs + 0x24) == tid);
    uint32_t slots = MEM32(fs + 0x2C);
    CHECK(slots > fs && slots + 64u * 4u <= fs + 0x1000u);
    CHECK(MEM32(fs + 0x34) == 0);

    g_tls_idx = CALL0("TlsAlloc");
    CHECK(g_tls_idx < 64);
    CHECK(CALL("TlsGetValue", g_tls_idx) == 0);
    CHECK(CALL("TlsSetValue", g_tls_idx, 0x1234u) == W32_TRUE);
    CHECK(CALL("TlsGetValue", g_tls_idx) == 0x1234u && w32_last_error() == 0);
    CHECK(MEM32(MEM32(fs + 0x2C) + 4u * g_tls_idx) == 0x1234u); /* fs:[0x2C] view */
    MEM32(MEM32(fs + 0x2C) + 4u * g_tls_idx) = 0x5678u;
    CHECK(CALL("TlsGetValue", g_tls_idx) == 0x5678u);
    uint32_t spare = CALL0("TlsAlloc");
    CHECK(spare < 64 && spare != g_tls_idx);
    CHECK(CALL("TlsFree", spare) == W32_TRUE);
    CHECK(CALL("TlsFree", spare) == W32_FALSE && w32_last_error() == 87u);
    CHECK(CALL("TlsSetValue", 64u, 1u) == W32_FALSE);

    /* FLS slots are TLS slots plus an exit callback. */
    g_fls_idx = CALL("FlsAlloc", host_func_va(fls_callback));
    CHECK(g_fls_idx < 64 && g_fls_idx != g_tls_idx);
    CHECK(CALL("FlsSetValue", g_fls_idx, 0xABCDu) == W32_TRUE);
    CHECK(CALL("FlsGetValue", g_fls_idx) == 0xABCDu && w32_last_error() == 0);
}

static void test_fls_exit_callback(void)
{
    uint32_t h = new_thread(start_fls, 0x2468u, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0 && w32_handle_close(h));
    CHECK(g_fls_cb_calls == 1 && g_fls_cb_value == 0x2468u);
    h = new_thread(start_gate_free, 0, 0); /* no value set: no callback */
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0 && w32_handle_close(h));
    CHECK(g_fls_cb_calls == 1);
    CHECK(CALL("FlsGetValue", g_fls_idx) == 0xABCDu); /* main thread's own value */
    CHECK(CALL("FlsFree", g_fls_idx) == W32_TRUE);
    CHECK(CALL("FlsFree", g_fls_idx) == W32_FALSE && w32_last_error() == 87u);
}

static void test_threads(void)
{
    uint32_t code = 0;
    g_gate = CALL("CreateEventA", 0, W32_TRUE, W32_FALSE, 0);
    CHECK(g_gate != 0);

    uint32_t h = new_thread(start_gate, 0x1111u, 0);
    CHECK(MEM32(G_CELL) != CALL0("GetCurrentThreadId"));
    CHECK(CALL("GetExitCodeThread", h, G_CELL + 4) == W32_TRUE && MEM32(G_CELL + 4) == STILL_ACTIVE);
    CHECK(CALL("WaitForSingleObject", h, 20) == WAIT_TIMEOUT);
    CHECK(CALL("SetEvent", g_gate) == W32_TRUE);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(CALL("GetExitCodeThread", h, G_CELL + 4) == W32_TRUE);
    code = MEM32(G_CELL + 4);
    CHECK(code == (0x1111u ^ 0xA5A5u));
    CHECK(g_thread_tib_ok && g_thread_tls_ok);
    CHECK(CALL("WaitForSingleObject", h, 0) == 0); /* a thread stays signalled */
    CHECK(w32_handle_close(h));

    /* CREATE_SUSPENDED waits for ResumeThread. */
    h = new_thread(start_gate, 0x2222u, 0x4u);
    CHECK(CALL("WaitForSingleObject", h, 20) == WAIT_TIMEOUT);
    CHECK(CALL("ResumeThread", h) == 1u);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(CALL("GetExitCodeThread", h, G_CELL + 4) == W32_TRUE && MEM32(G_CELL + 4) == (0x2222u ^ 0xA5A5u));
    CHECK(CALL("ResumeThread", h) == 0u);
    CHECK(w32_handle_close(h));

    /* ExitThread unwinds to the trampoline with its code. */
    h = new_thread(start_exit, 0x77u, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(CALL("GetExitCodeThread", h, G_CELL + 4) == W32_TRUE && MEM32(G_CELL + 4) == 0x77u);
    CHECK(w32_handle_close(h));

    /* Windows starts every thread at x87 0x027F and MXCSR 0x1F80, whatever
     * the creating thread runs with. */
    g_fp_control_word = 0x007F;
    g_mxcsr = 0x7F80;
    h = new_thread(start_fpu, 0, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(g_thread_cw == 0x027Fu && g_thread_mxcsr == 0x1F80u);
    CHECK(w32_handle_close(h));

    /* Not a handle. */
    CHECK(CALL("ResumeThread", 0x12345678u) == 0xFFFFFFFFu && w32_last_error() == 6u);
    CHECK(CALL("GetExitCodeThread", 0x12345678u, G_CELL + 4) == W32_FALSE);
}

static void test_host_threads(void)
{
    uint32_t main_tib = g_fs_base;
    w32_host_thread_attach(); /* main thread already attached: no-op */
    CHECK(g_fs_base == main_tib);
    struct host_probe r[2];
    g_host_expect = 0;
    for (int i = 0; i < 2; i++) {
        pthread_t t;
        CHECK(pthread_create(&t, NULL, host_thread, &r[i]) == 0);
        CHECK(pthread_join(t, NULL) == 0);
        CHECK(r[i].tib != 0 && r[i].tib != main_tib && r[i].self == r[i].tib);
        CHECK(r[i].tid >= 0x100u && r[i].tid == r[i].tid2);
        CHECK(r[i].base - r[i].limit == GUEST_CB_STACK_SIZE);
        CHECK(r[i].limit >= GUEST_CB_STACK_BASE && r[i].base <= GUEST_CB_STACK_END);
    }
    CHECK(r[0].tid != r[1].tid);
    /* Run both at once so neither can reuse the other's freed TIB. */
    pthread_t t[2];
    g_host_arrived = 0;
    g_host_expect = 2;
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&t[i], NULL, host_thread, &r[i]) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(r[0].tib != r[1].tib && r[0].tid != r[1].tid);
}

static void test_events_and_waits(void)
{
    uint32_t aut = CALL("CreateEventA", 0, W32_FALSE, W32_TRUE, 0);
    CHECK(aut != 0 && w32_last_error() == 0);
    CHECK(CALL("WaitForSingleObject", aut, 0) == 0);
    CHECK(CALL("WaitForSingleObject", aut, 0) == WAIT_TIMEOUT); /* auto reset */
    CHECK(CALL("SetEvent", aut) == W32_TRUE);
    CHECK(CALL("WaitForSingleObject", aut, 0) == 0);
    CHECK(CALL("WaitForSingleObject", aut, 0) == WAIT_TIMEOUT);

    uint32_t man = CALL("CreateEventA", 0, W32_TRUE, W32_TRUE, 0);
    CHECK(CALL("WaitForSingleObject", man, 0) == 0);
    CHECK(CALL("WaitForSingleObject", man, 0) == 0); /* manual reset stays set */

    /* WAIT_TIMEOUT after at least the timeout. */
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    CHECK(CALL("WaitForSingleObject", aut, 30) == WAIT_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &b);
    CHECK((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 >= 30);

    /* Wait-all on an unset event and a semaphore: atomic. */
    uint32_t ev = CALL("CreateEventA", 0, W32_TRUE, W32_FALSE, 0);
    uint32_t sem = CALL("CreateSemaphoreA", 0, 1, 2, 0);
    CHECK(ev != 0 && sem != 0);
    MEM32(G_HANDLES) = sem;
    MEM32(G_HANDLES + 4) = ev;
    CHECK(CALL("WaitForMultipleObjects", 2, G_HANDLES, W32_TRUE, 0) == WAIT_TIMEOUT);
    CHECK(CALL("ReleaseSemaphore", sem, 1, G_CELL) == W32_TRUE && MEM32(G_CELL) == 1u); /* not taken */
    CHECK(CALL("SetEvent", ev) == W32_TRUE);
    CHECK(CALL("WaitForMultipleObjects", 2, G_HANDLES, W32_TRUE, 0) == 0); /* count 2 -> 1 */
    CHECK(CALL("WaitForMultipleObjects", 2, G_HANDLES, W32_FALSE, 0) == 0); /* sem: 1 -> 0 */
    CHECK(CALL("WaitForMultipleObjectsEx", 2, G_HANDLES, W32_FALSE, 0, W32_TRUE) == 1u); /* ev */
    CHECK(CALL("WaitForMultipleObjects", 2, G_HANDLES, W32_TRUE, 10) == WAIT_TIMEOUT);

    CHECK(CALL("WaitForSingleObject", 0x12345678u, 0) == 0xFFFFFFFFu && w32_last_error() == 6u);
    CHECK(CALL("WaitForMultipleObjects", 0, G_HANDLES, W32_FALSE, 0) == 0xFFFFFFFFu);
    CHECK(CALL("SetEvent", sem) == W32_FALSE && w32_last_error() == 6u);
    CHECK(w32_handle_close(aut) && w32_handle_close(man));
    CHECK(w32_handle_close(ev) && w32_handle_close(sem));
}

/* A host thread that waits on one event, as the game's streaming thread
 * waits on its timer's event. */
struct pulse_waiter {
    uint32_t h, ms, r;
    _Atomic int started;
};

static void *pulse_wait_main(void *p)
{
    struct pulse_waiter *w = p;
    w32_host_thread_attach();
    w->started = 1;
    w->r = CALL("WaitForSingleObject", w->h, w->ms);
    return NULL;
}

/* Start n waiters on h and give them time to block in the wait. */
static void start_waiters(struct pulse_waiter *w, pthread_t *t, int n, uint32_t h, uint32_t ms)
{
    for (int i = 0; i < n; i++) {
        w[i].h = h, w[i].ms = ms, w[i].r = 0xDEADu, w[i].started = 0;
        CHECK(pthread_create(&t[i], NULL, pulse_wait_main, &w[i]) == 0);
    }
    for (int i = 0; i < n; i++)
        while (!w[i].started)
            sched_yield();
    usleep(100000);
}

/* ResetEvent, and the PulseEvent semantics timeSetEvent's
 * TIME_CALLBACK_EVENT_PULSE needs (w32_event_set(h, 1)). */
static void test_reset_and_pulse(void)
{
    uint32_t man = CALL("CreateEventA", 0, W32_TRUE, W32_TRUE, 0);
    CHECK(CALL("ResetEvent", man) == W32_TRUE);
    CHECK(CALL("WaitForSingleObject", man, 0) == WAIT_TIMEOUT);
    w32_set_last_error(0);
    CHECK(CALL("ResetEvent", 0x12345678u) == W32_FALSE && w32_last_error() == 6u);

    /* No waiter: a pulse is lost, and the event stays unsignalled. */
    CHECK(w32_event_set(man, 1) == W32_TRUE);
    CHECK(CALL("WaitForSingleObject", man, 0) == WAIT_TIMEOUT);

    /* Manual reset: every thread waiting at the pulse is released. */
    struct pulse_waiter w[2];
    pthread_t t[2];
    start_waiters(w, t, 2, man, 5000);
    CHECK(w32_event_set(man, 1) == W32_TRUE);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(t[i], NULL) == 0 && w[i].r == 0);
    CHECK(CALL("WaitForSingleObject", man, 0) == WAIT_TIMEOUT); /* then unsignalled */

    /* Auto reset: at most one waiter is released. */
    uint32_t aut = CALL("CreateEventA", 0, W32_FALSE, W32_FALSE, 0);
    start_waiters(w, t, 2, aut, 600);
    CHECK(w32_event_set(aut, 1) == W32_TRUE);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    CHECK((w[0].r == 0) + (w[1].r == 0) == 1);
    CHECK((w[0].r == WAIT_TIMEOUT) + (w[1].r == WAIT_TIMEOUT) == 1);
    CHECK(CALL("WaitForSingleObject", aut, 0) == WAIT_TIMEOUT);

    /* SetEvent through the same entry. */
    CHECK(w32_event_set(aut, 0) == W32_TRUE && CALL("WaitForSingleObject", aut, 0) == 0);
    CHECK(w32_event_set(0x12345678u, 1) == W32_FALSE);
    CHECK(w32_handle_close(man) && w32_handle_close(aut));
}

static void test_mutex_and_semaphore(void)
{
    g_mutex = CALL("CreateMutexA", 0, W32_TRUE, 0);
    CHECK(g_mutex != 0);
    CHECK(CALL("WaitForSingleObject", g_mutex, 0) == 0); /* recursive */
    uint32_t h = new_thread(start_probe_mutex, 0, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(w32_handle_close(h));
    CHECK(g_thread_not_owner);
    CHECK(CALL("ReleaseMutex", g_mutex) == W32_TRUE);
    CHECK(CALL("ReleaseMutex", g_mutex) == W32_TRUE);
    CHECK(CALL("ReleaseMutex", g_mutex) == W32_FALSE && w32_last_error() == 288u);

    /* A mutex whose owner exited is abandoned once. */
    h = new_thread(start_hold, 0, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(w32_handle_close(h));
    MEM32(G_HANDLES) = g_mutex;
    CHECK(CALL("WaitForMultipleObjects", 1, G_HANDLES, W32_FALSE, 0) == WAIT_ABANDONED_0);
    CHECK(CALL("ReleaseMutex", g_mutex) == W32_TRUE);
    CHECK(CALL("WaitForSingleObject", g_mutex, 0) == 0);
    CHECK(CALL("ReleaseMutex", g_mutex) == W32_TRUE);
    CHECK(w32_handle_close(g_mutex));

    /* Named objects: a second create opens the same object. */
    strcpy((char *)guest_ptr(G_NAME), "HPRecompTestMutex");
    uint32_t n1 = CALL("CreateMutexA", 0, W32_TRUE, G_NAME);
    CHECK(n1 != 0 && w32_last_error() == 0);
    uint32_t n2 = CALL("CreateMutexA", 0, W32_FALSE, G_NAME);
    CHECK(n2 != 0 && n2 != n1 && w32_last_error() == 183u);
    CHECK(CALL("ReleaseMutex", n2) == W32_TRUE); /* owned through n1 */
    CHECK(CALL("CreateEventA", 0, W32_FALSE, W32_FALSE, G_NAME) == 0 && w32_last_error() == 6u);
    CHECK(w32_handle_close(n1));
    CHECK(CALL("WaitForSingleObject", n2, 0) == 0); /* n2 keeps it alive */
    CHECK(CALL("ReleaseMutex", n2) == W32_TRUE);
    CHECK(w32_handle_close(n2));
    CHECK(CALL("CreateMutexA", 0, W32_FALSE, G_NAME) != 0 && w32_last_error() == 0); /* gone */

    uint32_t s = CALL("CreateSemaphoreA", 0, 1, 2, 0);
    CHECK(CALL("ReleaseSemaphore", s, 1, G_CELL) == W32_TRUE && MEM32(G_CELL) == 1u);
    MEM32(G_CELL) = 0xDEADu;
    CHECK(CALL("ReleaseSemaphore", s, 1, G_CELL) == W32_FALSE && w32_last_error() == 298u);
    CHECK(MEM32(G_CELL) == 0xDEADu);
    CHECK(CALL("WaitForSingleObject", s, 0) == 0 && CALL("WaitForSingleObject", s, 0) == 0);
    CHECK(CALL("WaitForSingleObject", s, 0) == WAIT_TIMEOUT);
    CHECK(CALL("CreateSemaphoreA", 0, 3, 2, 0) == 0 && w32_last_error() == 87u);
    CHECK(w32_handle_close(s));
}

static void test_critical_section(void)
{
    uint32_t tid = CALL0("GetCurrentThreadId");
    CHECK(CALL("InitializeCriticalSectionAndSpinCount", G_CS, 4000u) == W32_TRUE);
    CHECK(MEM32(G_CS + 4) == 0xFFFFFFFFu && MEM32(G_CS + 0x14) == 4000u);
    CALL("DeleteCriticalSection", G_CS);
    CALL("InitializeCriticalSection", G_CS);
    CHECK(MEM32(G_CS + 4) == 0xFFFFFFFFu && MEM32(G_CS + 8) == 0 && MEM32(G_CS + 12) == 0);
    CALL("EnterCriticalSection", G_CS);
    CALL("EnterCriticalSection", G_CS);
    CHECK(CALL("TryEnterCriticalSection", G_CS) == W32_TRUE);
    CHECK(MEM32(G_CS + 8) == 3u && MEM32(G_CS + 12) == tid && MEM32(G_CS + 4) == 2u);
    uint32_t h = new_thread(start_probe_cs, 0, 0);
    CHECK(CALL("WaitForSingleObject", h, INFINITE) == 0);
    CHECK(w32_handle_close(h) && g_thread_try_busy);
    for (int i = 0; i < 3; i++)
        CALL("LeaveCriticalSection", G_CS);
    CHECK(MEM32(G_CS + 4) == 0xFFFFFFFFu && MEM32(G_CS + 8) == 0 && MEM32(G_CS + 12) == 0);

    /* Two threads, N_ITERS each. */
    MEM32(G_COUNTER) = 0;
    MEM32(G_HANDLES) = new_thread(start_count, 0, 0);
    MEM32(G_HANDLES + 4) = new_thread(start_count, 0, 0);
    CHECK(CALL("WaitForMultipleObjects", 2, G_HANDLES, W32_TRUE, INFINITE) == 0);
    CHECK(MEM32(G_COUNTER) == 2u * N_ITERS);
    CHECK(MEM32(G_CS + 4) == 0xFFFFFFFFu && MEM32(G_CS + 8) == 0);
    CHECK(w32_handle_close(MEM32(G_HANDLES)) && w32_handle_close(MEM32(G_HANDLES + 4)));
    CALL("DeleteCriticalSection", G_CS);

    /* Entering a section that was never initialised stops the process. */
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(fds[1], 2);
        CALL("EnterCriticalSection", G_CS2);
        _exit(0);
    }
    close(fds[1]);
    char buf[256] = {0};
    ssize_t got = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(CHILD_ABORTED(status));
    CHECK(got > 0 && strcmp(buf, "UNSUPPORTED KERNEL32.dll!EnterCriticalSection: "
                                 "uninitialised critical section 0x00F000C0\n") == 0);
}

static void test_interlocked(void)
{
    MEM32(G_CELL) = 5;
    CHECK(CALL("InterlockedIncrement", G_CELL) == 6u && MEM32(G_CELL) == 6u);
    CHECK(CALL("InterlockedDecrement", G_CELL) == 5u && MEM32(G_CELL) == 5u);
    CHECK(CALL("InterlockedExchange", G_CELL, 9) == 5u && MEM32(G_CELL) == 9u);
    CHECK(CALL("InterlockedCompareExchange", G_CELL, 1, 9) == 9u && MEM32(G_CELL) == 1u);
    CHECK(CALL("InterlockedCompareExchange", G_CELL, 2, 9) == 1u && MEM32(G_CELL) == 1u);
    MEM32(G_CELL) = 0;
    CHECK(CALL("InterlockedDecrement", G_CELL) == 0xFFFFFFFFu);
}

static void test_clocks(void)
{
    CHECK(CALL("QueryPerformanceFrequency", G_FT) == W32_TRUE);
    CHECK(MEM32(G_FT) == 10000000u && MEM32(G_FT + 4) == 0);
    uint32_t t0 = CALL0("GetTickCount");
    CHECK(t0 >= 3600000u);
    CHECK(CALL("QueryPerformanceCounter", G_FT) == W32_TRUE);
    CALL("Sleep", 10);
    CALL("Sleep", 0);
    CHECK(CALL("QueryPerformanceCounter", G_FT2) == W32_TRUE);
    uint64_t a = MEM32(G_FT) | (uint64_t)MEM32(G_FT + 4) << 32;
    uint64_t b = MEM32(G_FT2) | (uint64_t)MEM32(G_FT2 + 4) << 32;
    CHECK(b > a && b - a >= 100000u && b - a < 10000000u); /* 10 ms .. 1 s */
    uint32_t t1 = CALL0("GetTickCount");
    CHECK(t1 - t0 >= 10u && t1 - t0 < 1000u);
}

static void st_set(uint32_t st, unsigned y, unsigned mo, unsigned d, unsigned h,
                   unsigned mi, unsigned s, unsigned ms)
{
    MEM16(st + 0) = (uint16_t)y;
    MEM16(st + 2) = (uint16_t)mo;
    MEM16(st + 4) = 0;
    MEM16(st + 6) = (uint16_t)d;
    MEM16(st + 8) = (uint16_t)h;
    MEM16(st + 10) = (uint16_t)mi;
    MEM16(st + 12) = (uint16_t)s;
    MEM16(st + 14) = (uint16_t)ms;
}

static uint64_t ft_get(uint32_t a)
{
    return MEM32(a) | (uint64_t)MEM32(a + 4) << 32;
}

static void test_filetime(void)
{
    /* 2007-11-20 13:45:30.250 UTC (a Tuesday) = Unix 1195566330.25. */
    st_set(G_ST, 2007, 11, 20, 13, 45, 30, 250);
    CHECK(CALL("SystemTimeToFileTime", G_ST, G_FT) == W32_TRUE);
    CHECK(ft_get(G_FT) == 128400399302500000ull);
    memset(guest_ptr(G_ST), 0xFF, 16);
    CHECK(CALL("FileTimeToSystemTime", G_FT, G_ST) == W32_TRUE);
    static const uint16_t want[8] = {2007, 11, 2, 20, 13, 45, 30, 250};
    for (int i = 0; i < 8; i++)
        CHECK(MEM16(G_ST + 2u * i) == want[i]);

    MEM32(G_FT2) = 0;
    MEM32(G_FT2 + 4) = 0;
    CHECK(CALL("FileTimeToSystemTime", G_FT2, G_ST) == W32_TRUE);
    CHECK(MEM16(G_ST) == 1601 && MEM16(G_ST + 2) == 1 && MEM16(G_ST + 4) == 1 && MEM16(G_ST + 6) == 1);
    MEM32(G_FT2 + 4) = 0x80000000u;
    CHECK(CALL("FileTimeToSystemTime", G_FT2, G_ST) == W32_FALSE && w32_last_error() == 87u);
    st_set(G_ST, 2007, 2, 29, 0, 0, 0, 0);
    CHECK(CALL("SystemTimeToFileTime", G_ST, G_FT2) == W32_FALSE && w32_last_error() == 87u);
    st_set(G_ST, 2000, 2, 29, 23, 59, 59, 999);
    CHECK(CALL("SystemTimeToFileTime", G_ST, G_FT2) == W32_TRUE);
    CHECK(ft_get(G_FT2) == (951868799ull * 10000000ull + 116444736000000000ull + 9990000ull));

    CHECK(CALL("CompareFileTime", G_FT, G_FT2) == 1u);
    CHECK(CALL("CompareFileTime", G_FT2, G_FT) == 0xFFFFFFFFu);
    CHECK(CALL("CompareFileTime", G_FT, G_FT) == 0u);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    CHECK(CALL("FileTimeToLocalFileTime", G_FT, G_FT2) == W32_TRUE);
    CHECK((int64_t)(ft_get(G_FT2) - ft_get(G_FT)) == (int64_t)lt.tm_gmtoff * 10000000);

    CALL("GetSystemTimeAsFileTime", G_FT);
    uint64_t unix_s = (ft_get(G_FT) - 116444736000000000ull) / 10000000ull;
    CHECK(unix_s + 2 >= (uint64_t)now && unix_s <= (uint64_t)now + 2);
    CALL("GetSystemTime", G_ST);
    struct tm ut;
    gmtime_r(&now, &ut);
    CHECK(MEM16(G_ST) == ut.tm_year + 1900 && MEM16(G_ST + 2) == ut.tm_mon + 1);
    CHECK(MEM16(G_ST + 4) == ut.tm_wday);
}

/* GetTimeZoneInformation for one zone; returns its result. */
static uint32_t tzi_for(const char *zone)
{
    setenv("TZ", zone, 1);
    tzset();
    return CALL("GetTimeZoneInformation", G_TZI);
}

static void check_rule(uint32_t st, unsigned month, unsigned week, unsigned hour)
{
    CHECK(MEM16(st + 2) == month && MEM16(st + 4) == 0 && MEM16(st + 6) == week);
    CHECK(MEM16(st + 8) == hour && MEM16(st + 10) == 0);
}

static void test_time_zone(void)
{
    time_t now = time(NULL);
    struct tm lt;
    uint32_t r = tzi_for("Europe/Madrid");
    localtime_r(&now, &lt);
    CHECK(r == (lt.tm_isdst > 0 ? 2u : 1u));
    CHECK(MEM32(G_TZI) == (uint32_t)-60 && MEM32(G_TZI + 0x54) == 0 && MEM32(G_TZI + 0xA8) == (uint32_t)-60);
    check_rule(G_TZI + 0x98, 3, 5, 2);  /* last Sunday of March, 02:00 CET */
    check_rule(G_TZI + 0x44, 10, 5, 3); /* last Sunday of October, 03:00 CEST */
    CHECK(MEM16(G_TZI + 4) == 'C' && MEM16(G_TZI + 6) == 'E' && MEM16(G_TZI + 8) == 'T' &&
          MEM16(G_TZI + 10) == 0);
    CHECK(MEM16(G_TZI + 0x58) == 'C' && MEM16(G_TZI + 0x5E) == 'T' && MEM16(G_TZI + 0x60) == 0);

    r = tzi_for("America/New_York");
    localtime_r(&now, &lt);
    CHECK(r == (lt.tm_isdst > 0 ? 2u : 1u));
    CHECK(MEM32(G_TZI) == 300u && MEM32(G_TZI + 0xA8) == (uint32_t)-60);
    check_rule(G_TZI + 0x98, 3, 2, 2);  /* second Sunday of March, 02:00 EST */
    check_rule(G_TZI + 0x44, 11, 1, 2); /* first Sunday of November, 02:00 EDT */

    CHECK(tzi_for("UTC") == 0u);
    CHECK(MEM32(G_TZI) == 0 && MEM16(G_TZI + 0x98 + 2) == 0 && MEM16(G_TZI + 0x44 + 2) == 0);
    unsetenv("TZ");
    tzset();
}

static void test_scheduling(void)
{
    CHECK(CALL0("GetCurrentThread") == W32_PSEUDO_CURRENT_THREAD);
    CHECK(CALL("SetThreadPriority", W32_PSEUDO_CURRENT_THREAD, 2) == W32_TRUE);
    uint32_t mask = CALL("SetThreadAffinityMask", W32_PSEUDO_CURRENT_THREAD, 1);
    CHECK(mask != 0 && mask <= 0xFFu && (mask & (mask + 1)) == 0);
    CHECK(CALL("SetProcessAffinityMask", W32_PSEUDO_CURRENT_PROCESS, 1) == W32_TRUE);
    CHECK(CALL("GetProcessAffinityMask", W32_PSEUDO_CURRENT_PROCESS, G_CELL, G_CELL + 4) == W32_TRUE);
    CHECK(MEM32(G_CELL) == mask && MEM32(G_CELL + 4) == mask);
    CHECK(CALL("SetThreadExecutionState", 0x80000003u) == 0x80000000u);
}

int main(void)
{
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00100000u, PROT_READ | PROT_WRITE) == 0);
    g_esp = MAIN_TOP;
    w32_tib_attach(MAIN_TOP, MAIN_LIMIT);

    test_table();
    test_tib_and_tls();
    test_threads();
    test_fls_exit_callback();
    test_host_threads();
    test_events_and_waits();
    test_reset_and_pulse();
    test_mutex_and_semaphore();
    test_critical_section();
    test_interlocked();
    test_clocks();
    test_filetime();
    test_time_zone();
    test_scheduling();

    /* Every thunk but ExitThread went through call(); ExitThread does not
     * return, and start_exit covered it. */
    for (unsigned i = 0; i < N_EXPECT; i++) {
        if (!g_called[i] && strcmp(g_expect[i].name, "ExitThread") != 0) {
            fprintf(stderr, "FAIL: %s never called\n", g_expect[i].name);
            return 1;
        }
    }
    printf("ok: 44 thunks, 43 stack checks\n");
    return 0;
}
