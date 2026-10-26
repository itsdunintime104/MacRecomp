/* Frame-time overlay logic shared by the draw backends (hud_core.h). Moved
 * from frontend_hud.c unchanged, except that the backend passes its sizes,
 * vsync, heap guard mode and count names in, and the on/off state is atomic
 * because a toggle can come from another thread than the draw. */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud_core.h"

/* The build defines both (build/Makefile); a unit test may not. */
#ifndef RECOMP_TSO
#define RECOMP_TSO 0
#endif
#ifndef RECOMP_OPT
#define RECOMP_OPT 0
#endif

static atomic_int s_on = -1;
static double s_ms[HUD_FRAMES];
static unsigned s_head, s_count;
static double s_text_age;
static char s_lines[HUD_LINES][HUD_LINE_LEN];
static int s_text_ready, s_text_dirty;

/* Sums since the last text refresh. */
static double s_acc_swap_ms, s_acc_frame_ms;
static unsigned s_acc_frames, s_acc_a, s_acc_b;

/* 5x7 glyphs, one byte per row, bit 4 is the leftmost column. */
static const char GLYPH_CHARS[] = "0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ%:-x";
static const unsigned char GLYPHS[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},                                             /* . */
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* A B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* C D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* E F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* G H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* I J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* K L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* M N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* O P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* Q R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* S T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* U V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* W X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Y Z */
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, /* % : */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, /* - x */
};

