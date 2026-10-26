/* The frame-time overlay without a device: the shared engine logic
 * (hud_core.c: stats over the last 240 frames, the text lines) and the D3D9
 * backend's quads (d3d9_hud.c: the layout). */
#include <stdio.h>
#include <string.h>

#include "d3d9_hud.h"
#include "recomp_types.h"

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

static struct d3d9_hud_vertex g_v[D3D9_HUD_MAX_QUADS * 6];

int main(void)
{
    struct hud_settings st = {.render_w = 3456, .render_h = 2234, .win_w = 1728, .win_h = 1117, .vsync = 1,
                              .heap_guard = 0, .count_a = "DRAWS", .count_b = "RT SETS"};
    /* Before any frame: zeros, no bars. */
    hud_refresh_text(&st);
    CHECK(strcmp(hud_line(2), "OVER 33 MS: 0 OF 0") == 0);

    /* 60 slow frames that leave the ring, then 10 over 33.3 ms, one at the
     * 33.3 edge (not over) and 229 at 16 ms, newest last; 800 draws and 9
     * render-target sets in each frame since the last refresh. */
    for (int i = 0; i < 60; i++)
        hud_record_frame(80.0, 800, 9);
    hud_refresh_text(&st); /* restart the per-frame means */
    for (int i = 0; i < 10; i++)
        hud_record_frame(40.0, 800, 9);
    hud_record_frame(33.3, 800, 9);
    for (int i = 0; i < 229; i++) {
        hud_record_frame(16.0, 800, 9);
        hud_add_swap(4.0);
    }
    CHECK(hud_text_due());
    hud_refresh_text(&st);
    CHECK(!hud_text_due());
    CHECK(strcmp(hud_line(0), "FPS 62.5   1% LOW 25.0") == 0); /* newest second all 16 ms; P99 40 ms */
    CHECK(strcmp(hud_line(1), "AVG 16.0 MS  P99 40.0  MAX 40.0") == 0);
    CHECK(strcmp(hud_line(2), "OVER 33 MS: 10 OF 240") == 0); /* 80 ms frames left; 33.3 not over */
    CHECK(strncmp(hud_line(3), "GAME 13.", 8) == 0 && strstr(hud_line(3), "SWAP 3.8 MS") != NULL);
    CHECK(strcmp(hud_line(4), "DRAWS 800  RT SETS 9 PER FRAME") == 0);
    CHECK(strcmp(hud_line(5), "RENDER 3456x2234  WINDOW 1728x1117  2.00x") == 0);
    CHECK(strncmp(hud_line(6), "VSYNC ON  TSO ", 14) == 0 && strstr(hud_line(6), "HEAP GUARD OFF") != NULL);

    /* Layout at 2 px dots: the backdrop, then the glyphs of each line in
     * order, then one bar per kept frame and the two reference lines. */
    float px = 2.0f, pad = 4.0f * px, line_h = 10.0f * px;
    unsigned glyphs = 0;
    for (int i = 0; i < HUD_LINES; i++)
        for (const char *s = hud_line(i); *s; s++)
            glyphs += hud_glyph(*s) != NULL;
    unsigned n = d3d9_hud_build(g_v, px);
    CHECK(n == 1 + glyphs + HUD_FRAMES + 2 && n <= D3D9_HUD_MAX_QUADS);
    CHECK(g_v[0].x == pad * 0.5f - 0.5f && g_v[0].color == 0x99000000u && g_v[0].rhw == 1.0f);
    /* Line 0 is "FPS ...": 'F' at the pad, 'P' one 6-dot cell right; 'F' is atlas cell 16. */
    CHECK(g_v[6].x == pad - 0.5f && g_v[6].y == pad - 0.5f && g_v[6].u == 96.0f / 256.0f);
    CHECK(g_v[12].x == pad + 6.0f * px - 0.5f);
    CHECK(g_v[7].x - g_v[6].x == 6.0f * px && g_v[8].y - g_v[6].y == 8.0f * px);
    /* The first glyph of line 1 ("AVG") is one line down. */
    unsigned first1 = 1;
    for (const char *s = hud_line(0); *s; s++)
        first1 += hud_glyph(*s) != NULL;
    CHECK(g_v[6 * first1].y == pad + line_h - 0.5f && g_v[6 * first1].x == pad - 0.5f);
    /* The newest bar (16 ms, green) is the last bar, at the graph's right end. */
    const struct d3d9_hud_vertex *bar = &g_v[6 * (n - 3)];
    CHECK(bar->color == 0xE64DE64Du && bar[1].x == pad + 240.0f * px * 0.5f - 0.5f);
    CHECK(g_v[6 * (n - 3 - 229)].color == 0xE6F2D933u); /* the 33.3 ms frame: yellow */
    CHECK(g_v[6 * (n - 3 - 230)].color == 0xE6F24D40u); /* a 40 ms frame: red */

    printf("%s test_d3d9_hud\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
