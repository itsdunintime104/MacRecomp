/* D3D9 draw backend of the frame-time overlay (engine runtime/src/hud_core.c
 * holds the state, text, layout and font; frontend_hud.c is its GL backend).
 * Drawn over the game's back buffer just before each host Present, with the
 * same lines, graph and layout. RECOMP_HUD=1 shows it from the start; Cmd+F
 * toggles it (win32_ui.c). Off, it only records frame times and makes no
 * device call.
 *
 * It draws with the game's host device: one DrawPrimitiveUP of pre-transformed
 * (XYZRHW) quads textured from a font atlas built from hud_glyph, so the
 * game's matrices and shaders cannot affect it. A D3DSBT_ALL state block,
 * made and applied each frame, restores every state it sets (it is released
 * at once, so none is alive when the game calls Reset); the render target and
 * depth surface, which a state block does not hold, are saved and restored by
 * hand. */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
/* DXVK Native defines THIS_ and THIS empty in C; restore the MinGW ones, as
 * d3d9_guest_manual.c does. */
#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>

#include "d3d9_guest.h"
#include "guest_heap.h"
#include "d3d9_hud.h"
#include "hud_core.h"

#define FONT_W 256    /* atlas: 6x8 cells, glyph i at x = 6 * i */
#define FONT_H 8
#define SOLID_CELL 41 /* an all-white cell for the backdrop and bars */

/* The atlas cell of each font character; the order of cells only has to
 * match between font() and d3d9_hud_build. */
static const char CELL_CHARS[] = "0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ%:-x";

static uint32_t argb(const float c[4])
{
    return (uint32_t)(c[3] * 255.0f + 0.5f) << 24 | (uint32_t)(c[0] * 255.0f + 0.5f) << 16 |
           (uint32_t)(c[1] * 255.0f + 0.5f) << 8 | (uint32_t)(c[2] * 255.0f + 0.5f);
}

/* One quad as two triangles; (x, y, w, h) in pixels, cell the atlas cell
 * (SOLID_CELL samples its centre). The -0.5 maps texel centres to pixel
 * centres (D3D9 rasterisation rules). */
static struct d3d9_hud_vertex *quad(struct d3d9_hud_vertex *v, float x, float y, float w, float h, int cell,
                                  uint32_t color)
{
    float u0 = (float)(6 * cell) / FONT_W, u1 = (float)(6 * cell + 6) / FONT_W, v0 = 0.0f, v1 = 1.0f;
    if (cell == SOLID_CELL)
        u0 = u1 = (6.0f * cell + 3.0f) / FONT_W, v0 = v1 = 0.5f;
    x -= 0.5f;
    y -= 0.5f;
    const struct d3d9_hud_vertex c[4] = {
        {x, y, 0.0f, 1.0f, color, u0, v0},
        {x + w, y, 0.0f, 1.0f, color, u1, v0},
        {x + w, y + h, 0.0f, 1.0f, color, u1, v1},
        {x, y + h, 0.0f, 1.0f, color, u0, v1},
    };
    v[0] = c[0], v[1] = c[1], v[2] = c[2];
    v[3] = c[0], v[4] = c[2], v[5] = c[3];
    return v + 6;
}

unsigned d3d9_hud_build(struct d3d9_hud_vertex *v, float px)
{
    static const float k_box[4] = {0.0f, 0.0f, 0.0f, 0.6f}, k_line[4] = {1.0f, 1.0f, 1.0f, 0.35f};
    struct d3d9_hud_vertex *p = v;
    struct hud_layout l;
    hud_layout(px, &l);
    p = quad(p, l.box_x, l.box_y, l.box_w, l.box_h, SOLID_CELL, argb(k_box));
    for (int i = 0; i < HUD_LINES; i++) {
        float x = l.x0, y = l.y0 + (float)i * l.line_h;
        for (const char *s = hud_line(i); *s; s++, x += 6.0f * px)
            if (hud_glyph(*s) != NULL)
                p = quad(p, x, y, 6.0f * px, (float)FONT_H * px, (int)(strchr(CELL_CHARS, *s) - CELL_CHARS),
                         0xFFFFFFFFu);
    }
    for (unsigned i = 0, n = hud_frame_count(); i < n; i++) {
        float r[4], c[4];
        hud_bar(&l, i, r, c);
        p = quad(p, r[0], r[1], r[2], r[3], SOLID_CELL, argb(c));
    }
    for (int k = 1; k <= 2; k++)
        p = quad(p, l.x0, hud_ref_line_y(&l, k), l.gw, px * 0.5f, SOLID_CELL, argb(k_line));
    return (unsigned)(p - v) / 6u;
}

/* ---- Frame state ------------------------------------------------------- */

/* The event counts and the Present end come from any D3D9 thread; the rest
 * is written only by the thread that presents. */
static atomic_uint s_draws, s_rt_sets;
static atomic_llong s_present_end_ns;
static int64_t s_last_ns, s_present_ns;
static float s_px = 2.0f;
static IDirect3DDevice9 *s_font_dev;
static IDirect3DTexture9 *s_font;
static struct d3d9_hud_vertex s_verts[D3D9_HUD_MAX_QUADS * 6];

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void d3d9_hud_event(int ev)
{
    if (ev == D3D9G_TR_DRAW)
        atomic_fetch_add_explicit(&s_draws, 1u, memory_order_relaxed);
    else if (ev == D3D9G_TR_SET_RENDER_TARGET)
        atomic_fetch_add_explicit(&s_rt_sets, 1u, memory_order_relaxed);
    else if (ev == D3D9G_TR_PRESENT)
        atomic_store_explicit(&s_present_end_ns, now_ns(), memory_order_relaxed);
}

