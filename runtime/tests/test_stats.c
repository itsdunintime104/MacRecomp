/* Unit test for runtime/src/stats.c: line format and the counter adders. */
#include <stdio.h>
#include <string.h>

#include "stats.h"

int main(void)
{
    char buf[256];
    stats_format(buf, sizeof buf, 10.4, 600, 123.45, 7, 8, 9, 12.6, 3);
    const char *want = "stats: t=10 swaps=600 draws_per_swap=123.5 tex_uploads=7 audio_renders=8 "
                       "cg_programs=9 cg_ms=13 gl_errors=3\n";
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "format mismatch:\n got  %s want %s", buf, want);
        return 1;
    }
    stat_count_draw();
    stat_count_draw();
    stat_count_tex_upload();
    stat_count_audio_render();
    stat_add_cg_program(1000);
    stat_add_cg_program(500);
    if (g_stat_draws != 2 || g_stat_tex_uploads != 1 || g_stat_audio_renders != 1 ||
        g_stat_cg_programs != 2 || g_stat_cg_ns != 1500) {
        fprintf(stderr, "counter mismatch\n");
        return 1;
    }
    stats_on_swap(); /* env unset: returns after one branch */
    printf("stats ok\n");
    return 0;
}
