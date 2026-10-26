/* Win32 sync slice: threads and the TIB, waitable objects, critical sections,
 * interlocked operations, TLS slots, time and scheduling (notes/slice_owners.md,
 * slice "sync"). Contract: runtime/include/win32_thunk.h and
 * docs/WIN32_THUNKS.md.
 *
 * Waitable objects (event, mutex, semaphore, thread) keep their state in one
 * struct sync_obj each. Every state change and every wait happens under one
 * process-wide lock, and every change broadcasts one condition variable, so a
 * wait-all can test and take all its objects at once. A handle is a small
 * struct sync_handle that points at the shared object: a named object opened
 * twice gets two handles, and a running thread keeps its own object alive
 * after the guest closes the thread handle.
 *
 * ExitThread: the trampoline of every CreateThread thread calls setjmp
 * before it enters the guest start routine. ExitThread longjmps back there.
 * The lifted C frames in between are plain C frames with no cleanup that must
 * run, and the trampoline does not read g_esp afterwards. */
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_heap.h"
#include "guest_call.h"
#include "diag.h"
#include "game_hooks.h"
#include "win32_thunk.h"

/* No-op defaults, as in thunk_threads.c: diag.c has the strong definitions. */
__attribute__((weak)) void diag_thread_register(void) {}
__attribute__((weak)) void diag_thread_unregister(void) {}
/* Default game hook (game_hooks.h): no patched Sleep call sites. */
__attribute__((weak)) uint32_t game_patch_sleep_ms(uint32_t guest_ret, uint32_t ms)
{
    (void)guest_ret;
    return ms;
}

#define W32_INFINITE 0xFFFFFFFFu
#define W32_WAIT_OBJECT_0 0x000u
#define W32_WAIT_ABANDONED_0 0x080u
#define W32_WAIT_TIMEOUT 0x102u
#define W32_WAIT_FAILED 0xFFFFFFFFu
#define W32_MAXIMUM_WAIT_OBJECTS 64u
#define W32_STILL_ACTIVE 259u
#define W32_CREATE_SUSPENDED 0x4u
#define W32_TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define W32_ERROR_NO_MORE_ITEMS 259u
#define W32_ERROR_NOT_OWNER 288u
#define W32_ERROR_TOO_MANY_POSTS 298u
#define W32_ES_CONTINUOUS 0x80000000u
#define W32_TIME_ZONE_ID_UNKNOWN 0u
#define W32_TIME_ZONE_ID_STANDARD 1u
#define W32_TIME_ZONE_ID_DAYLIGHT 2u

/* TIB layout (docs/WIN32_THUNKS.md). The TLS slots sit where a real TEB keeps
 * TlsSlots. */
#define TIB_SIZE 0x1000u
#define TIB_SEH_HEAD 0x00u
#define TIB_STACK_BASE 0x04u
#define TIB_STACK_LIMIT 0x08u
#define TIB_SELF 0x18u
#define TIB_PID 0x20u
#define TIB_TID 0x24u
#define TIB_TLS_PTR 0x2Cu
#define TIB_LAST_ERROR 0x34u
#define TIB_TLS_SLOTS 0xE10u
#define TLS_SLOT_COUNT 64u
#define MAX_TIBS 256u

#define FIRST_TID 0x100u
#define HOST_STACKSIZE (8u << 20) /* lifted code recurses on the host stack */
#define TICK_OFFSET_MS 3600000u    /* GetTickCount starts at one hour */

/* FILETIME: 100 ns ticks since 1601-01-01 UTC. */
#define FT_PER_SEC 10000000ull
#define FT_UNIX_EPOCH 116444736000000000ull
#define DAYS_1601_TO_1970 134774

/* ---- Waitable objects --------------------------------------------------- */

struct sync_obj {
    enum w32_kind kind;
    int refs;              /* handles, plus 1 while a thread runs */
    char *name;            /* malloc'd, or NULL */
    struct sync_obj *next; /* g_objs */
    int signalled;         /* EVENT: set; THREAD: exited */
    int manual;            /* EVENT: manual reset */
    uint32_t pulses;       /* EVENT: w32_event_set(h, 1) calls so far */
    int pulse_credit;      /* EVENT: waiters the last pulse may still release; -1: all */
    int32_t count, max;    /* SEMAPHORE */
    uint32_t owner;        /* MUTEX: owning thread id, 0 when free */
    uint32_t recursion;    /* MUTEX */
    int abandoned;         /* MUTEX: owner exited while holding it */
    uint32_t tid;          /* THREAD */
    uint32_t start, param; /* THREAD */
    uint32_t suspend;      /* THREAD: suspend count */
    uint32_t exit_code;    /* THREAD */
};

struct sync_handle {
    struct w32_object hdr;
    struct sync_obj *obj;
};

/* Guards every sync_obj, g_objs, the TLS bitmap and g_tibs. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct sync_obj *g_objs;

static _Atomic uint32_t g_next_tid = FIRST_TID;
static __thread uint32_t t_tid;
static __thread jmp_buf *t_exit_jmp; /* set by thread_main only */
static __thread uint32_t t_exit_code;

static uint32_t cur_tid(void)
{
    if (t_tid == 0)
        t_tid = atomic_fetch_add(&g_next_tid, 1);
    return t_tid;
}

/* Under g_lock. */
static void obj_unref_locked(struct sync_obj *o)
{
    if (--o->refs > 0)
        return;
    for (struct sync_obj **p = &g_objs; *p != NULL; p = &(*p)->next) {
        if (*p == o) {
            *p = o->next;
            break;
        }
    }
    free(o->name);
    free(o);
}

static void sync_handle_destroy(struct w32_object *self)
{
    struct sync_handle *sh = (struct sync_handle *)self;
    pthread_mutex_lock(&g_lock);
    obj_unref_locked(sh->obj);
    pthread_mutex_unlock(&g_lock);
    free(sh);
}

/* A new handle for o. Takes one reference on o; the caller holds no lock.
 * Returns 0 when out of memory or handles. */
