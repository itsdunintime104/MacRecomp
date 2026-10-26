/* winmm slice: timeGetTime, timeSetEvent and timeKillEvent, which LEGO
 * Indiana Jones imports (notes/win32_imports.md, "LEGOIndy.exe imports").
 * Contract: runtime/include/win32_thunk.h and docs/WIN32_THUNKS.md.
 *
 * Each timer runs on its own host thread. A TIME_CALLBACK_FUNCTION timer
 * calls the guest's stdcall LPTIMECALLBACK(uTimerID, uMsg, dwUser, dw1, dw2)
 * on that thread after w32_host_thread_attach, as a guest thread would run
 * it. The game itself uses TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE,
 * which pulses a manual-reset event. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "recomp_types.h"
#include "guest_call.h"
#include "diag.h"
#include "win32_thunk.h"

#define MM_TIME_PERIODIC 0x0001u
#define MM_TIME_CALLBACK_EVENT_SET 0x0010u
#define MM_TIME_CALLBACK_EVENT_PULSE 0x0020u
#define MM_TIME_CALLBACK_MASK 0x0030u
#define MM_TIME_KILL_SYNCHRONOUS 0x0100u
#define MM_TIMERR_NOERROR 0u
#define MM_MMSYSERR_INVALPARAM 11u

struct mm_timer {
    struct mm_timer *next; /* g_timers: timers that can still be killed */
    uint32_t id, delay_ms, flags, proc, user;
    int killed;    /* timeKillEvent ran: stop after the current callback */
    int self_free; /* the timer thread frees this struct when it ends */
    pthread_t thread;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct mm_timer *g_timers;
static uint32_t g_next_id = 1;

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Under g_lock. */
static void unlink_timer(struct mm_timer *t)
{
    for (struct mm_timer **p = &g_timers; *p != NULL; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            return;
        }
    }
}

static void fire(const struct mm_timer *t)
{
    switch (t->flags & MM_TIME_CALLBACK_MASK) {
    case MM_TIME_CALLBACK_EVENT_SET: w32_event_set(t->proc, 0); break;
    case MM_TIME_CALLBACK_EVENT_PULSE: w32_event_set(t->proc, 1); break;
    default: {
        uint32_t args[5] = {t->id, 0, t->user, 0, 0};
        guest_call_cdecl(t->proc, 5, args);
        break;
    }
    }
}

static void *timer_main(void *p)
{
    struct mm_timer *t = p;
    if ((t->flags & MM_TIME_CALLBACK_MASK) == 0)
        w32_host_thread_attach(); /* guest code runs here */
    uint64_t period = (uint64_t)t->delay_ms * 1000000u;
    pthread_mutex_lock(&g_lock);
    uint64_t due = now_ns() + period;
    for (;;) {
        uint64_t now;
        while (!t->killed && (now = now_ns()) < due) {
            struct timespec rel = {(time_t)((due - now) / 1000000000u),
                                   (long)((due - now) % 1000000000u)};
            pthread_cond_timedwait_relative_np(&g_cond, &g_lock, &rel);
        }
        if (t->killed)
            break;
        pthread_mutex_unlock(&g_lock);
        fire(t);
        pthread_mutex_lock(&g_lock);
        if (t->killed)
            break;
        if ((t->flags & MM_TIME_PERIODIC) == 0) {
            /* One-shot: its id is no longer valid once it has fired. */
            unlink_timer(t);
            t->self_free = 1;
            pthread_detach(t->thread);
            break;
        }
        due += period;
        now = now_ns();
        if (due < now) /* a callback ran longer than the period */
            due = now;
    }
    int self_free = t->self_free;
    pthread_mutex_unlock(&g_lock);
    if (self_free)
        free(t);
    return NULL;
}

/* DWORD timeGetTime(void) */
static void imp_timeGetTime(void)
{
    W32_RETURN(0, w32_tick_ms());
}

/* MMRESULT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK proc,
 * DWORD_PTR user, UINT flags): the timer id, or 0. resolution is ignored. */
static void imp_timeSetEvent(void)
{
    uint32_t delay = W32_ARG(0), proc = W32_ARG(2), user = W32_ARG(3), flags = W32_ARG(4);
    uint32_t kind = flags & MM_TIME_CALLBACK_MASK;
    if (delay == 0 || proc == 0 || kind == MM_TIME_CALLBACK_MASK ||
        (flags & ~(MM_TIME_PERIODIC | MM_TIME_CALLBACK_MASK | MM_TIME_KILL_SYNCHRONOUS)) != 0) {
        diag_unsupported("winmm.dll!timeSetEvent", MEM32(g_esp),
                         "delay %u, proc 0x%08X, flags 0x%X not supported", delay, proc, flags);
        W32_RETURN(5, 0);
        return;
    }
    struct mm_timer *t = calloc(1, sizeof *t);
    if (t == NULL) {
        W32_RETURN(5, 0);
        return;
    }
    t->delay_ms = delay, t->flags = flags, t->proc = proc, t->user = user;
    pthread_mutex_lock(&g_lock);
    t->id = g_next_id++;
    if (pthread_create(&t->thread, NULL, timer_main, t) != 0) {
        pthread_mutex_unlock(&g_lock);
        free(t);
        W32_RETURN(5, 0);
        return;
    }
    t->next = g_timers;
    g_timers = t;
    uint32_t id = t->id;
    pthread_mutex_unlock(&g_lock);
    W32_RETURN(5, id);
}

/* MMRESULT timeKillEvent(UINT id): stops the timer; returns after a callback
 * that is running has finished, unless the callback itself kills it. */
static void imp_timeKillEvent(void)
{
    uint32_t id = W32_ARG(0);
    pthread_mutex_lock(&g_lock);
    struct mm_timer *t = g_timers;
    while (t != NULL && t->id != id)
        t = t->next;
    if (t == NULL) {
        pthread_mutex_unlock(&g_lock);
        W32_RETURN(1, MM_MMSYSERR_INVALPARAM);
        return;
    }
    unlink_timer(t);
    t->killed = 1;
    pthread_cond_broadcast(&g_cond);
    int own = pthread_equal(t->thread, pthread_self());
    if (own) {
        t->self_free = 1;
        pthread_detach(t->thread);
    }
    pthread_mutex_unlock(&g_lock);
    if (!own) {
        pthread_join(t->thread, NULL);
        free(t);
    }
    W32_RETURN(1, MM_TIMERR_NOERROR);
}

const win32_export_t win32_exports_winmm[] = {
    {"WINMM.dll", "timeGetTime", 0, imp_timeGetTime},
    {"WINMM.dll", "timeKillEvent", 0, imp_timeKillEvent},
    {"WINMM.dll", "timeSetEvent", 0, imp_timeSetEvent},
    {0},
};