/* Twice a second: the device's sizes and vsync for the text. */
static void refresh_text(IDirect3DDevice9 *dev, int render_w, int render_h)
{
    struct hud_settings st = {.render_w = render_w, .render_h = render_h, .heap_guard = guest_heap_guard_mode(),
                              .count_a = "DRAWS", .count_b = "RT SETS"};
    IDirect3DSwapChain9 *sc = NULL;
    D3DPRESENT_PARAMETERS pp;
    if (SUCCEEDED(dev->lpVtbl->GetSwapChain(dev, 0, &sc)) && SUCCEEDED(sc->lpVtbl->GetPresentParameters(sc, &pp))) {
        st.vsync = pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
        if (pp.hDeviceWindow != NULL) /* DXVK Native on SDL2: the HWND is the SDL_Window */
            SDL_GetWindowSize((SDL_Window *)pp.hDeviceWindow, &st.win_w, &st.win_h);
    }
    if (sc != NULL)
        sc->lpVtbl->Release(sc);
    hud_refresh_text(&st);
    s_px = st.win_w > 0 && render_w > st.win_w ? 4.0f : 2.0f; /* Retina: twice the dots */
}

/* The font atlas, MANAGED so it survives Reset. A new device (the game
 * makes one) gets a new atlas; the old one went with its device. */
static IDirect3DTexture9 *font(IDirect3DDevice9 *dev)
{
    if (s_font_dev == dev)
        return s_font;
    s_font_dev = dev;
    s_font = NULL;
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(dev->lpVtbl->CreateTexture(dev, FONT_W, FONT_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
        return NULL;
    if (FAILED(t->lpVtbl->LockRect(t, 0, &lr, NULL, 0))) {
        t->lpVtbl->Release(t);
        return NULL;
    }
    for (int y = 0; y < FONT_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch);
        for (int x = 0; x < FONT_W; x++) {
            int cell = x / 6, col = x % 6;
            const unsigned char *g = cell < SOLID_CELL ? hud_glyph(CELL_CHARS[cell]) : NULL;
            int on = cell == SOLID_CELL || (g != NULL && col < 5 && y < 7 && (g[y] & (0x10 >> col)) != 0);
            row[x] = on ? 0xFFFFFFFFu : 0x00FFFFFFu;
        }
    }
    t->lpVtbl->UnlockRect(t, 0);
    s_font = t;
    return t;
}

static void set_draw_state(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex)
{
    static const DWORD rs[][2] = {
        {D3DRS_ZENABLE, D3DZB_FALSE},
        {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, TRUE},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
        {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
        {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xF},
        {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_FOGENABLE, FALSE},
        {D3DRS_SPECULARENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE},
        {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF},
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++)
        dev->lpVtbl->SetRenderState(dev, (D3DRENDERSTATETYPE)rs[i][0], rs[i][1]);
    dev->lpVtbl->SetVertexShader(dev, NULL);
    dev->lpVtbl->SetPixelShader(dev, NULL);
    dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
}

static void draw(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *old_rt = NULL, *old_ds = NULL;
    IDirect3DStateBlock9 *sb = NULL;
    IDirect3DTexture9 *tex;
    D3DSURFACE_DESC d;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    if (FAILED(bb->lpVtbl->GetDesc(bb, &d)) || (tex = font(dev)) == NULL ||
        FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &sb)) ||
        FAILED(dev->lpVtbl->GetRenderTarget(dev, 0, &old_rt))) {
        if (sb != NULL)
            sb->lpVtbl->Release(sb);
        bb->lpVtbl->Release(bb);
        return;
    }
    if (FAILED(dev->lpVtbl->GetDepthStencilSurface(dev, &old_ds)))
        old_ds = NULL;
    if (hud_text_due())
        refresh_text(dev, (int)d.Width, (int)d.Height);

    /* SetRenderTarget resets the viewport and scissor; sb->Apply restores them. */
    dev->lpVtbl->SetRenderTarget(dev, 0, bb);
    dev->lpVtbl->SetDepthStencilSurface(dev, NULL);
    set_draw_state(dev, tex);
    unsigned n = d3d9_hud_build(s_verts, s_px);
    if (SUCCEEDED(dev->lpVtbl->BeginScene(dev))) {
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2 * n, s_verts, sizeof s_verts[0]);
        dev->lpVtbl->EndScene(dev);
    }
    dev->lpVtbl->SetRenderTarget(dev, 0, old_rt);
    dev->lpVtbl->SetDepthStencilSurface(dev, old_ds);
    sb->lpVtbl->Apply(sb);
    sb->lpVtbl->Release(sb);
    old_rt->lpVtbl->Release(old_rt);
    if (old_ds != NULL)
        old_ds->lpVtbl->Release(old_ds);
    bb->lpVtbl->Release(bb);
}

void d3d9_hud_present(void *host_device)
{
    int on = hud_on();
    int64_t now = now_ns();
    /* The previous Present's end (d3d9_hud_event), for the time spent in it. */
    int64_t end = atomic_load_explicit(&s_present_end_ns, memory_order_relaxed);
    if (s_present_ns && end > s_present_ns)
        hud_add_swap((double)(end - s_present_ns) / 1e6);
    if (s_last_ns)
        hud_record_frame((double)(now - s_last_ns) / 1e6, atomic_exchange_explicit(&s_draws, 0u, memory_order_relaxed),
                         atomic_exchange_explicit(&s_rt_sets, 0u, memory_order_relaxed));
    s_last_ns = now;
    if (on && host_device != NULL)
        draw((IDirect3DDevice9 *)host_device);
    s_present_ns = now_ns();
}