const unsigned char *hud_glyph(char c)
{
    const char *at = c ? strchr(GLYPH_CHARS, c) : NULL;
    return at ? GLYPHS[at - GLYPH_CHARS] : NULL;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void reset_means(void)
{
    s_acc_swap_ms = s_acc_frame_ms = 0.0;
    s_acc_frames = s_acc_a = s_acc_b = 0;
    s_text_age = 0.0;
}

int hud_on(void)
{
    int on = atomic_load(&s_on);
    if (on < 0) {
        const char *e = getenv("RECOMP_HUD");
        on = e && e[0] == '1';
        atomic_store(&s_on, on);
        if (on)
            fprintf(stderr, "frontend: HUD on (Cmd+F toggles)\n");
    }
    return on;
}

void hud_toggle(void)
{
    int on = atomic_load(&s_on) > 0 ? 0 : 1;
    atomic_store(&s_on, on);
    fprintf(stderr, "frontend: HUD %s\n", on ? "on" : "off");
    /* Start the per-frame means fresh, not from when the overlay was hidden. */
    reset_means();
}

void hud_record_frame(double dt_ms, unsigned count_a, unsigned count_b)
{
    s_ms[s_head] = dt_ms;
    s_head = (s_head + 1) % HUD_FRAMES;
    if (s_count < HUD_FRAMES)
        s_count++;
    s_acc_frame_ms += dt_ms;
    s_acc_frames++;
    s_acc_a += count_a;
    s_acc_b += count_b;
    s_text_age += dt_ms / 1000.0;
}

void hud_add_swap(double ms)
{
    s_acc_swap_ms += ms;
}

int hud_text_due(void)
{
    return !s_text_ready || s_text_age >= HUD_TEXT_PERIOD;
}

int hud_text_dirty_take(void)
{
    int d = s_text_dirty;
    s_text_dirty = 0;
    return d;
}

const char *hud_line(int i)
{
    return s_lines[i];
}

/* Rewrite the text lines. FPS and AVG cover the newest frames that fit in one
 * second; P99, 1% LOW, MAX and the slow count cover all HUD_FRAMES frames;
 * GAME, SWAP and the two counts are per-frame means since the last refresh. */
void hud_refresh_text(const struct hud_settings *st)
{
    double sum = 0.0;
    unsigned n = 0;
    for (; n < s_count && sum < 1000.0; n++)
        sum += s_ms[(s_head + HUD_FRAMES - 1 - n) % HUD_FRAMES];
    double avg = n ? sum / n : 0.0;

    double sorted[HUD_FRAMES];
    memcpy(sorted, s_ms, s_count * sizeof sorted[0]);
    qsort(sorted, s_count, sizeof sorted[0], cmp_double);
    double p99 = 0.0, worst = 0.0;
    unsigned slow = 0;
    if (s_count) {
        p99 = sorted[(unsigned)ceil(0.99 * s_count) - 1];
        worst = sorted[s_count - 1];
        for (unsigned i = 0; i < s_count; i++)
            slow += sorted[i] > HUD_SLOW_MS;
    }

    double frames = s_acc_frames ? (double)s_acc_frames : 1.0;
    double swap = s_acc_swap_ms / frames;
    double game = s_acc_frame_ms / frames - swap;
    int w = st->render_w, win_w = st->win_w;

    snprintf(s_lines[0], HUD_LINE_LEN, "FPS %.1f   1%% LOW %.1f",
             avg > 0.0 ? 1000.0 / avg : 0.0, p99 > 0.0 ? 1000.0 / p99 : 0.0);
    snprintf(s_lines[1], HUD_LINE_LEN, "AVG %.1f MS  P99 %.1f  MAX %.1f", avg, p99, worst);
    snprintf(s_lines[2], HUD_LINE_LEN, "OVER 33 MS: %u OF %u", slow, s_count);
    snprintf(s_lines[3], HUD_LINE_LEN, "GAME %.1f MS  SWAP %.1f MS", game > 0.0 ? game : 0.0, swap);
    snprintf(s_lines[4], HUD_LINE_LEN, "%s %.0f  %s %.0f PER FRAME", st->count_a,
             (double)s_acc_a / frames, st->count_b, (double)s_acc_b / frames);
    snprintf(s_lines[5], HUD_LINE_LEN, "RENDER %dx%d  WINDOW %dx%d  %.2fx", w, st->render_h, win_w, st->win_h,
             win_w > 0 ? (double)w / win_w : 0.0);
    /* RECOMP_TSO and RECOMP_OPT come from build/Makefile. */
    snprintf(s_lines[6], HUD_LINE_LEN, "VSYNC %s  TSO %d  -O%d  HEAP GUARD %s",
             st->vsync ? "ON" : "OFF", RECOMP_TSO, RECOMP_OPT,
             st->heap_guard == 0 ? "OFF" : st->heap_guard == 1 ? "ON" : "LOG");

    reset_means();
    s_text_ready = 1;
    s_text_dirty = 1;
}

void hud_layout(float px, struct hud_layout *l)
{
    l->px = px;
    l->pad = 4.0f * px;
    l->x0 = l->y0 = l->pad;
    l->line_h = 10.0f * px;
    l->gw = (float)HUD_FRAMES * px * 0.5f;
    l->gh = 24.0f * px;
    float text_w = 0.0f;
    for (int i = 0; i < HUD_LINES; i++) {
        float lw = (float)strlen(s_lines[i]) * 6.0f * px;
        if (lw > text_w)
            text_w = lw;
    }
    l->box_w = (text_w > l->gw ? text_w : l->gw) + 2.0f * l->pad;
    l->gy = l->y0 + (float)HUD_LINES * l->line_h + l->pad;
    l->box_h = l->gy - l->y0 + l->gh + 2.0f * l->pad;
    l->box_x = l->x0 - l->pad * 0.5f;
    l->box_y = l->y0 - l->pad * 0.5f;
    l->bar = l->gw / HUD_FRAMES;
}

unsigned hud_frame_count(void)
{
    return s_count;
}

/* Graph: one bar per frame, oldest on the left; green under 16.7 ms,
 * yellow under 33.3 ms, red above. */
void hud_bar(const struct hud_layout *l, unsigned i, float xywh[4], float rgba[4])
{
    static const float k_green[4] = {0.3f, 0.9f, 0.3f, 0.9f}, k_yellow[4] = {0.95f, 0.85f, 0.2f, 0.9f},
                       k_red[4] = {0.95f, 0.3f, 0.25f, 0.9f};
    double ms = s_ms[(s_head + HUD_FRAMES - s_count + i) % HUD_FRAMES];
    float bh = (float)(ms > HUD_GRAPH_MS ? 1.0 : ms / HUD_GRAPH_MS) * l->gh;
    const float *c = ms <= 1000.0 / 60.0 + 0.5 ? k_green : ms <= 1000.0 / 30.0 + 0.5 ? k_yellow : k_red;
    xywh[0] = l->x0 + (float)(HUD_FRAMES - s_count + i) * l->bar;
    xywh[1] = l->gy + l->gh - bh;
    xywh[2] = l->bar;
    xywh[3] = bh;
    memcpy(rgba, c, sizeof k_green);
}

float hud_ref_line_y(const struct hud_layout *l, int k)
{
    return l->gy + l->gh - (float)((1000.0 / (60.0 / k)) / HUD_GRAPH_MS) * l->gh;
}
