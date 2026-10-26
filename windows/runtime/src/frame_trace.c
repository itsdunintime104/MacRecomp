/* Frame timing and slow host call log. Contract: runtime/include/frame_trace.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_funcs.h"
#include "frame_trace.h"

#define FT_SLOTS 2048
#define FT_SLOW_NS 2000000ull

static int g_on = -1;
static FILE *g_out;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static recomp_func_t g_fn[FT_SLOTS];
static char g_name[FT_SLOTS][64];
static unsigned g_count;
static int g_present_k[2] = {-1, -1};
static uint64_t g_t0;

/* Frame state, written only by the thread that calls Present. */
static uint64_t g_frames, g_last_ns, g_last_cpu_ns, g_present_ns, g_flush_ns;

static _Thread_local int t_depth;
static _Thread_local uint64_t t_host_ns, t_max_ns;
static _Thread_local int t_max_k = -1;

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

int ft_enabled(void)
{
    if (g_on < 0) {
        const char *e = getenv("HPRECOMP_FRAME_TRACE");
        g_on = e != NULL && e[0] != '\0' && e[0] != '0';
        if (g_on) {
            const char *path = getenv("HPRECOMP_FRAME_TRACE_OUT");
            g_out = path != NULL ? fopen(path, "w") : NULL;
            if (g_out == NULL)
                g_out = stderr;
            g_t0 = now_ns();
        }
    }
    return g_on;
}

static void frame_line(void)
{
    uint64_t t = now_ns(), cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    if (g_frames > 0) {
        pthread_mutex_lock(&g_lock);
        fprintf(g_out, "F %llu %.3f %.3f %.3f %.3f %.3f %s %.3f\n", (unsigned long long)g_frames,
                (t - g_t0) / 1e6, (t - g_last_ns) / 1e6, (cpu - g_last_cpu_ns) / 1e6,
                t_host_ns / 1e6, t_max_ns / 1e6, t_max_k >= 0 ? g_name[t_max_k] : "-",
                g_present_ns / 1e6);
        if (t - g_flush_ns >= 1000000000ull) {
            fflush(g_out);
            g_flush_ns = t;
        }
        pthread_mutex_unlock(&g_lock);
    }
    g_frames++;
    g_last_ns = t;
    g_last_cpu_ns = cpu;
    t_host_ns = t_max_ns = 0;
    t_max_k = -1;
}

static void slow_line(unsigned k, uint64_t t0, uint64_t d)
{
    uint64_t tid = 0;
    char tname[32] = "";
    pthread_threadid_np(NULL, &tid);
    pthread_getname_np(pthread_self(), tname, sizeof tname);
    for (char *c = tname; *c != '\0'; c++)
        if (*c == ' ')
            *c = '_';
    pthread_mutex_lock(&g_lock);
    fprintf(g_out, "S %.3f %llu %s %s %.3f\n", (t0 - g_t0) / 1e6, (unsigned long long)tid,
            tname[0] != '\0' ? tname : "-", g_name[k], d / 1e6);
    pthread_mutex_unlock(&g_lock);
}

static void ft_call(unsigned k)
{
    int present = (int)k == g_present_k[0] || (int)k == g_present_k[1];
    if (present && t_depth == 0)
        frame_line();
    uint64_t t0 = now_ns();
    t_depth++;
    g_fn[k]();
    t_depth--;
    uint64_t d = now_ns() - t0;
    if (t_depth == 0) {
        t_host_ns += d;
        if (d > t_max_ns) {
            t_max_ns = d;
            t_max_k = (int)k;
        }
        if (present)
            g_present_ns = d;
    }
    if (d >= FT_SLOW_NS)
        slow_line(k, t0, d);
}

/* k is four octal digits, so 0##k is the slot index 0..2047. */
#define T1(k) static void ft_##k(void) { ft_call(0##k); }
#define T8(k) T1(k##0) T1(k##1) T1(k##2) T1(k##3) T1(k##4) T1(k##5) T1(k##6) T1(k##7)
#define T64(k) T8(k##0) T8(k##1) T8(k##2) T8(k##3) T8(k##4) T8(k##5) T8(k##6) T8(k##7)
#define T512(k) T64(k##0) T64(k##1) T64(k##2) T64(k##3) T64(k##4) T64(k##5) T64(k##6) T64(k##7)
T512(0) T512(1) T512(2) T512(3)
#define R1(k) ft_##k,
#define R8(k) R1(k##0) R1(k##1) R1(k##2) R1(k##3) R1(k##4) R1(k##5) R1(k##6) R1(k##7)
#define R64(k) R8(k##0) R8(k##1) R8(k##2) R8(k##3) R8(k##4) R8(k##5) R8(k##6) R8(k##7)
#define R512(k) R64(k##0) R64(k##1) R64(k##2) R64(k##3) R64(k##4) R64(k##5) R64(k##6) R64(k##7)
static const recomp_func_t g_trampolines[FT_SLOTS] = {R512(0) R512(1) R512(2) R512(3)};

static int wrap_slot(recomp_func_t fn, const char *name)
{
    pthread_mutex_lock(&g_lock);
    unsigned k = 0;
    while (k < g_count && g_fn[k] != fn)
        k++;
    if (k == g_count) {
        if (k == FT_SLOTS) {
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "frame_trace: pool full, %s not timed\n", name);
            return -1;
        }
        g_fn[k] = fn;
        snprintf(g_name[k], sizeof g_name[k], "%s", name);
        g_count++;
    }
    pthread_mutex_unlock(&g_lock);
    return (int)k;
}

uint32_t ft_wrap(recomp_func_t fn, const char *name)
{
    int k = fn != NULL && ft_enabled() ? wrap_slot(fn, name) : -1;
    return host_func_va(k >= 0 ? g_trampolines[k] : fn);
}

uint32_t ft_wrap_method(recomp_func_t fn, const char *iface, unsigned index)
{
    if (fn == NULL || !ft_enabled())
        return host_func_va(fn);
    char name[64];
    snprintf(name, sizeof name, "%s#%u", iface, index);
    int k = wrap_slot(fn, name);
    if (k < 0)
        return host_func_va(fn);
    if (index == 17 && strcmp(iface, "IDirect3DDevice9") == 0)
        g_present_k[0] = k;
    else if (index == 3 && strcmp(iface, "IDirect3DSwapChain9") == 0)
        g_present_k[1] = k;
    return host_func_va(g_trampolines[k]);
}
