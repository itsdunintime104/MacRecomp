/* D3D9 draw backend of the frame-time overlay (runtime/src/d3d9_hud.c); the
 * state, text and layout are the engine's hud_core.h. RECOMP_HUD=1 shows it
 * from the start; Cmd+F (hud_toggle) toggles it. */
#ifndef D3D9_HUD_H
#define D3D9_HUD_H

#include <stdint.h>

#include "hud_core.h"

/* Before each host IDirect3DDevice9::Present: record the frame time and,
 * when the overlay is on, draw it into the device's back buffer. */
void d3d9_hud_present(void *host_device);
/* Every HP_TRACE_D3D9 event (d3d9g_trace), counted whether or not the trace
 * is on: draws and render-target sets per frame, and the end of each
 * Present for the time spent in it. */
void d3d9_hud_event(int ev);

/* ---- Exported for tests/test_d3d9_hud.c ---- */

#define D3D9_HUD_MAX_QUADS (1 + HUD_LINES * HUD_LINE_LEN + HUD_FRAMES + 2)

/* XYZRHW | DIFFUSE | TEX1 */
struct d3d9_hud_vertex {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};

/* The overlay as quads (6 vertices each) in back-buffer pixels, each font dot
 * px pixels: the backdrop, the text, one bar per kept frame and the 16.7 and
 * 33.3 ms lines (hud_layout). Returns the quad count. */
unsigned d3d9_hud_build(struct d3d9_hud_vertex *v, float px);

#endif
