/* Run-time totals and the per-swap hook; see stats.h.
 *   RECOMP_STATS=<seconds>  print a "stats:" line every <seconds> (to stderr)
 *   RECOMP_GL_CHECK=1       drain glGetError after every swap */
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "stats.h"

unsigned long g_stat_draws, g_stat_tex_uploads, g_stat_audio_renders, g_stat_cg_programs,
    g_stat_cg_ns;

#define MAX_GL_CODES 16
#define MAX_GL_DRAIN 16

int stats_format(char *out, size_t n, double t_s, unsigned long swaps, double draws_per_swap,
                 unsigned long tex, unsigned long audio, unsigned long cg_programs, double cg_ms,
                 unsigned long gl_errors)
{
    return snprintf(out, n,
                    "stats: t=%.0f swaps=%lu draws_per_swap=%.1f tex_uploads=%lu audio_renders=%lu "
                    "cg_programs=%lu cg_ms=%.0f gl_errors=%lu\n",
                    t_s, swaps, draws_per_swap, tex, audio, cg_programs, cg_ms, gl_errors);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void gl_check(unsigned long swaps, unsigned long *total, GLenum *seen, int *nseen)
{
    for (int i = 0; i < MAX_GL_DRAIN; i++) {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            return;
        (*total)++;
        int known = 0;
        for (int k = 0; k < *nseen; k++)
            known |= seen[k] == e;
        if (!known && *nseen < MAX_GL_CODES) {
            seen[(*nseen)++] = e;
            fprintf(stderr, "gl: error 0x%04X first at swap %lu\n", (unsigned)e, swaps);
        }
    }
}

void stats_on_swap(void)
{
    static int inited, gl_on;
    static double interval_s;
    if (!inited) {
        const char *s = getenv("RECOMP_STATS");
        int v = s ? atoi(s) : 0;
        interval_s = v > 0 ? (double)v : 0.0;
        const char *g = getenv("RECOMP_GL_CHECK");
        gl_on = g && atoi(g) == 1;
        inited = 1;
    }
    if (!gl_on && interval_s == 0.0)
        return;

    static unsigned long swaps, gl_errors, last_swaps, last_draws;
    static GLenum seen[MAX_GL_CODES];
    static int nseen;
    static double start_s, last_s;
    swaps++;
    if (gl_on)
        gl_check(swaps, &gl_errors, seen, &nseen);
    if (interval_s == 0.0)
        return;
    double now = now_s();
    if (swaps == 1)
        start_s = last_s = now;
    if (now - last_s < interval_s)
        return;
    unsigned long draws = __atomic_load_n(&g_stat_draws, __ATOMIC_RELAXED);
    unsigned long dswaps = swaps - last_swaps;
    double dps = dswaps ? (double)(draws - last_draws) / (double)dswaps : 0.0;
    char line[256];
    int n = stats_format(line, sizeof line, now - start_s, swaps, dps,
                         __atomic_load_n(&g_stat_tex_uploads, __ATOMIC_RELAXED),
                         __atomic_load_n(&g_stat_audio_renders, __ATOMIC_RELAXED),
                         __atomic_load_n(&g_stat_cg_programs, __ATOMIC_RELAXED),
                         (double)__atomic_load_n(&g_stat_cg_ns, __ATOMIC_RELAXED) * 1e-6,
                         gl_errors);
    if (n > 0)
        fputs(line, stderr);
    last_s = now;
    last_swaps = swaps;
    last_draws = draws;
}
