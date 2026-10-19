/* Run-time totals for the boot check (tools/checks/boot_check.py).
 * The thunks bump the counters with relaxed atomic adds. stats_on_swap() runs
 * once per presented frame; it prints a "stats:" line under RECOMP_STATS=<s>
 * and checks glGetError under RECOMP_GL_CHECK=1. With neither set it costs one
 * branch per swap. */
#ifndef STATS_H
#define STATS_H

#include <stddef.h>

extern unsigned long g_stat_draws, g_stat_tex_uploads, g_stat_audio_renders, g_stat_cg_programs,
    g_stat_cg_ns;

static inline void stat_count_draw(void)
{
    __atomic_fetch_add(&g_stat_draws, 1ul, __ATOMIC_RELAXED);
}

static inline void stat_count_tex_upload(void)
{
    __atomic_fetch_add(&g_stat_tex_uploads, 1ul, __ATOMIC_RELAXED);
}

static inline void stat_count_audio_render(void)
{
    __atomic_fetch_add(&g_stat_audio_renders, 1ul, __ATOMIC_RELAXED);
}

static inline void stat_add_cg_program(unsigned long ns)
{
    __atomic_fetch_add(&g_stat_cg_programs, 1ul, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stat_cg_ns, ns, __ATOMIC_RELAXED);
}

void stats_on_swap(void);

/* Writes the "stats:" line (with the newline) to out; returns the snprintf result. */
int stats_format(char *out, size_t n, double t_s, unsigned long swaps, double draws_per_swap,
                 unsigned long tex, unsigned long audio, unsigned long cg_programs, double cg_ms,
                 unsigned long gl_errors);

#endif