static uint32_t handle_for(struct sync_obj *o)
{
    struct sync_handle *sh = calloc(1, sizeof *sh);
    uint32_t h = 0;
    pthread_mutex_lock(&g_lock);
    o->refs++;
    pthread_mutex_unlock(&g_lock);
    if (sh != NULL) {
        sh->hdr.kind = o->kind;
        sh->hdr.destroy = sync_handle_destroy;
        sh->obj = o;
        h = w32_handle_new(&sh->hdr);
    }
    if (h == 0) {
        free(sh);
        pthread_mutex_lock(&g_lock);
        obj_unref_locked(o);
        pthread_mutex_unlock(&g_lock);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    return h;
}

/* The object behind h when it is one of kind (or any waitable kind for
 * W32_KIND_ANY), else NULL. */
static struct sync_obj *obj_get(uint32_t h, enum w32_kind kind)
{
    struct w32_object *w = w32_handle_get(h, kind);
    if (w == NULL || w->destroy != sync_handle_destroy)
        return NULL;
    return ((struct sync_handle *)w)->obj;
}

/* Create a named or unnamed object and return a handle to it. A name already
 * in use returns a new handle to that object with ERROR_ALREADY_EXISTS, or
 * fails with ERROR_INVALID_HANDLE when the object is of another kind.
 * init fills a new object; it is not called for an existing one. */
static uint32_t obj_create(enum w32_kind kind, uint32_t name_va,
                           void (*init)(struct sync_obj *, uint32_t, uint32_t),
                           uint32_t a, uint32_t b)
{
    const char *name = name_va != 0 ? (const char *)guest_ptr(name_va) : NULL;
    struct sync_obj *o = NULL;
    pthread_mutex_lock(&g_lock);
    if (name != NULL) {
        for (struct sync_obj *p = g_objs; p != NULL; p = p->next) {
            if (p->name != NULL && strcmp(p->name, name) == 0) {
                o = p;
                break;
            }
        }
    }
    if (o != NULL) {
        enum w32_kind found = o->kind;
        if (found == kind)
            o->refs++; /* keep it alive until handle_for takes its own */
        pthread_mutex_unlock(&g_lock);
        if (found != kind) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return 0;
        }
        uint32_t h = handle_for(o);
        pthread_mutex_lock(&g_lock);
        obj_unref_locked(o);
        pthread_mutex_unlock(&g_lock);
        if (h != 0)
            w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
        return h;
    }
    o = calloc(1, sizeof *o);
    char *copy = name != NULL ? strdup(name) : NULL;
    if (o == NULL || (name != NULL && copy == NULL)) {
        pthread_mutex_unlock(&g_lock);
        free(o);
        free(copy);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    o->kind = kind;
    o->name = copy;
    init(o, a, b);
    o->refs = 1; /* dropped below, once handle_for holds its own */
    o->next = g_objs;
    g_objs = o;
    pthread_mutex_unlock(&g_lock);
    uint32_t h = handle_for(o);
    pthread_mutex_lock(&g_lock);
    obj_unref_locked(o);
    pthread_mutex_unlock(&g_lock);
    if (h != 0)
        w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

/* Under g_lock: can thread me take o now? */
/* seen: o->pulses when this wait began. A pulse releases only waiters that
 * were already waiting when it happened (seen differs). */
static int obj_ready(const struct sync_obj *o, uint32_t me, uint32_t seen)
{
    switch (o->kind) {
    case W32_KIND_EVENT: return o->signalled || (o->pulses != seen && o->pulse_credit != 0);
    case W32_KIND_THREAD: return o->signalled;
    case W32_KIND_SEMAPHORE: return o->count > 0;
    case W32_KIND_MUTEX: return o->owner == 0 || o->owner == me;
    default: return 0;
    }
}

/* Under g_lock, after obj_ready: take o. Returns 1 for an abandoned mutex. */
static int obj_take(struct sync_obj *o, uint32_t me)
{
    int abandoned = 0;
    switch (o->kind) {
    case W32_KIND_EVENT:
        if (o->signalled) {
            if (!o->manual)
                o->signalled = 0;
        } else if (o->pulse_credit > 0) { /* released by a pulse */
            o->pulse_credit--;
        }
        break;
    case W32_KIND_SEMAPHORE: o->count--; break;
    case W32_KIND_MUTEX:
        o->owner = me;
        o->recursion++;
        abandoned = o->abandoned;
        o->abandoned = 0;
        break;
    default: break;
    }
    return abandoned;
}

/* Under g_lock: the WAIT_* result if the wait is satisfied now, else
 * W32_WAIT_TIMEOUT. Wait-all takes every object or none. */
static uint32_t wait_try(struct sync_obj **o, uint32_t n, int all, uint32_t me,
                         const uint32_t *seen)
{
    if (!all) {
        for (uint32_t i = 0; i < n; i++)
            if (obj_ready(o[i], me, seen[i]))
                return (obj_take(o[i], me) ? W32_WAIT_ABANDONED_0 : W32_WAIT_OBJECT_0) + i;
        return W32_WAIT_TIMEOUT;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!obj_ready(o[i], me, seen[i]))
            return W32_WAIT_TIMEOUT;
    uint32_t r = W32_WAIT_OBJECT_0;
    for (uint32_t i = 0; i < n; i++)
        if (obj_take(o[i], me) && r == W32_WAIT_OBJECT_0)
            r = W32_WAIT_ABANDONED_0 + i;
    return r;
}

/* mach_absolute_time in nanoseconds. */
static uint64_t mono_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0)
        mach_timebase_info(&tb);
    return (uint64_t)((unsigned __int128)mach_absolute_time() * tb.numer / tb.denom);
}

static uint32_t wait_objects(const uint32_t *handles, uint32_t n, int all, uint32_t ms)
{
    struct sync_obj *o[W32_MAXIMUM_WAIT_OBJECTS];
    uint32_t seen[W32_MAXIMUM_WAIT_OBJECTS];
    if (n == 0 || n > W32_MAXIMUM_WAIT_OBJECTS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_WAIT_FAILED;
    }
    for (uint32_t i = 0; i < n; i++) {
        o[i] = obj_get(handles[i], W32_KIND_ANY);
        if (o[i] == NULL) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_WAIT_FAILED;
        }
    }
    uint32_t me = cur_tid();
    uint64_t deadline = mono_ns() + (uint64_t)ms * 1000000u;
    uint32_t r;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < n; i++) {
        o[i]->refs++; /* a CloseHandle during the wait must not free it */
        seen[i] = o[i]->pulses;
    }
    while ((r = wait_try(o, n, all, me, seen)) == W32_WAIT_TIMEOUT) {
        if (ms == W32_INFINITE) {
            pthread_cond_wait(&g_cond, &g_lock);
            continue;
        }
        uint64_t now = mono_ns();
        if (now >= deadline)
            break;
        struct timespec rel = {(time_t)((deadline - now) / 1000000000u),
                               (long)((deadline - now) % 1000000000u)};
        pthread_cond_timedwait_relative_np(&g_cond, &g_lock, &rel);
    }
    for (uint32_t i = 0; i < n; i++)
        obj_unref_locked(o[i]);
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* DWORD WaitForSingleObject(HANDLE, DWORD ms) */
static void imp_WaitForSingleObject(void)
{
    uint32_t h = W32_ARG(0);
    W32_RETURN(2, wait_objects(&h, 1, 0, W32_ARG(1)));
}

/* Copy n guest handles (n is checked by wait_objects; copy at most 64). */
static uint32_t wait_multiple(uint32_t n, uint32_t arr, uint32_t all, uint32_t ms)
{
    uint32_t h[W32_MAXIMUM_WAIT_OBJECTS];
    for (uint32_t i = 0; i < n && i < W32_MAXIMUM_WAIT_OBJECTS; i++)
        h[i] = MEM32(arr + 4u * i);
    return wait_objects(h, n, all != 0, ms);
}

/* DWORD WaitForMultipleObjects(DWORD n, const HANDLE *, BOOL all, DWORD ms) */
static void imp_WaitForMultipleObjects(void)
{
    W32_RETURN(4, wait_multiple(W32_ARG(0), W32_ARG(1), W32_ARG(2), W32_ARG(3)));
}

/* DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *, BOOL all, DWORD ms,
 * BOOL alertable): no APCs are ever queued, so alertable changes nothing. */
static void imp_WaitForMultipleObjectsEx(void)
{
    W32_RETURN(5, wait_multiple(W32_ARG(0), W32_ARG(1), W32_ARG(2), W32_ARG(3)));
}

/* ---- Events, mutexes, semaphores ---------------------------------------- */

static void event_init(struct sync_obj *o, uint32_t manual, uint32_t initial)
{
    o->manual = manual != 0;
    o->signalled = initial != 0;
}

/* HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) */
static void imp_CreateEventA(void)
{
    W32_RETURN(4, obj_create(W32_KIND_EVENT, W32_ARG(3), event_init, W32_ARG(1), W32_ARG(2)));
}

