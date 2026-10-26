/* winmm slice test: timeGetTime, timeSetEvent and timeKillEvent, resolved
 * through the core resolver as the loader binds them. Callbacks are host
 * functions registered as guest code; they check that they run on a thread
 * with a TIB, as a guest thread would. Links with win32_winmm.c,
 * win32_sync.c, win32_core.c, win32_unsupported.c and the engine runtime. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "win32_thunk.h"

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define MAIN_TOP 0x01000000u
#define MAIN_LIMIT 0x00FF0000u
#define WAIT_TIMEOUT 0x102u
#define TIME_ONESHOT 0x00u
#define TIME_PERIODIC 0x01u
#define TIME_CALLBACK_EVENT_SET 0x10u
#define TIME_CALLBACK_EVENT_PULSE 0x20u

static uint64_t now_ms(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u;
}

static recomp_func_t import(const char *dll, const char *name)
{
    uint32_t va = win32_resolve_import(dll, name, 0, 0);
    CHECK(va != 0);
    return guest_resolve(va);
}

/* Call fn as lifted code does; check the stdcall pop and callee-saved
 * registers. */
static uint32_t call(recomp_func_t fn, unsigned n, const uint32_t *args)
{
    uint32_t sp = g_esp;
    for (unsigned i = n; i-- > 0;)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0x12345678u);
    uint32_t before = g_esp;
    g_ebx = 0xB0B0B0B0u, g_esi = 0x51515151u, g_edi = 0xD1D1D1D1u, g_ebp = 0xEBEBEBEBu;
    fn();
    CHECK(g_esp == before + 4u + 4u * n);
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xEBEBEBEBu);
    g_esp = sp;
    return g_eax;
}
#define CALL(fn, ...) call(fn, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})

static recomp_func_t f_get, f_set, f_kill, f_tick, f_create_event, f_wait;

/* ---- Guest callback: LPTIMECALLBACK(id, msg, user, dw1, dw2), stdcall -- */

static _Atomic int g_fires, g_bad, g_sleep_ms, g_in_cb, g_cb_done;
static _Atomic uint32_t g_expect_id;
static uint64_t g_times[64];

static void timer_cb(void)
{
    int k = atomic_fetch_add(&g_fires, 1);
    if (k < 64)
        g_times[k] = now_ms();
    /* A TIB like a guest thread's: fs:[0x18] is the TIB itself. */
    if (g_fs_base == 0 || MEM32(g_fs_base + 0x18) != g_fs_base)
        g_bad = 1;
    if (W32_ARG(0) != g_expect_id || W32_ARG(1) != 0 || W32_ARG(2) != 0x1234u ||
        W32_ARG(3) != 0 || W32_ARG(4) != 0)
        g_bad = 2;
    if (g_sleep_ms) {
        g_in_cb = 1;
        usleep((useconds_t)g_sleep_ms * 1000u);
        g_cb_done = 1;
    }
    W32_RETURN(5, 0);
}

static void reset_cb(void)
{
    g_fires = 0, g_bad = 0, g_sleep_ms = 0, g_in_cb = 0, g_cb_done = 0;
}

static void test_time_get_time(void)
{
    uint32_t a = call(f_get, 0, NULL), t = call(f_tick, 0, NULL);
    CHECK(t - a <= 1u); /* GetTickCount's clock */
    uint32_t prev = a;
    for (int i = 0; i < 1000; i++) {
        uint32_t v = call(f_get, 0, NULL);
        CHECK(v >= prev);
        prev = v;
    }
    usleep(50000);
    uint32_t b = call(f_get, 0, NULL);
    CHECK(b - a >= 49u && b - a < 250u);
}

static void test_periodic(void)
{
    reset_cb();
    uint32_t cb = host_func_va(timer_cb);
    uint64_t t0 = now_ms();
    uint32_t id = CALL(f_set, 20, 1, cb, 0x1234u, TIME_PERIODIC);
    g_expect_id = id;
    CHECK(id != 0);
    usleep(230000);
    CHECK(CALL(f_kill, id) == 0);
    int n = g_fires;
    CHECK(n >= 7 && n <= 12);
    CHECK(g_bad == 0);
    CHECK(g_times[0] - t0 >= 19u && g_times[0] - t0 < 80u);
    uint64_t span = g_times[n - 1] - g_times[0];
    CHECK(span >= 17u * (uint64_t)(n - 1) && span <= 30u * (uint64_t)(n - 1));
    usleep(80000);
    CHECK(g_fires == n);              /* stopped */
    CHECK(CALL(f_kill, id) == 11u);   /* MMSYSERR_INVALPARAM */
    CHECK(CALL(f_kill, 0xBADu) == 11u);
}

