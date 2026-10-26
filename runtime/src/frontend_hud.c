/* Frame-time overlay drawn over the game's back buffer just before each swap.
 * The Metal HUD does not attach to this OpenGL app, so this replaces it.
 * RECOMP_HUD=1 shows it from the start; Cmd+F toggles it.
 *
 * It draws with legacy GL 2.1 immediate mode and saves and restores every
 * state it touches (attributes, matrices, GLSL program, framebuffer), so the
 * game's next frame sees the state it left. The text changes twice a second;
 * it is compiled into a display list then, so a normal frame replays it.
 * The state, text and layout are in hud_core.c; this file draws them. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frontend_hud.h"
#include "guest_heap.h"
#include "hud_core.h"
#include "hud_counters.h"
#include "slowframe.h"

static Uint64 s_last, s_swap_start;
static GLuint s_text_list;

/* RECOMP_FRAMELOG=<file>: every frame time as CSV, independent of the HUD. */
static int s_log_state = -1;  /* -1 not read yet, 0 off, 1 writing */
static FILE *s_log;
static char s_log_buf[64 * 1024];
static Uint64 s_log_t0;
static double s_log_t0_uptime_s;  /* CLOCK_UPTIME_RAW at s_log_t0, for matching other logs */
static double s_log_flushed;

static void quad(float x, float y, float w, float h)
{
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
}

/* Text at (x, y), top-left origin, each font dot `px` pixels. Characters
 * outside the font draw as a space. */
static void text(const char *s, float x, float y, float px)
{
    glBegin(GL_QUADS);
    for (; *s; s++, x += 6.0f * px) {
        const unsigned char *g = hud_glyph(*s);
        if (!g)
            continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    quad(x + (float)col * px, y + (float)row * px, px, px);
    }
    glEnd();
}

static void refresh_text(SDL_Window *window, int w, int h)
{
    struct hud_settings st = {.render_w = w, .render_h = h, .vsync = SDL_GL_GetSwapInterval(),
                              .heap_guard = guest_heap_guard_mode(), .count_a = "DRAWS", .count_b = "MAPS"};
    SDL_GetWindowSize(window, &st.win_w, &st.win_h);
    hud_refresh_text(&st);
}

static void log_frame(Uint64 now, double dt_ms)
{
    if (s_log_state < 0) {
        const char *path = getenv("RECOMP_FRAMELOG");
        s_log_state = 0;
        if (path && path[0]) {
            s_log = fopen(path, "w");
            if (!s_log) {
                fprintf(stderr, "frontend: cannot open frame log %s\n", path);
            } else {
                setvbuf(s_log, s_log_buf, _IOFBF, sizeof s_log_buf);
                fputs("t_s,frame_ms\n", s_log);
                s_log_state = 1;
                fprintf(stderr, "frontend: frame log %s (t_s 0 = %.3f s uptime)\n", path, s_log_t0_uptime_s);
            }
        }
    }
    if (s_log_state != 1)
        return;
    double t = (double)(now - s_log_t0) / (double)SDL_GetPerformanceFrequency();
    fprintf(s_log, "%.4f,%.3f\n", t, dt_ms);
    if (t - s_log_flushed >= 1.0) {
        fflush(s_log);
        s_log_flushed = t;
    }
}

void fe_hud_toggle(void)
{
    hud_toggle();
}

void fe_hud_swap_done(void)
{
    if (s_swap_start)
        hud_add_swap((double)(SDL_GetPerformanceCounter() - s_swap_start) * 1000.0 /
                     (double)SDL_GetPerformanceFrequency());
}

void fe_hud_frame(SDL_Window *window, int w, int h)
{
    slowframe_mark();
    int on = hud_on();
    Uint64 now = SDL_GetPerformanceCounter();
    if (!s_log_t0) {
        s_log_t0 = now;
        s_log_t0_uptime_s = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    }
    if (s_last) {
        double dt_ms = (double)(now - s_last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        hud_record_frame(dt_ms, __atomic_exchange_n(&g_hud_draws, 0u, __ATOMIC_RELAXED),
                         __atomic_exchange_n(&g_hud_maps, 0u, __ATOMIC_RELAXED));
        log_frame(now, dt_ms);
    }
    s_last = now;
    if (!on) {
        s_swap_start = SDL_GetPerformanceCounter();
        return;
    }
    if (w <= 0 || h <= 0)
        return;
    if (hud_text_due())
        refresh_text(window, w, h);
    int win_w = 0;
    SDL_GetWindowSize(window, &win_w, NULL);
    float px = win_w > 0 && w > win_w ? 4.0f : 2.0f;  /* Retina: twice the dots */

    GLint program = 0, fbo = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, w, h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glUseProgram(0);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    for (GLenum unit = 0; unit < 8; unit++) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    struct hud_layout l;
    hud_layout(px, &l);
    glColor4f(0.0f, 0.0f, 0.0f, 0.6f);
    glBegin(GL_QUADS);
    quad(l.box_x, l.box_y, l.box_w, l.box_h);
    glEnd();

    /* The text only changes on a refresh: compile it then, replay it otherwise. */
    if (!s_text_list)
        s_text_list = glGenLists(1);
    if (s_text_list && hud_text_dirty_take()) {
        glNewList(s_text_list, GL_COMPILE);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        for (int i = 0; i < HUD_LINES; i++)
            text(hud_line(i), l.x0, l.y0 + (float)i * l.line_h, px);
        glEndList();
    }
    if (s_text_list)
        glCallList(s_text_list);

    /* Graph: one bar per frame, oldest on the left; green under 16.7 ms,
     * yellow under 33.3 ms, red above. */
    glBegin(GL_QUADS);
    for (unsigned i = 0, n = hud_frame_count(); i < n; i++) {
        float r[4], c[4];
        hud_bar(&l, i, r, c);
        glColor4f(c[0], c[1], c[2], c[3]);
        quad(r[0], r[1], r[2], r[3]);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
    for (int k = 1; k <= 2; k++)
        quad(l.x0, hud_ref_line_y(&l, k), l.gw, px * 0.5f);
    glEnd();

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopAttrib();
    glUseProgram((GLuint)program);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)fbo);
    s_swap_start = SDL_GetPerformanceCounter();
}
