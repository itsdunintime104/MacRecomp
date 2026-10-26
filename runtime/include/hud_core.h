/* Frame-time overlay logic (runtime/src/hud_core.c), shared by the GL
 * frontend (frontend_hud.c) and other draw backends: the on/off state, the
 * frame-time ring, the text lines, the layout and the font. No drawing, no
 * SDL. A backend records each frame, refreshes the text when it is due and
 * draws the backdrop, the lines and the graph from hud_layout. */
#ifndef HUD_CORE_H
#define HUD_CORE_H

#define HUD_FRAMES 240          /* frame times kept for the graph and the spread stats */
#define HUD_GRAPH_MS 50.0       /* graph height in milliseconds */
#define HUD_TEXT_PERIOD 0.5     /* seconds between text refreshes */
#define HUD_SLOW_MS (1000.0 / 30.0)
#define HUD_LINES 7
#define HUD_LINE_LEN 48

/* What the backend reports at a text refresh. count_a and count_b name the
 * two per-frame counts (GL: "DRAWS" and "MAPS"). */
struct hud_settings {
    int render_w, render_h, win_w, win_h;
    int vsync, heap_guard; /* heap_guard: guest_heap_guard_mode() */
    const char *count_a, *count_b;
};

/* Geometry in pixels for dots of px pixels, from the current lines. */
struct hud_layout {
    float px, pad, x0, y0, line_h;
    float box_x, box_y, box_w, box_h; /* backdrop */
    float gy, gw, gh, bar;            /* graph top, size, bar width */
};

/* On at the first call with RECOMP_HUD=1; hud_toggle flips it (any thread). */
int hud_on(void);
void hud_toggle(void);

/* One frame of dt_ms with count_a and count_b calls in it. */
void hud_record_frame(double dt_ms, unsigned count_a, unsigned count_b);
/* Time spent in the swap (Present) of the last frame. */
void hud_add_swap(double ms);
/* Nonzero when the text is older than HUD_TEXT_PERIOD or never made. */
int hud_text_due(void);
void hud_refresh_text(const struct hud_settings *s);
/* Nonzero once after each refresh (the GL backend recompiles its list). */
int hud_text_dirty_take(void);
const char *hud_line(int i);

void hud_layout(float px, struct hud_layout *l);
/* Kept frames, 0 the oldest; bar i of the graph and its colour. */
unsigned hud_frame_count(void);
void hud_bar(const struct hud_layout *l, unsigned i, float xywh[4], float rgba[4]);
/* The y of the 16.7 ms (k = 1) and 33.3 ms (k = 2) reference lines. */
float hud_ref_line_y(const struct hud_layout *l, int k);
/* The 5x7 glyph of c, one byte per row, bit 4 the leftmost column; NULL
 * for a character outside the font (drawn as a space). */
const unsigned char *hud_glyph(char c);

#endif