static void test_oneshot(void)
{
    reset_cb();
    uint32_t id = CALL(f_set, 30, 1, host_func_va(timer_cb), 0x1234u, TIME_ONESHOT);
    g_expect_id = id;
    CHECK(id != 0);
    usleep(200000);
    CHECK(g_fires == 1 && g_bad == 0);
    CHECK(CALL(f_kill, id) == 11u); /* a fired one-shot timer is gone */
}

/* timeKillEvent returns only after a running callback has finished. */
static void test_kill_waits(void)
{
    reset_cb();
    g_sleep_ms = 100;
    uint32_t id = CALL(f_set, 10, 1, host_func_va(timer_cb), 0x1234u, TIME_PERIODIC);
    g_expect_id = id;
    while (!g_in_cb)
        sched_yield();
    CHECK(CALL(f_kill, id) == 0);
    CHECK(g_cb_done == 1 && g_fires == 1);
}

struct waiter {
    uint32_t h, r;
    uint64_t ms;
    _Atomic int started;
};

static void *wait_main(void *p)
{
    struct waiter *w = p;
    w32_host_thread_attach();
    w->started = 1;
    uint64_t t = now_ms();
    w->r = CALL(f_wait, w->h, 3000);
    w->ms = now_ms() - t;
    return NULL;
}

/* The game's use: TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE on a
 * manual-reset event that another thread waits on. */
static void test_event_pulse(void)
{
    uint32_t ev = CALL(f_create_event, 0, 1, 0, 0);
    CHECK(ev != 0);
    struct waiter w = {ev, 0xDEADu, 0, 0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, wait_main, &w) == 0);
    while (!w.started)
        sched_yield();
    usleep(50000);
    uint32_t id = CALL(f_set, 100, 100, ev, 0, TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE);
    CHECK(id != 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(w.r == 0 && w.ms >= 100u && w.ms < 600u);
    CHECK(CALL(f_wait, ev, 0) == WAIT_TIMEOUT); /* a pulse leaves it unsignalled */
    CHECK(CALL(f_wait, ev, 400) == 0); /* a later waiter is released by the next pulse */
    CHECK(CALL(f_kill, id) == 0);
    CHECK(w32_handle_close(ev));
}

static void test_event_set(void)
{
    uint32_t ev = CALL(f_create_event, 0, 0, 0, 0);
    uint32_t id = CALL(f_set, 20, 1, ev, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
    CHECK(id != 0);
    CHECK(CALL(f_wait, ev, 1000) == 0);
    CHECK(w32_handle_close(ev));
    /* Both event kinds at once is not a valid flag set. */
    CHECK(CALL(f_set, 20, 1, ev, 0, TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE) == 0);
    CHECK(CALL(f_set, 0, 1, host_func_va(timer_cb), 0, TIME_ONESHOT) == 0);
}

int main(void)
{
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00100000u, PROT_READ | PROT_WRITE) == 0);
    g_esp = MAIN_TOP;
    w32_tib_attach(MAIN_TOP, MAIN_LIMIT);

    f_get = import("winmm.dll", "timeGetTime");
    f_set = import("winmm.dll", "timeSetEvent");
    f_kill = import("winmm.dll", "timeKillEvent");
    f_tick = import("KERNEL32.dll", "GetTickCount");
    f_create_event = import("KERNEL32.dll", "CreateEventA");
    f_wait = import("KERNEL32.dll", "WaitForSingleObject");
    int n = 0;
    for (const win32_export_t *e = win32_exports_winmm; e->dll != NULL; e++, n++)
        CHECK(guest_resolve(win32_resolve_import("winmm.dll", e->name, 0, 0)) == e->fn);
    CHECK(n == 3);

    test_time_get_time();
    test_periodic();
    test_oneshot();
    test_kill_waits();
    test_event_pulse();
    test_event_set();
    printf("ok: 3 winmm thunks; periodic, one-shot, kill-waits, event pulse and set\n");
    return 0;
}