uint32_t w32_event_set(uint32_t h, int pulse)
{
    struct sync_obj *o = obj_get(h, W32_KIND_EVENT);
    if (o == NULL) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    pthread_mutex_lock(&g_lock);
    if (pulse) {
        /* Release the threads waiting now (all, or one for auto reset);
         * the event itself ends unsignalled. */
        o->pulses++;
        o->pulse_credit = o->manual ? -1 : 1;
        o->signalled = 0;
    } else {
        o->signalled = 1;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return W32_TRUE;
}

/* BOOL SetEvent(HANDLE) */
static void imp_SetEvent(void)
{
    W32_RETURN(1, w32_event_set(W32_ARG(0), 0));
}

/* BOOL ResetEvent(HANDLE) */
static void imp_ResetEvent(void)
{
    struct sync_obj *o = obj_get(W32_ARG(0), W32_KIND_EVENT);
    if (o != NULL) {
        pthread_mutex_lock(&g_lock);
        o->signalled = 0;
        pthread_mutex_unlock(&g_lock);
    } else {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    }
    W32_RETURN(1, o != NULL ? W32_TRUE : W32_FALSE);
}

/* a = initial owner flag, b = the creating thread's id. */
static void mutex_init(struct sync_obj *o, uint32_t initial_owner, uint32_t me)
{
    if (initial_owner) {
        o->owner = me;
        o->recursion = 1;
    }
}

/* HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initial_owner, LPCSTR) */
static void imp_CreateMutexA(void)
{
    W32_RETURN(3, obj_create(W32_KIND_MUTEX, W32_ARG(2), mutex_init, W32_ARG(1), cur_tid()));
}

/* BOOL ReleaseMutex(HANDLE) */
static void imp_ReleaseMutex(void)
{
    struct sync_obj *o = obj_get(W32_ARG(0), W32_KIND_MUTEX);
    uint32_t err = W32_ERROR_INVALID_HANDLE;
    if (o != NULL) {
        pthread_mutex_lock(&g_lock);
        err = W32_ERROR_NOT_OWNER;
        if (o->owner == cur_tid()) {
            if (--o->recursion == 0) {
                o->owner = 0;
                pthread_cond_broadcast(&g_cond);
            }
            err = W32_ERROR_SUCCESS;
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (err != W32_ERROR_SUCCESS)
        w32_set_last_error(err);
    W32_RETURN(1, err == W32_ERROR_SUCCESS ? W32_TRUE : W32_FALSE);
}

static void semaphore_init(struct sync_obj *o, uint32_t initial, uint32_t max)
{
    o->count = (int32_t)initial;
    o->max = (int32_t)max;
}

/* HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG max, LPCSTR) */
static void imp_CreateSemaphoreA(void)
{
    int32_t initial = (int32_t)W32_ARG(1), max = (int32_t)W32_ARG(2);
    uint32_t h = 0;
    if (max <= 0 || initial < 0 || initial > max)
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    else
        h = obj_create(W32_KIND_SEMAPHORE, W32_ARG(3), semaphore_init, W32_ARG(1), W32_ARG(2));
    W32_RETURN(4, h);
}

/* BOOL ReleaseSemaphore(HANDLE, LONG n, LPLONG previous) */
static void imp_ReleaseSemaphore(void)
{
    struct sync_obj *o = obj_get(W32_ARG(0), W32_KIND_SEMAPHORE);
    int32_t n = (int32_t)W32_ARG(1);
    uint32_t prev_va = W32_ARG(2);
    uint32_t err = W32_ERROR_SUCCESS;
    int32_t prev = 0;
    if (o == NULL) {
        err = W32_ERROR_INVALID_HANDLE;
    } else if (n <= 0) {
        err = W32_ERROR_INVALID_PARAMETER;
    } else {
        pthread_mutex_lock(&g_lock);
        prev = o->count;
        if (n > o->max - o->count) {
            err = W32_ERROR_TOO_MANY_POSTS;
        } else {
            o->count += n;
            pthread_cond_broadcast(&g_cond);
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (err != W32_ERROR_SUCCESS)
        w32_set_last_error(err);
    else if (prev_va != 0)
        MEM32(prev_va) = (uint32_t)prev;
    W32_RETURN(3, err == W32_ERROR_SUCCESS ? W32_TRUE : W32_FALSE);
}

/* ---- Thread information block ------------------------------------------- */

/* Guest addresses of the live TIBs, for TlsAlloc. Under g_lock. */
static uint32_t g_tibs[MAX_TIBS];
static uint32_t g_tib_count;
static uint64_t g_tls_used; /* bit i: slot i allocated */
static uint32_t g_fls_callback[TLS_SLOT_COUNT]; /* FlsAlloc callback of slot i, or 0 */

void w32_tib_attach(uint32_t stack_base, uint32_t stack_limit)
{
    uint32_t tib = guest_calloc(1, TIB_SIZE);
    if (tib == 0) {
        fprintf(stderr, "w32_tib_attach: guest heap exhausted\n");
        abort();
    }
    MEM32(tib + TIB_SEH_HEAD) = 0xFFFFFFFFu;
    MEM32(tib + TIB_STACK_BASE) = stack_base;
    MEM32(tib + TIB_STACK_LIMIT) = stack_limit;
    MEM32(tib + TIB_SELF) = tib;
    MEM32(tib + TIB_PID) = (uint32_t)getpid();
    MEM32(tib + TIB_TID) = cur_tid();
    MEM32(tib + TIB_TLS_PTR) = tib + TIB_TLS_SLOTS;
    MEM32(tib + TIB_LAST_ERROR) = 0;
    pthread_mutex_lock(&g_lock);
    if (g_tib_count == MAX_TIBS) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "w32_tib_attach: more than %u threads\n", MAX_TIBS);
        abort();
    }
    g_tibs[g_tib_count++] = tib;
    pthread_mutex_unlock(&g_lock);
    g_fs_base = tib;
}

/* Free a TIB that w32_tib_attach made. */
static void tib_free(uint32_t tib)
{
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_tib_count; i++) {
        if (g_tibs[i] == tib) {
            g_tibs[i] = g_tibs[--g_tib_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    guest_free(tib);
}

/* Host threads attached by w32_host_thread_attach: the key holds the TIB's
 * guest address, and its destructor frees the TIB when the thread exits. */
static pthread_key_t g_host_tib_key;
static pthread_once_t g_host_tib_once = PTHREAD_ONCE_INIT;

static void host_tib_release(void *value)
{
    tib_free((uint32_t)(uintptr_t)value);
}

static void make_host_tib_key(void)
{
    if (pthread_key_create(&g_host_tib_key, host_tib_release) != 0) {
        perror("w32_host_thread_attach: pthread_key_create");
        abort();
    }
}

/* Windows starts every thread at x87 0x027F (53-bit precision) and MXCSR
 * 0x1F80; guest_thread_attach sets the engine's 0x037F. */
static void thread_fpu_init(void)
{
    g_fp_control_word = 0x027F;
    g_mxcsr = 0x1F80;
}

/* guest_thread_attach places stack k at GUEST_CB_STACK_BASE + k *
 * GUEST_CB_STACK_SIZE and starts g_esp at its top, so the slot that holds
 * g_esp - 1 gives the exact bounds even after guest frames were pushed. */
void w32_host_thread_attach(void)
{
    if (g_fs_base != 0)
        return;
    guest_thread_attach();
    thread_fpu_init();
    uint32_t sp = g_esp - 1u;
    if (sp < GUEST_CB_STACK_BASE || sp >= GUEST_CB_STACK_END) {
        fprintf(stderr, "w32_host_thread_attach: g_esp 0x%08X is not a guest_thread_attach stack\n",
                g_esp);
        abort();
    }
    uint32_t limit = GUEST_CB_STACK_BASE +
                     (sp - GUEST_CB_STACK_BASE) / GUEST_CB_STACK_SIZE * GUEST_CB_STACK_SIZE;
    w32_tib_attach(limit + GUEST_CB_STACK_SIZE, limit);
    pthread_once(&g_host_tib_once, make_host_tib_key);
    pthread_setspecific(g_host_tib_key, (void *)(uintptr_t)g_fs_base);
}

/* ---- TLS slots ---------------------------------------------------------- */

/* Allocate a TLS slot that reads 0 on every thread; W32_TLS_OUT_OF_INDEXES
 * when all are taken. */
static uint32_t tls_alloc(void)
{
    uint32_t idx = W32_TLS_OUT_OF_INDEXES;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < TLS_SLOT_COUNT; i++) {
        if ((g_tls_used & (1ull << i)) == 0) {
            g_tls_used |= 1ull << i;
            for (uint32_t t = 0; t < g_tib_count; t++)
                MEM32(g_tibs[t] + TIB_TLS_SLOTS + 4u * i) = 0;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (idx == W32_TLS_OUT_OF_INDEXES)
        w32_set_last_error(W32_ERROR_NO_MORE_ITEMS);
    return idx;
}

/* Free slot i. FLS slots lose their callback too. */
static uint32_t tls_free(uint32_t i)
{
    uint32_t ok = W32_FALSE;
    pthread_mutex_lock(&g_lock);
    if (i < TLS_SLOT_COUNT && (g_tls_used & (1ull << i)) != 0) {
        g_tls_used &= ~(1ull << i);
        g_fls_callback[i] = 0;
        ok = W32_TRUE;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    return ok;
}

/* DWORD TlsAlloc(void) */
static void imp_TlsAlloc(void)
{
    W32_RETURN(0, tls_alloc());
}

/* BOOL TlsFree(DWORD) */
static void imp_TlsFree(void)
{
    W32_RETURN(1, tls_free(W32_ARG(0)));
}

/* Fiber-local storage. The game has no fibers, so an FLS slot is a TLS slot
 * (FlsGetValue/FlsSetValue are TlsGetValue/TlsSetValue in the table). The
 * CRT finds these only through GetProcAddress (_mtinit); without
 * them it takes its Windows XP path. */

/* DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION): the callback runs with the
 * thread's non-NULL value when a CreateThread thread exits
 * (fls_run_callbacks). FlsFree does not call it for live threads. */
static void imp_FlsAlloc(void)
{
    uint32_t cb = W32_ARG(0), idx = tls_alloc();
    if (idx != W32_TLS_OUT_OF_INDEXES) {
        pthread_mutex_lock(&g_lock);
        g_fls_callback[idx] = cb;
        pthread_mutex_unlock(&g_lock);
    }
    W32_RETURN(1, idx);
}

/* BOOL FlsFree(DWORD) */
static void imp_FlsFree(void)
{
    W32_RETURN(1, tls_free(W32_ARG(0)));
}

/* At thread exit, before the TIB goes: each FLS callback with this thread's
 * value, as Windows does. Called outside g_lock (the callback is guest code). */
static void fls_run_callbacks(void)
{
    uint32_t cb[TLS_SLOT_COUNT], val[TLS_SLOT_COUNT];
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < TLS_SLOT_COUNT; i++) {
        cb[i] = (g_tls_used & (1ull << i)) != 0 ? g_fls_callback[i] : 0;
        val[i] = MEM32(g_fs_base + TIB_TLS_SLOTS + 4u * i);
    }
    pthread_mutex_unlock(&g_lock);
    for (uint32_t i = 0; i < TLS_SLOT_COUNT; i++)
        if (cb[i] != 0 && val[i] != 0)
            guest_call_cdecl(cb[i], 1, &val[i]);
}

/* LPVOID TlsGetValue(DWORD): clears the last error on success, as documented. */
static void imp_TlsGetValue(void)
{
    uint32_t i = W32_ARG(0);
    uint32_t v = 0;
    if (i < TLS_SLOT_COUNT) {
        v = MEM32(g_fs_base + TIB_TLS_SLOTS + 4u * i);
        w32_set_last_error(W32_ERROR_SUCCESS);
    } else {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    }
    W32_RETURN(1, v);
}

/* BOOL TlsSetValue(DWORD, LPVOID) */
static void imp_TlsSetValue(void)
{
    uint32_t i = W32_ARG(0);
    if (i < TLS_SLOT_COUNT)
        MEM32(g_fs_base + TIB_TLS_SLOTS + 4u * i) = W32_ARG(1);
    else
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    W32_RETURN(2, i < TLS_SLOT_COUNT ? W32_TRUE : W32_FALSE);
}

/* ---- Threads ------------------------------------------------------------ */

static void *thread_main(void *p)
{
    struct sync_obj *t = p;
    jmp_buf exit_jmp;
    volatile uint32_t code;
    t_tid = t->tid;
    guest_thread_attach();
    thread_fpu_init();
    w32_tib_attach(g_esp, g_esp - GUEST_CB_STACK_SIZE);
    diag_thread_register();

    pthread_mutex_lock(&g_lock);
    while (t->suspend > 0)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);

    t_exit_jmp = &exit_jmp;
    if (setjmp(exit_jmp) == 0)
        code = guest_call_cdecl(t->start, 1, &t->param);
    else
        code = t_exit_code;
    t_exit_jmp = NULL;

    diag_thread_unregister();
    fls_run_callbacks();
    tib_free(g_fs_base);
    g_fs_base = 0;
    pthread_mutex_lock(&g_lock);
    uint32_t me = cur_tid();
    for (struct sync_obj *o = g_objs; o != NULL; o = o->next) {
        if (o->kind == W32_KIND_MUTEX && o->owner == me) {
            o->owner = 0;
            o->recursion = 0;
            o->abandoned = 1;
        }
    }
    t->exit_code = code;
    t->signalled = 1;
    pthread_cond_broadcast(&g_cond);
    obj_unref_locked(t);
    pthread_mutex_unlock(&g_lock);
    /* The guest stack goes back to guest_call.c's free list when this host
     * thread exits (its key destructor). */
    return NULL;
}

/* CreateThread after the allocation: a handle to running (or suspended) t,
 * or 0 with the last error set. */
static uint32_t thread_new(struct sync_obj *t, uint32_t start, uint32_t param,
                           uint32_t flags, uint32_t tid_va)
{
    t->kind = W32_KIND_THREAD;
    t->refs = 1; /* the running thread's reference */
    t->tid = atomic_fetch_add(&g_next_tid, 1);
    t->start = start;
    t->param = param;
    t->suspend = (flags & W32_CREATE_SUSPENDED) ? 1u : 0u;
    pthread_mutex_lock(&g_lock);
    t->next = g_objs;
    g_objs = t;
    pthread_mutex_unlock(&g_lock);

    uint32_t h = handle_for(t);
    pthread_t host;
    pthread_attr_t ha;
    int rc = -1;
    if (h != 0) {
        if (tid_va != 0)
            MEM32(tid_va) = t->tid;
        pthread_attr_init(&ha);
        pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&ha, HOST_STACKSIZE);
        rc = pthread_create(&host, &ha, thread_main, t);
        pthread_attr_destroy(&ha);
        if (rc != 0) {
            w32_handle_close(h);
            h = 0;
        }
    }
    if (rc != 0) {
        pthread_mutex_lock(&g_lock);
        obj_unref_locked(t);
        pthread_mutex_unlock(&g_lock);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    return h;
}

/* HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, start, LPVOID param,
 * DWORD flags, LPDWORD tid). The guest stack is guest_thread_attach's fixed
 * GUEST_CB_STACK_SIZE; the requested size is not used. */
static void imp_CreateThread(void)
{
    uint32_t start = W32_ARG(2), param = W32_ARG(3), flags = W32_ARG(4), tid_va = W32_ARG(5);
    struct sync_obj *t = calloc(1, sizeof *t);
    uint32_t h = 0;
    if (t == NULL) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    } else {
        h = thread_new(t, start, param, flags, tid_va);
    }
    W32_RETURN(6, h);
}

/* void ExitThread(DWORD code): back to thread_main's setjmp. */
static void imp_ExitThread(void)
{
    if (t_exit_jmp == NULL) {
        fprintf(stderr, "UNSUPPORTED KERNEL32.dll!ExitThread: not a CreateThread thread "
                        "(guest return 0x%08X)\n", MEM32(g_esp));
        abort();
    }
    t_exit_code = W32_ARG(0);
    longjmp(*t_exit_jmp, 1);
}

/* DWORD ResumeThread(HANDLE): the previous suspend count. */
static void imp_ResumeThread(void)
{
    struct sync_obj *t = obj_get(W32_ARG(0), W32_KIND_THREAD);
    uint32_t prev = 0xFFFFFFFFu;
    if (t != NULL) {
        pthread_mutex_lock(&g_lock);
        prev = t->suspend;
        if (t->suspend > 0 && --t->suspend == 0)
            pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
    } else {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    }
    W32_RETURN(1, prev);
}

/* BOOL GetExitCodeThread(HANDLE, LPDWORD) */
static void imp_GetExitCodeThread(void)
{
    struct sync_obj *t = obj_get(W32_ARG(0), W32_KIND_THREAD);
    uint32_t out = W32_ARG(1);
    if (t != NULL) {
        pthread_mutex_lock(&g_lock);
        uint32_t code = t->signalled ? t->exit_code : W32_STILL_ACTIVE;
        pthread_mutex_unlock(&g_lock);
        MEM32(out) = code;
    } else {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    }
    W32_RETURN(2, t != NULL ? W32_TRUE : W32_FALSE);
}

/* HANDLE GetCurrentThread(void) */
static void imp_GetCurrentThread(void)
{
    W32_RETURN(0, W32_PSEUDO_CURRENT_THREAD);
}

/* DWORD GetCurrentThreadId(void) */
static void imp_GetCurrentThreadId(void)
{
    W32_RETURN(0, cur_tid());
}

/* ---- Scheduling --------------------------------------------------------- */

/* All logical CPUs, at most 8. */
static uint32_t cpu_mask(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    if (n > 8)
        n = 8;
    return (1u << n) - 1u;
}

/* void Sleep(DWORD ms) */
static void imp_Sleep(void)
{
    uint32_t ms = game_patch_sleep_ms(MEM32(g_esp), W32_ARG(0)); /* MEM32(g_esp): the return address */
    if (ms == 0) {
        sched_yield();
    } else {
        struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
            ;
    }
    W32_RETURN(1, 0);
}

/* BOOL SetThreadPriority(HANDLE, int): priorities are not modelled. */
static void imp_SetThreadPriority(void)
{
    W32_RETURN(2, W32_TRUE);
}

/* DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR): the previous mask. */
static void imp_SetThreadAffinityMask(void)
{
    W32_RETURN(2, cpu_mask());
}

/* BOOL SetProcessAffinityMask(HANDLE, DWORD_PTR) */
static void imp_SetProcessAffinityMask(void)
{
    W32_RETURN(2, W32_TRUE);
}

/* BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR process, PDWORD_PTR system) */
static void imp_GetProcessAffinityMask(void)
{
    MEM32(W32_ARG(1)) = cpu_mask();
    MEM32(W32_ARG(2)) = cpu_mask();
    W32_RETURN(3, W32_TRUE);
}

/* EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE): the previous state. */
static void imp_SetThreadExecutionState(void)
{
    W32_RETURN(1, W32_ES_CONTINUOUS);
}

/* ---- Critical sections -------------------------------------------------- */

/* Guest CRITICAL_SECTION (24 bytes). LockCount follows the pre-Vista rule:
 * -1 when free, +1 for every Enter that is held or waiting. */
#define CS_DEBUG_INFO 0x00u
#define CS_LOCK_COUNT 0x04u
#define CS_RECURSION 0x08u
#define CS_OWNER 0x0Cu
#define CS_SEMAPHORE 0x10u
#define CS_SPIN_COUNT 0x14u

/* Side table: guest address -> recursive host mutex (thunk_threads.c's gmap).
 * Open addressing with linear probing; key 0 marks an empty slot. */
struct gmap {
    uint32_t *keys;
    pthread_mutex_t **vals;
    uint32_t cap; /* power of two, or 0 before first insert */
    uint32_t len;
};

static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gmap g_cs;

static uint32_t gmap_slot(const struct gmap *m, uint32_t key)
{
    return (key * 0x9E3779B1u) & (m->cap - 1);
}

static pthread_mutex_t *gmap_find(const struct gmap *m, uint32_t key)
{
    if (m->cap == 0)
        return NULL;
    for (uint32_t i = gmap_slot(m, key);; i = (i + 1) & (m->cap - 1)) {
        if (m->keys[i] == key)
            return m->vals[i];
        if (m->keys[i] == 0)
            return NULL;
    }
}

static void gmap_insert_raw(struct gmap *m, uint32_t key, pthread_mutex_t *val)
{
    uint32_t i = gmap_slot(m, key);
    while (m->keys[i] != 0 && m->keys[i] != key)
        i = (i + 1) & (m->cap - 1);
    if (m->keys[i] == 0)
        m->len++;
    m->keys[i] = key;
    m->vals[i] = val;
}

/* Insert or replace. Returns 0, or ENOMEM. */
static int gmap_put(struct gmap *m, uint32_t key, pthread_mutex_t *val)
{
    if ((m->len + 1) * 2 > m->cap) {
        struct gmap old = *m;
        uint32_t cap = old.cap ? old.cap * 2 : 64;
        uint32_t *keys = calloc(cap, sizeof *keys);
        pthread_mutex_t **vals = calloc(cap, sizeof *vals);
        if (keys == NULL || vals == NULL) {
            free(keys);
            free(vals);
            return ENOMEM;
        }
        m->keys = keys;
        m->vals = vals;
        m->cap = cap;
        m->len = 0;
        for (uint32_t i = 0; i < old.cap; i++)
            if (old.keys[i] != 0)
                gmap_insert_raw(m, old.keys[i], old.vals[i]);
        free(old.keys);
        free(old.vals);
    }
    gmap_insert_raw(m, key, val);
    return 0;
}

/* Remove with backward-shift deletion, so lookups need no tombstones. */
static void gmap_del(struct gmap *m, uint32_t key)
{
    if (m->cap == 0)
        return;
    uint32_t mask = m->cap - 1;
    uint32_t i = gmap_slot(m, key);
    while (m->keys[i] != key) {
        if (m->keys[i] == 0)
            return;
        i = (i + 1) & mask;
    }
    m->len--;
    for (uint32_t j = (i + 1) & mask; m->keys[j] != 0; j = (j + 1) & mask) {
        uint32_t home = gmap_slot(m, m->keys[j]);
        /* Move j into the hole at i if its home slot is not in (i, j]. */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            i = j;
        }
    }
    m->keys[i] = 0;
    m->vals[i] = NULL;
}

/* The host mutex of the critical section at a. A section that was never
 * initialised is a guest bug or a missed import: stop, do not invent one. */
static pthread_mutex_t *cs_get(const char *fn, uint32_t a)
{
    pthread_mutex_lock(&g_cs_lock);
    pthread_mutex_t *hm = gmap_find(&g_cs, a);
    pthread_mutex_unlock(&g_cs_lock);
    if (hm == NULL) {
        fprintf(stderr, "UNSUPPORTED KERNEL32.dll!%s: uninitialised critical section 0x%08X\n",
                fn, a);
        abort();
    }
    return hm;
}

static _Atomic uint32_t *guest_atomic(uint32_t a)
{
    return (_Atomic uint32_t *)guest_ptr(a);
}

/* Under the host mutex: record the owner. */
static void cs_owned(uint32_t a)
{
    MEM32(a + CS_RECURSION) = MEM32(a + CS_RECURSION) + 1u;
    MEM32(a + CS_OWNER) = cur_tid();
}

/* void InitializeCriticalSection(LPCRITICAL_SECTION). A second initialise of
 * the same address replaces the entry, as pthread_mutex_init does in
 * thunk_threads.c; a busy old mutex is leaked. */
static void cs_init(uint32_t a)
{
    pthread_mutex_t *hm = malloc(sizeof *hm);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (hm == NULL || pthread_mutex_init(hm, &attr) != 0) {
        fprintf(stderr, "InitializeCriticalSection: out of host memory\n");
        abort();
    }
    pthread_mutexattr_destroy(&attr);
    MEM32(a + CS_DEBUG_INFO) = 0;
    MEM32(a + CS_LOCK_COUNT) = 0xFFFFFFFFu;
    MEM32(a + CS_RECURSION) = 0;
    MEM32(a + CS_OWNER) = 0;
    MEM32(a + CS_SEMAPHORE) = 0;
    MEM32(a + CS_SPIN_COUNT) = 0;
    pthread_mutex_lock(&g_cs_lock);
    pthread_mutex_t *old = gmap_find(&g_cs, a);
    int rc = gmap_put(&g_cs, a, hm);
    pthread_mutex_unlock(&g_cs_lock);
    if (rc != 0) {
        fprintf(stderr, "InitializeCriticalSection: out of host memory\n");
        abort();
    }
    if (old != NULL && pthread_mutex_destroy(old) == 0)
        free(old);
}

static void imp_InitializeCriticalSection(void)
{
    cs_init(W32_ARG(0));
    W32_RETURN(1, 0);
}

/* BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION, DWORD).
 * The CRT finds it only through GetProcAddress (_mtinitlocks). The
 * spin count is stored and not used. */
static void imp_InitializeCriticalSectionAndSpinCount(void)
{
    uint32_t a = W32_ARG(0);
    cs_init(a);
    MEM32(a + CS_SPIN_COUNT) = W32_ARG(1);
    W32_RETURN(2, W32_TRUE);
}

/* void DeleteCriticalSection(LPCRITICAL_SECTION) */
static void imp_DeleteCriticalSection(void)
{
    uint32_t a = W32_ARG(0);
    pthread_mutex_t *hm = cs_get("DeleteCriticalSection", a);
    pthread_mutex_lock(&g_cs_lock);
    gmap_del(&g_cs, a);
    pthread_mutex_unlock(&g_cs_lock);
    if (pthread_mutex_destroy(hm) == 0) /* a busy mutex is leaked */
        free(hm);
    W32_RETURN(1, 0);
}

/* void EnterCriticalSection(LPCRITICAL_SECTION) */
static void imp_EnterCriticalSection(void)
{
    uint32_t a = W32_ARG(0);
    pthread_mutex_t *hm = cs_get("EnterCriticalSection", a);
    atomic_fetch_add(guest_atomic(a + CS_LOCK_COUNT), 1u);
    pthread_mutex_lock(hm);
    cs_owned(a);
    W32_RETURN(1, 0);
}

/* BOOL TryEnterCriticalSection(LPCRITICAL_SECTION) */
static void imp_TryEnterCriticalSection(void)
{
    uint32_t a = W32_ARG(0);
    pthread_mutex_t *hm = cs_get("TryEnterCriticalSection", a);
    int got = pthread_mutex_trylock(hm) == 0;
    if (got) {
        atomic_fetch_add(guest_atomic(a + CS_LOCK_COUNT), 1u);
        cs_owned(a);
    }
    W32_RETURN(1, got ? W32_TRUE : W32_FALSE);
}

/* void LeaveCriticalSection(LPCRITICAL_SECTION) */
static void imp_LeaveCriticalSection(void)
{
    uint32_t a = W32_ARG(0);
    pthread_mutex_t *hm = cs_get("LeaveCriticalSection", a);
    uint32_t depth = MEM32(a + CS_RECURSION) - 1u;
    MEM32(a + CS_RECURSION) = depth;
    if (depth == 0)
        MEM32(a + CS_OWNER) = 0;
    atomic_fetch_sub(guest_atomic(a + CS_LOCK_COUNT), 1u);
    pthread_mutex_unlock(hm);
    W32_RETURN(1, 0);
}

/* ---- Interlocked (seq_cst; the guest address must be 4-byte aligned) ---- */

/* LONG InterlockedIncrement(LONG volatile *): the new value */
static void imp_InterlockedIncrement(void)
{
    W32_RETURN(1, atomic_fetch_add(guest_atomic(W32_ARG(0)), 1u) + 1u);
}

/* LONG InterlockedDecrement(LONG volatile *): the new value */
static void imp_InterlockedDecrement(void)
{
    W32_RETURN(1, atomic_fetch_sub(guest_atomic(W32_ARG(0)), 1u) - 1u);
}

/* LONG InterlockedExchange(LONG volatile *, LONG): the old value */
static void imp_InterlockedExchange(void)
{
    W32_RETURN(2, atomic_exchange(guest_atomic(W32_ARG(0)), W32_ARG(1)));
}

/* LONG InterlockedCompareExchange(LONG volatile *dst, LONG exchange,
 * LONG comparand): the old value */
static void imp_InterlockedCompareExchange(void)
{
    uint32_t expected = W32_ARG(2);
    atomic_compare_exchange_strong(guest_atomic(W32_ARG(0)), &expected, W32_ARG(1));
    W32_RETURN(3, expected);
}

/* ---- Clocks ------------------------------------------------------------- */

static uint64_t g_start_ns;

__attribute__((constructor)) static void clock_start(void)
{
    g_start_ns = mono_ns();
}

uint32_t w32_tick_ms(void)
{
    return (uint32_t)((mono_ns() - g_start_ns) / 1000000u) + TICK_OFFSET_MS;
}

/* DWORD GetTickCount(void) */
static void imp_GetTickCount(void)
{
    W32_RETURN(0, w32_tick_ms());
}

/* BOOL QueryPerformanceCounter(LARGE_INTEGER *): 100 ns ticks. */
static void imp_QueryPerformanceCounter(void)
{
    uint32_t out = W32_ARG(0);
    uint64_t ticks = mono_ns() / 100u;
    MEM32(out) = (uint32_t)ticks;
    MEM32(out + 4) = (uint32_t)(ticks >> 32);
    W32_RETURN(1, W32_TRUE);
}

/* BOOL QueryPerformanceFrequency(LARGE_INTEGER *) */
static void imp_QueryPerformanceFrequency(void)
{
    uint32_t out = W32_ARG(0);
    MEM32(out) = (uint32_t)FT_PER_SEC;
    MEM32(out + 4) = 0;
    W32_RETURN(1, W32_TRUE);
}

/* ---- FILETIME and SYSTEMTIME -------------------------------------------- */

/* SYSTEMTIME: eight WORDs. */
#define ST_YEAR 0x0u
#define ST_MONTH 0x2u
#define ST_DOW 0x4u
#define ST_DAY 0x6u
#define ST_HOUR 0x8u
#define ST_MINUTE 0xAu
#define ST_SECOND 0xCu
#define ST_MS 0xEu

static uint64_t ft_read(uint32_t a)
{
    return (uint64_t)MEM32(a) | (uint64_t)MEM32(a + 4) << 32;
}

static void ft_write(uint32_t a, uint64_t v)
{
    MEM32(a) = (uint32_t)v;
    MEM32(a + 4) = (uint32_t)(v >> 32);
}

static uint64_t ft_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return FT_UNIX_EPOCH + (uint64_t)ts.tv_sec * FT_PER_SEC + (uint64_t)ts.tv_nsec / 100u;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static unsigned days_in_month(int64_t y, unsigned m)
{
    static const unsigned n[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return n[m - 1] + (m == 2 && leap);
}

/* ft must be <= 0x7FFFFFFFFFFFFFFF. 1601-01-01 was a Monday. */
static void st_write(uint32_t st, uint64_t ft)
{
    uint64_t secs = ft / FT_PER_SEC;
    int64_t days = (int64_t)(secs / 86400u);
    uint32_t rem = (uint32_t)(secs % 86400u);
    int64_t y;
    unsigned m, d;
    civil_from_days(days - DAYS_1601_TO_1970, &y, &m, &d);
    MEM16(st + ST_YEAR) = (uint16_t)y;
    MEM16(st + ST_MONTH) = (uint16_t)m;
    MEM16(st + ST_DOW) = (uint16_t)((days + 1) % 7);
    MEM16(st + ST_DAY) = (uint16_t)d;
    MEM16(st + ST_HOUR) = (uint16_t)(rem / 3600u);
    MEM16(st + ST_MINUTE) = (uint16_t)(rem / 60u % 60u);
    MEM16(st + ST_SECOND) = (uint16_t)(rem % 60u);
    MEM16(st + ST_MS) = (uint16_t)(ft / 10000u % 1000u);
}

/* void GetSystemTime(LPSYSTEMTIME) */
static void imp_GetSystemTime(void)
{
    st_write(W32_ARG(0), ft_now());
    W32_RETURN(1, 0);
}

/* void GetSystemTimeAsFileTime(LPFILETIME) */
static void imp_GetSystemTimeAsFileTime(void)
{
    ft_write(W32_ARG(0), ft_now());
    W32_RETURN(1, 0);
}

/* BOOL FileTimeToSystemTime(const FILETIME *, LPSYSTEMTIME) */
static void imp_FileTimeToSystemTime(void)
{
    uint64_t ft = ft_read(W32_ARG(0));
    int ok = ft <= 0x7FFFFFFFFFFFFFFFull;
    if (ok)
        st_write(W32_ARG(1), ft);
    else
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    W32_RETURN(2, ok ? W32_TRUE : W32_FALSE);
}

/* BOOL SystemTimeToFileTime(const SYSTEMTIME *, LPFILETIME): wDayOfWeek is
 * ignored. */
static void imp_SystemTimeToFileTime(void)
{
    uint32_t st = W32_ARG(0);
    unsigned y = MEM16(st + ST_YEAR), m = MEM16(st + ST_MONTH), d = MEM16(st + ST_DAY);
    unsigned hh = MEM16(st + ST_HOUR), mm = MEM16(st + ST_MINUTE);
    unsigned ss = MEM16(st + ST_SECOND), ms = MEM16(st + ST_MS);
    int ok = y >= 1601 && y <= 30827 && m >= 1 && m <= 12 && d >= 1 &&
             d <= days_in_month(y, m) && hh <= 23 && mm <= 59 && ss <= 59 && ms <= 999;
    if (ok) {
        uint64_t days = (uint64_t)(days_from_civil(y, m, d) + DAYS_1601_TO_1970);
        uint64_t secs = days * 86400u + hh * 3600u + mm * 60u + ss;
        ft_write(W32_ARG(1), secs * FT_PER_SEC + ms * 10000u);
    } else {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    }
    W32_RETURN(2, ok ? W32_TRUE : W32_FALSE);
}

/* BOOL FileTimeToLocalFileTime(const FILETIME *, LPFILETIME): the current
 * offset, as Windows uses, not the offset at that date. */
static void imp_FileTimeToLocalFileTime(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t off = (int64_t)lt.tm_gmtoff * (int64_t)FT_PER_SEC;
    ft_write(W32_ARG(1), (uint64_t)((int64_t)ft_read(W32_ARG(0)) + off));
    W32_RETURN(2, W32_TRUE);
}

/* LONG CompareFileTime(const FILETIME *, const FILETIME *) */
static void imp_CompareFileTime(void)
{
    uint64_t a = ft_read(W32_ARG(0)), b = ft_read(W32_ARG(1));
    W32_RETURN(2, a < b ? 0xFFFFFFFFu : a > b ? 1u : 0u);
}

/* ---- Time zone ---------------------------------------------------------- */

/* TIME_ZONE_INFORMATION (172 bytes). */
#define TZI_SIZE 0xACu
#define TZI_BIAS 0x00u
#define TZI_STD_NAME 0x04u
#define TZI_STD_DATE 0x44u
#define TZI_STD_BIAS 0x54u
#define TZI_DST_NAME 0x58u
#define TZI_DST_DATE 0x98u
#define TZI_DST_BIAS 0xA8u
#define TZI_NAME_CHARS 32u

static void put_wname(uint32_t a, const char *s)
{
    uint32_t i = 0;
    for (; s != NULL && s[i] != '\0' && i < TZI_NAME_CHARS - 1; i++)
        MEM16(a + 2u * i) = (uint8_t)s[i];
    MEM16(a + 2u * i) = 0;
}

/* The transition at UTC second t (first second of the new rule), written as
 * a Windows yearly rule in the local time of the rule it ends. */
static void put_rule(uint32_t st, time_t t, long old_gmtoff)
{
    time_t wall = t + old_gmtoff;
    struct tm tm;
    gmtime_r(&wall, &tm);
    unsigned m = (unsigned)tm.tm_mon + 1u, d = (unsigned)tm.tm_mday;
    unsigned week = d + 7u > days_in_month(tm.tm_year + 1900, m) ? 5u : (d - 1u) / 7u + 1u;
    MEM16(st + ST_MONTH) = (uint16_t)m;
    MEM16(st + ST_DOW) = (uint16_t)tm.tm_wday;
    MEM16(st + ST_DAY) = (uint16_t)week; /* week of the month, 5 = last */
    MEM16(st + ST_HOUR) = (uint16_t)tm.tm_hour;
    MEM16(st + ST_MINUTE) = (uint16_t)tm.tm_min;
}

/* DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION): the host zone. The
 * transition rules come from this year's transitions, found by stepping
 * localtime one hour, then one minute, at a time. */
static void imp_GetTimeZoneInformation(void)
{
    uint32_t tzi = W32_ARG(0);
    for (uint32_t i = 0; i < TZI_SIZE; i += 4)
        MEM32(tzi + i) = 0;
    tzset();
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int now_dst = lt.tm_isdst > 0;
    long std_off = now_dst ? 0 : lt.tm_gmtoff, dst_off = now_dst ? lt.tm_gmtoff : 0;
    int have_std = !now_dst, have_dst = now_dst;

    struct tm jan = {.tm_year = lt.tm_year, .tm_mon = 0, .tm_mday = 1};
    time_t t0 = timegm(&jan);
    localtime_r(&t0, &lt);
    int prev_dst = lt.tm_isdst > 0;
    long prev_off = lt.tm_gmtoff;
    for (time_t t = t0 + 3600; t <= t0 + 366 * 86400; t += 3600) {
        localtime_r(&t, &lt);
        if ((lt.tm_isdst > 0) == prev_dst)
            continue;
        time_t edge = t - 3600;
        struct tm e;
        do {
            edge += 60;
            localtime_r(&edge, &e);
        } while ((e.tm_isdst > 0) == prev_dst);
        if (e.tm_isdst > 0) {
            put_rule(tzi + TZI_DST_DATE, edge, prev_off);
            dst_off = e.tm_gmtoff;
            have_dst = 1;
        } else {
            put_rule(tzi + TZI_STD_DATE, edge, prev_off);
            std_off = e.tm_gmtoff;
            have_std = 1;
        }
        prev_dst = e.tm_isdst > 0;
        prev_off = e.tm_gmtoff;
    }
    uint32_t has_rules = MEM16(tzi + TZI_DST_DATE + ST_MONTH) != 0;
    if (!have_std)
        std_off = dst_off;
    MEM32(tzi + TZI_BIAS) = (uint32_t)(int32_t)(-std_off / 60);
    MEM32(tzi + TZI_STD_BIAS) = 0;
    MEM32(tzi + TZI_DST_BIAS) = have_dst ? (uint32_t)(int32_t)(-(dst_off - std_off) / 60) : 0u;
    put_wname(tzi + TZI_STD_NAME, tzname[0]);
    put_wname(tzi + TZI_DST_NAME, tzname[1]);
    W32_RETURN(1, !has_rules ? W32_TIME_ZONE_ID_UNKNOWN
                  : now_dst  ? W32_TIME_ZONE_ID_DAYLIGHT
                             : W32_TIME_ZONE_ID_STANDARD);
}

const win32_export_t win32_exports_sync[] = {
    {"KERNEL32.dll", "CompareFileTime", 0, imp_CompareFileTime},
    {"KERNEL32.dll", "CreateEventA", 0, imp_CreateEventA},
    {"KERNEL32.dll", "CreateMutexA", 0, imp_CreateMutexA},
    {"KERNEL32.dll", "CreateSemaphoreA", 0, imp_CreateSemaphoreA},
    {"KERNEL32.dll", "CreateThread", 0, imp_CreateThread},
    {"KERNEL32.dll", "DeleteCriticalSection", 0, imp_DeleteCriticalSection},
    {"KERNEL32.dll", "EnterCriticalSection", 0, imp_EnterCriticalSection},
    {"KERNEL32.dll", "ExitThread", 0, imp_ExitThread},
    {"KERNEL32.dll", "FileTimeToLocalFileTime", 0, imp_FileTimeToLocalFileTime},
    {"KERNEL32.dll", "FileTimeToSystemTime", 0, imp_FileTimeToSystemTime},
    {"KERNEL32.dll", "GetCurrentThread", 0, imp_GetCurrentThread},
    {"KERNEL32.dll", "GetCurrentThreadId", 0, imp_GetCurrentThreadId},
    {"KERNEL32.dll", "GetExitCodeThread", 0, imp_GetExitCodeThread},
    {"KERNEL32.dll", "GetProcessAffinityMask", 0, imp_GetProcessAffinityMask},
    {"KERNEL32.dll", "GetSystemTime", 0, imp_GetSystemTime},
    {"KERNEL32.dll", "GetSystemTimeAsFileTime", 0, imp_GetSystemTimeAsFileTime},
    {"KERNEL32.dll", "GetTickCount", 0, imp_GetTickCount},
    {"KERNEL32.dll", "GetTimeZoneInformation", 0, imp_GetTimeZoneInformation},
    {"KERNEL32.dll", "InitializeCriticalSection", 0, imp_InitializeCriticalSection},
    {"KERNEL32.dll", "InterlockedCompareExchange", 0, imp_InterlockedCompareExchange},
    {"KERNEL32.dll", "InterlockedDecrement", 0, imp_InterlockedDecrement},
    {"KERNEL32.dll", "InterlockedExchange", 0, imp_InterlockedExchange},
    {"KERNEL32.dll", "InterlockedIncrement", 0, imp_InterlockedIncrement},
    {"KERNEL32.dll", "LeaveCriticalSection", 0, imp_LeaveCriticalSection},
    {"KERNEL32.dll", "QueryPerformanceCounter", 0, imp_QueryPerformanceCounter},
    {"KERNEL32.dll", "QueryPerformanceFrequency", 0, imp_QueryPerformanceFrequency},
    {"KERNEL32.dll", "ReleaseMutex", 0, imp_ReleaseMutex},
    {"KERNEL32.dll", "ReleaseSemaphore", 0, imp_ReleaseSemaphore},
    {"KERNEL32.dll", "ResetEvent", 0, imp_ResetEvent},
    {"KERNEL32.dll", "ResumeThread", 0, imp_ResumeThread},
    {"KERNEL32.dll", "SetEvent", 0, imp_SetEvent},
    {"KERNEL32.dll", "SetProcessAffinityMask", 0, imp_SetProcessAffinityMask},
    {"KERNEL32.dll", "SetThreadAffinityMask", 0, imp_SetThreadAffinityMask},
    {"KERNEL32.dll", "SetThreadExecutionState", 0, imp_SetThreadExecutionState},
    {"KERNEL32.dll", "SetThreadPriority", 0, imp_SetThreadPriority},
    {"KERNEL32.dll", "Sleep", 0, imp_Sleep},
    {"KERNEL32.dll", "SystemTimeToFileTime", 0, imp_SystemTimeToFileTime},
    {"KERNEL32.dll", "FlsAlloc", 0, imp_FlsAlloc},
    {"KERNEL32.dll", "FlsFree", 0, imp_FlsFree},
    {"KERNEL32.dll", "FlsGetValue", 0, imp_TlsGetValue},
    {"KERNEL32.dll", "FlsSetValue", 0, imp_TlsSetValue},
    {"KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", 0,
     imp_InitializeCriticalSectionAndSpinCount},
    {"KERNEL32.dll", "TlsAlloc", 0, imp_TlsAlloc},
    {"KERNEL32.dll", "TlsFree", 0, imp_TlsFree},
    {"KERNEL32.dll", "TlsGetValue", 0, imp_TlsGetValue},
    {"KERNEL32.dll", "TlsSetValue", 0, imp_TlsSetValue},
    {"KERNEL32.dll", "TryEnterCriticalSection", 0, imp_TryEnterCriticalSection},
    {"KERNEL32.dll", "WaitForMultipleObjects", 0, imp_WaitForMultipleObjects},
    {"KERNEL32.dll", "WaitForMultipleObjectsEx", 0, imp_WaitForMultipleObjectsEx},
    {"KERNEL32.dll", "WaitForSingleObject", 0, imp_WaitForSingleObject},
    {0},
};
