/* d3dx9_41 imports other than the shader pair: textures from an in-memory
 * TGA (HP) or DDS (LEGO Indiana Jones), surface to an in-memory BMP, matrix
 * multiply and look-at, on the host Direct3D 9 objects, and an UNSUPPORTED
 * stub for each import neither game reaches. Contract: runtime/include/d3dx_misc.h;
 * evidence: notes/d3dx_liveness.md. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* DXVK Native defines THIS_ and THIS empty in C; restore the MinGW
 * definitions, as gen_d3d9_guest.c does (notes/dxvk_triangle.md). */
#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>
/* d3dx_guest.h defines the same value without the MAKE_D3DHRESULT macro. */
#undef D3DERR_INVALIDCALL

#include "recomp_types.h"
#include "diag.h"
#include "guest_mem.h"
#include "com_guest.h"
#include "d3d9_guest.h"
#include "d3dx_guest.h"
#include "d3dx_misc.h"
#include "win32_thunk.h"

#define D3DXIFF_BMP 0u

#define TGA_HEADER_SIZE 18u
#define TGA_TRUECOLOR 2u
#define TGA_TOP_LEFT 0x20u  /* image descriptor: first row is the top row */
#define TGA_RIGHT_LEFT 0x10u /* image descriptor: rows run right to left */

#define BMP_HEADER_SIZE 54u /* BITMAPFILEHEADER + BITMAPINFOHEADER */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

static int is_pow2(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

/* ---- D3DXCreateTextureFromFileInMemory ----------------------------------- */

/* Box-filter one 32-bit level into the next (D3DX_DEFAULT MipFilter is
 * D3DX_FILTER_BOX); a dimension of 1 stays 1. */
static void box_down(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst, uint32_t dw, uint32_t dh)
{
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = 2u * x, y0 = 2u * y;
            uint32_t x1 = x0 + 1u < sw ? x0 + 1u : x0, y1 = y0 + 1u < sh ? y0 + 1u : y0;
            for (uint32_t c = 0; c < 4u; c++) {
                uint32_t s = src[4u * (y0 * sw + x0) + c] + src[4u * (y0 * sw + x1) + c] +
                             src[4u * (y1 * sw + x0) + c] + src[4u * (y1 * sw + x1) + c];
                dst[4u * (y * dw + x) + c] = (uint8_t)((s + 2u) >> 2);
            }
        }
}

/* Lock level of tex and copy w x h 32-bit texels in, row by row. */
static HRESULT fill_level(IDirect3DTexture9 *tex, UINT level, const uint8_t *px, uint32_t w, uint32_t h)
{
    D3DLOCKED_RECT lr;
    HRESULT hr = tex->lpVtbl->LockRect(tex, level, &lr, NULL, 0);
    if (FAILED(hr))
        return hr;
    for (uint32_t y = 0; y < h; y++)
        memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch, px + (size_t)y * w * 4u, (size_t)w * 4u);
    return tex->lpVtbl->UnlockRect(tex, level);
}

/* The A8R8G8B8 texture with a full mip chain in D3DPOOL_MANAGED that D3DX
 * builds from a 32-bit uncompressed TGA (the D3DX_DEFAULT arguments of
 * D3DXCreateTextureFromFileInMemoryEx). */
static HRESULT texture_from_tga(IDirect3DDevice9 *dev, const uint8_t *f, uint32_t size, IDirect3DTexture9 **out,
                                uint32_t ret)
{
    const char *fn = "d3dx9_41.dll!D3DXCreateTextureFromFileInMemory";
    if (size < TGA_HEADER_SIZE) {
        diag_unsupported(fn, ret, "file of %u bytes is not a TGA", size);
        return D3DXERR_INVALIDDATA;
    }
    uint32_t idlen = f[0], cmap = f[1], type = f[2], w = rd16(f + 12), h = rd16(f + 14), bpp = f[16], desc = f[17];
    if (cmap != 0 || type != TGA_TRUECOLOR || bpp != 32u || (desc & TGA_RIGHT_LEFT) != 0) {
        diag_unsupported(fn, ret, "image is not an uncompressed 32-bit TGA (bytes %02X %02X %02X ... bpp %u desc 0x%02X)",
                         f[0], f[1], f[2], bpp, desc);
        return D3DXERR_INVALIDDATA;
    }
    if (!is_pow2(w) || !is_pow2(h)) {
        diag_unsupported(fn, ret, "TGA is %ux%u; resizing to a power of two is not implemented", w, h);
        return D3DXERR_INVALIDDATA;
    }
    uint64_t need = (uint64_t)TGA_HEADER_SIZE + idlen + (uint64_t)w * h * 4u;
    if (need > size) {
        diag_unsupported(fn, ret, "%ux%u TGA needs %llu bytes, has %u", w, h, (unsigned long long)need, size);
        return D3DXERR_INVALIDDATA;
    }

    uint32_t levels = 1;
    while ((w >> levels) != 0 || (h >> levels) != 0)
        levels++;
    /* Level 0 top row first, then every smaller level after it. */
    size_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1u, lh = h >> l ? h >> l : 1u;
        total += (size_t)lw * lh * 4u;
    }
    uint8_t *px = malloc(total);
    if (px == NULL)
        return E_OUTOFMEMORY;
    const uint8_t *src = f + TGA_HEADER_SIZE + idlen;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = (desc & TGA_TOP_LEFT) != 0 ? y : h - 1u - y;
        memcpy(px + (size_t)y * w * 4u, src + (size_t)sy * w * 4u, (size_t)w * 4u);
    }
    for (uint32_t l = 1, off = 0; l < levels; l++) {
        uint32_t sw = w >> (l - 1) ? w >> (l - 1) : 1u, sh = h >> (l - 1) ? h >> (l - 1) : 1u;
        uint32_t dw = w >> l ? w >> l : 1u, dh = h >> l ? h >> l : 1u;
        box_down(px + off, sw, sh, px + off + (size_t)sw * sh * 4u, dw, dh);
        off += sw * sh * 4u;
    }

    IDirect3DTexture9 *tex = NULL;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    size_t off = 0;
    for (uint32_t l = 0; SUCCEEDED(hr) && l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1u, lh = h >> l ? h >> l : 1u;
        hr = fill_level(tex, l, px + off, lw, lh);
        off += (size_t)lw * lh * 4u;
    }
    free(px);
    if (FAILED(hr)) {
        if (tex != NULL)
            tex->lpVtbl->Release(tex);
        return hr;
    }
    *out = tex;
    return D3D_OK;
}

/* ---- DDS: D3DXCreateTextureFromFileInMemory(Ex), D3DXCreateCubeTextureFromFileInMemory
 * A DDS file is "DDS ", a 124-byte DDS_HEADER (DDS_PIXELFORMAT at +72), then
 * each face (one, or six for a cube map in +X -X +Y -Y +Z -Z order) as its
 * mip levels, largest first, tightly packed. Formats: DXT1, DXT3 and DXT5 by
 * FourCC, and the uncompressed layouts in g_dds_masks. The texture gets the
 * file's format and size; its level count follows the MipLevels argument
 * (D3DX_DEFAULT or 0: the full chain, D3DX_FROM_FILE: the file's levels).
 * Levels the file does not hold are box-filtered from the level above, as
 * D3DX's default mip filter does (DXT levels are decoded, filtered and
 * encoded again). Resizing, format conversion, colour keys and skipped DDS
 * levels are not implemented and fail with an UNSUPPORTED line. */

#define D3DX_DEFAULT_ARG 0xFFFFFFFFu   /* D3DX_DEFAULT */
#define D3DX_DEFAULT_NONPOW2 0xFFFFFFFEu
#define D3DX_FROM_FILE 0xFFFFFFFDu     /* also D3DFMT_FROM_FILE */
#define D3DX_FILTER_NONE 1u
#define D3DX_SKIP_DDS_MIP_LEVELS_MASK 0x7C000000u
#define D3DXIFF_DDS 4u

#define DDS_HEADER_SIZE 128u /* magic + DDS_HEADER */
#define DDSD_MIPMAPCOUNT 0x20000u
#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_ALPHA 0x2u
#define DDPF_FOURCC 0x4u
#define DDPF_RGB 0x40u
#define DDPF_LUMINANCE 0x20000u
#define DDSCAPS2_CUBEMAP 0x200u
#define DDSCAPS2_CUBEMAP_ALLFACES 0xFC00u
#define DDSCAPS2_VOLUME 0x200000u

static uint32_t rd32le(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

/* Uncompressed layouts: bits per pixel, then the R, G, B and A masks (for
 * luminance formats R is the luminance mask). */
static const struct {
    uint32_t bits, r, g, b, a;
    D3DFORMAT fmt;
} g_dds_masks[] = {
    {32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u, D3DFMT_A8R8G8B8},
    {32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0, D3DFMT_X8R8G8B8},
    {32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u, D3DFMT_A8B8G8R8},
    {32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0, D3DFMT_X8B8G8R8},
    {16, 0xF800u, 0x07E0u, 0x001Fu, 0, D3DFMT_R5G6B5},
    {16, 0x7C00u, 0x03E0u, 0x001Fu, 0x8000u, D3DFMT_A1R5G5B5},
    {16, 0x7C00u, 0x03E0u, 0x001Fu, 0, D3DFMT_X1R5G5B5},
    {16, 0x0F00u, 0x00F0u, 0x000Fu, 0xF000u, D3DFMT_A4R4G4B4},
    {8, 0xFFu, 0, 0, 0, D3DFMT_L8},
    {16, 0x00FFu, 0, 0, 0xFF00u, D3DFMT_A8L8},
    {8, 0, 0, 0, 0xFFu, D3DFMT_A8},
};

struct dds {
    uint32_t w, h, levels, faces;
    D3DFORMAT fmt;
    uint32_t block;  /* bytes per 4x4 block; 0 for an uncompressed format */
    int mask;        /* g_dds_masks row of an uncompressed format */
    int luminance;
    const uint8_t *data;
};

static uint32_t lvl(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1u; }

/* Rows and bytes per row of a w x h level as the file and LockRect lay it
 * out (a row of 4x4 blocks for a DXT format). */
static void level_rows(const struct dds *d, uint32_t w, uint32_t h, uint32_t *rows, uint32_t *row_bytes)
{
    if (d->block != 0) {
        *rows = (h + 3u) / 4u;
        *row_bytes = (w + 3u) / 4u * d->block;
    } else {
        *rows = h;
        *row_bytes = w * (g_dds_masks[d->mask].bits / 8u);
    }
}

/* Parse and bound-check the header and every face and level against size.
 * 0, or an UNSUPPORTED line and -1. */
static int dds_parse(const uint8_t *f, uint32_t size, struct dds *d, const char *fn, uint32_t ret)
{
    if (size < DDS_HEADER_SIZE || memcmp(f, "DDS ", 4) != 0 || rd32le(f + 4) != 124u || rd32le(f + 76) != 32u) {
        diag_unsupported(fn, ret, "file of %u bytes has no DDS header", size);
        return -1;
    }
    uint32_t flags = rd32le(f + 8), mips = rd32le(f + 28), pf = rd32le(f + 80), caps2 = rd32le(f + 112);
    memset(d, 0, sizeof *d);
    d->h = rd32le(f + 12);
    d->w = rd32le(f + 16);
    d->levels = (flags & DDSD_MIPMAPCOUNT) != 0 && mips != 0 ? mips : 1u;
    d->faces = (caps2 & DDSCAPS2_CUBEMAP) != 0 ? 6u : 1u;
    d->data = f + DDS_HEADER_SIZE;
    if (d->w == 0 || d->h == 0 || d->w > 16384u || d->h > 16384u || d->levels > 15u) {
        diag_unsupported(fn, ret, "DDS of %ux%u with %u levels", d->w, d->h, d->levels);
        return -1;
    }
    if ((caps2 & DDSCAPS2_VOLUME) != 0 ||
        (d->faces == 6u && (caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES)) {
        diag_unsupported(fn, ret, "DDS volume or partial cube map (caps2 0x%X)", caps2);
        return -1;
    }
    if ((pf & DDPF_FOURCC) != 0) {
        const uint8_t *cc = f + 84;
        if (memcmp(cc, "DXT1", 4) == 0)
            d->fmt = D3DFMT_DXT1, d->block = 8;
        else if (memcmp(cc, "DXT3", 4) == 0)
            d->fmt = D3DFMT_DXT3, d->block = 16;
        else if (memcmp(cc, "DXT5", 4) == 0)
            d->fmt = D3DFMT_DXT5, d->block = 16;
        else {
            diag_unsupported(fn, ret, "DDS FourCC %c%c%c%c", cc[0], cc[1], cc[2], cc[3]);
            return -1;
        }
    } else {
        uint32_t bits = rd32le(f + 88), r = rd32le(f + 92), g = rd32le(f + 96), b = rd32le(f + 100),
                 a = (pf & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) != 0 ? rd32le(f + 104) : 0;
        d->mask = -1;
        if ((pf & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA)) != 0)
            for (size_t i = 0; i < sizeof g_dds_masks / sizeof g_dds_masks[0]; i++)
                if (g_dds_masks[i].bits == bits && g_dds_masks[i].r == r && g_dds_masks[i].g == g &&
                    g_dds_masks[i].b == b && g_dds_masks[i].a == a)
                    d->mask = (int)i;
        if (d->mask < 0) {
            diag_unsupported(fn, ret, "DDS pixel format flags 0x%X, %u bits, masks %08X %08X %08X %08X", pf, bits,
                             r, g, b, a);
            return -1;
        }
        d->fmt = g_dds_masks[d->mask].fmt;
        d->luminance = (pf & DDPF_LUMINANCE) != 0;
    }
    uint64_t need = DDS_HEADER_SIZE;
    for (uint32_t l = 0; l < d->levels; l++) {
        uint32_t rows, row_bytes;
        level_rows(d, lvl(d->w, l), lvl(d->h, l), &rows, &row_bytes);
        need += (uint64_t)rows * row_bytes * d->faces;
    }
    if (need > size) {
        diag_unsupported(fn, ret, "%ux%u DDS with %u levels and %u faces needs %llu bytes, has %u", d->w, d->h,
                         d->levels, d->faces, (unsigned long long)need, size);
        return -1;
    }
    return 0;
}

/* File bytes of face, level l. */
static const uint8_t *dds_level(const struct dds *d, uint32_t face, uint32_t l)
{
    size_t off = 0;
    for (uint32_t f = 0; f <= face; f++)
        for (uint32_t k = 0; k < d->levels; k++) {
            if (f == face && k == l)
                return d->data + off;
            uint32_t rows, row_bytes;
            level_rows(d, lvl(d->w, k), lvl(d->h, k), &rows, &row_bytes);
            off += (size_t)rows * row_bytes;
        }
    return NULL;
}

/* -- Pixel conversion to and from RGBA8 (4 bytes per texel, R G B A), for
 *    generated levels. */

static void rgb565(uint32_t c, uint8_t *o)
{
    uint32_t r = c >> 11 & 31u, g = c >> 5 & 63u, b = c & 31u;
    o[0] = (uint8_t)(r << 3 | r >> 2), o[1] = (uint8_t)(g << 2 | g >> 4), o[2] = (uint8_t)(b << 3 | b >> 2);
    o[3] = 255;
}

static uint32_t to565(const uint8_t *c)
{
    return (uint32_t)(c[0] * 31u + 127u) / 255u << 11 | (uint32_t)(c[1] * 63u + 127u) / 255u << 5 |
           (uint32_t)(c[2] * 31u + 127u) / 255u;
}

/* The four colours of a DXT colour block; four_colour for DXT3 and DXT5. */
static void dxt_palette(uint32_t c0, uint32_t c1, int four_colour, uint8_t p[4][4])
{
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    for (int i = 0; i < 3; i++) {
        if (c0 > c1 || four_colour) {
            p[2][i] = (uint8_t)((2u * p[0][i] + p[1][i]) / 3u);
            p[3][i] = (uint8_t)((p[0][i] + 2u * p[1][i]) / 3u);
        } else {
            p[2][i] = (uint8_t)((p[0][i] + p[1][i]) / 2u);
            p[3][i] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = c0 > c1 || four_colour ? 255 : 0;
}

static void dxt5_alphas(uint32_t a0, uint32_t a1, uint8_t a[8])
{
    a[0] = (uint8_t)a0, a[1] = (uint8_t)a1;
    for (uint32_t i = 1; i < 7u; i++)
        if (a0 > a1)
            a[i + 1] = (uint8_t)(((7u - i) * a0 + i * a1) / 7u);
        else if (i < 5u)
            a[i + 1] = (uint8_t)(((5u - i) * a0 + i * a1) / 5u);
    if (a0 <= a1)
        a[6] = 0, a[7] = 255;
}

static void decode_level(const struct dds *d, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *px)
{
    if (d->block == 0) {
        uint32_t bpp = g_dds_masks[d->mask].bits / 8u;
        const uint32_t m[4] = {g_dds_masks[d->mask].r, g_dds_masks[d->mask].g, g_dds_masks[d->mask].b,
                               g_dds_masks[d->mask].a};
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t v = 0;
            memcpy(&v, src + (size_t)i * bpp, bpp);
            for (int c = 0; c < 4; c++) {
                uint32_t sh = m[c] ? (uint32_t)__builtin_ctz(m[c]) : 0, max = m[c] >> sh;
                px[4u * i + c] = m[c] ? (uint8_t)(((v & m[c]) >> sh) * 255u / max) : (c == 3 ? 255 : 0);
            }
            if (d->luminance)
                px[4u * i + 1] = px[4u * i + 2] = px[4u * i];
        }
        return;
    }
    for (uint32_t by = 0; by < (h + 3u) / 4u; by++)
        for (uint32_t bx = 0; bx < (w + 3u) / 4u; bx++) {
            const uint8_t *b = src + ((size_t)by * ((w + 3u) / 4u) + bx) * d->block;
            const uint8_t *cb = d->block == 16 ? b + 8 : b;
            uint8_t p[4][4], a[8];
            dxt_palette(rd16(cb), rd16(cb + 2), d->block == 16, p);
            uint32_t idx = rd32le(cb + 4);
            uint64_t abits = 0;
            if (d->fmt == D3DFMT_DXT5) {
                dxt5_alphas(b[0], b[1], a);
                for (int i = 0; i < 6; i++)
                    abits |= (uint64_t)b[2 + i] << (8 * i);
            }
            for (uint32_t k = 0; k < 16u; k++) {
                uint32_t x = 4u * bx + k % 4u, y = 4u * by + k / 4u;
                if (x >= w || y >= h)
                    continue;
                uint8_t *o = px + 4u * ((size_t)y * w + x);
                memcpy(o, p[idx >> (2u * k) & 3u], 4);
                if (d->fmt == D3DFMT_DXT3)
                    o[3] = (uint8_t)((b[k / 2u] >> (4u * (k % 2u)) & 15u) * 17u);
                else if (d->fmt == D3DFMT_DXT5)
                    o[3] = a[abits >> (3u * k) & 7u];
            }
        }
}

static uint32_t dist2(const uint8_t *a, const uint8_t *b)
{
    uint32_t s = 0;
    for (int i = 0; i < 3; i++)
        s += (uint32_t)((a[i] - b[i]) * (a[i] - b[i]));
    return s;
}

/* One DXT colour block from 16 RGBA8 texels: the bounding-box end points and
 * the nearest palette entry per texel. transparent: DXT1 three-colour mode,
 * with index 3 for texels of alpha below 128. */
static void encode_colour(const uint8_t t[16][4], int four_colour, int transparent, uint8_t *out)
{
    uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int k = 0; k < 16; k++)
        if (!transparent || t[k][3] >= 128)
            for (int i = 0; i < 3; i++)
                lo[i] = t[k][i] < lo[i] ? t[k][i] : lo[i], hi[i] = t[k][i] > hi[i] ? t[k][i] : hi[i];
    uint32_t c0 = to565(hi), c1 = to565(lo);
    if (transparent ? c0 > c1 : c0 < c1) {
        uint32_t s = c0;
        c0 = c1, c1 = s;
    }
    uint8_t p[4][4];
    dxt_palette(c0, c1, four_colour, p);
    int n = transparent ? 3 : (c0 == c1 && !four_colour ? 1 : 4);
    uint32_t idx = 0;
    for (int k = 0; k < 16; k++) {
        uint32_t best = 0, bd = UINT32_MAX;
        if (transparent && t[k][3] < 128)
            best = 3;
        else
            for (int i = 0; i < n; i++) {
                uint32_t dd = dist2(t[k], p[i]);
                if (dd < bd)
                    bd = dd, best = (uint32_t)i;
            }
        idx |= best << (2 * k);
    }
    wr16(out, c0);
    wr16(out + 2, c1);
    wr32(out + 4, idx);
}

static void encode_level(const struct dds *d, const uint8_t *px, uint32_t w, uint32_t h, uint8_t *dst)
{
    if (d->block == 0) {
        uint32_t bpp = g_dds_masks[d->mask].bits / 8u;
        const uint32_t m[4] = {g_dds_masks[d->mask].r, g_dds_masks[d->mask].g, g_dds_masks[d->mask].b,
                               g_dds_masks[d->mask].a};
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t v = 0;
            for (int c = 0; c < 4; c++)
                if (m[c]) {
                    uint32_t sh = (uint32_t)__builtin_ctz(m[c]), max = m[c] >> sh;
                    v |= (px[4u * i + c] * max + 127u) / 255u << sh;
                }
            memcpy(dst + (size_t)i * bpp, &v, bpp);
        }
        return;
    }
    for (uint32_t by = 0; by < (h + 3u) / 4u; by++)
        for (uint32_t bx = 0; bx < (w + 3u) / 4u; bx++) {
            uint8_t t[16][4], *b = dst + ((size_t)by * ((w + 3u) / 4u) + bx) * d->block;
            int transparent = 0;
            uint32_t amin = 255, amax = 0;
            for (uint32_t k = 0; k < 16u; k++) {
                uint32_t x = 4u * bx + k % 4u, y = 4u * by + k / 4u;
                memcpy(t[k], px + 4u * ((size_t)(y < h ? y : h - 1u) * w + (x < w ? x : w - 1u)), 4);
                transparent |= t[k][3] < 128;
                amin = t[k][3] < amin ? t[k][3] : amin, amax = t[k][3] > amax ? t[k][3] : amax;
            }
            if (d->fmt == D3DFMT_DXT1) {
                encode_colour(t, 0, transparent, b);
                continue;
            }
            memset(b, 0, 8);
            if (d->fmt == D3DFMT_DXT3) {
                for (uint32_t k = 0; k < 16u; k++)
                    b[k / 2u] |= (uint8_t)((t[k][3] * 15u + 127u) / 255u << (4u * (k % 2u)));
            } else {
                uint8_t a[8];
                uint64_t bits = 0;
                dxt5_alphas(amax, amin, a);
                for (uint32_t k = 0; amax != amin && k < 16u; k++) {
                    uint32_t best = 0, bd = UINT32_MAX;
                    for (uint32_t i = 0; i < 8u; i++) {
                        uint32_t dd = (uint32_t)abs(t[k][3] - a[i]);
                        if (dd < bd)
                            bd = dd, best = i;
                    }
                    bits |= (uint64_t)best << (3u * k);
                }
                b[0] = (uint8_t)amax, b[1] = (uint8_t)amin;
                for (int i = 0; i < 6; i++)
                    b[2 + i] = (uint8_t)(bits >> (8 * i));
            }
            encode_colour(t, 1, 0, b + 8);
        }
}

/* Lock face, level of tex (a cube texture when cube) and copy rows of
 * row_bytes in, honouring the locked pitch. */
static HRESULT copy_level(void *tex, int cube, uint32_t face, uint32_t level, const uint8_t *src, uint32_t rows,
                          uint32_t row_bytes)
{
    D3DLOCKED_RECT lr;
    IDirect3DTexture9 *t2 = tex;
    IDirect3DCubeTexture9 *tc = tex;
    HRESULT hr = cube ? tc->lpVtbl->LockRect(tc, (D3DCUBEMAP_FACES)face, level, &lr, NULL, 0)
                      : t2->lpVtbl->LockRect(t2, level, &lr, NULL, 0);
    if (FAILED(hr))
        return hr;
    for (uint32_t y = 0; y < rows; y++)
        memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch, src + (size_t)y * row_bytes, row_bytes);
    return cube ? tc->lpVtbl->UnlockRect(tc, (D3DCUBEMAP_FACES)face, level) : t2->lpVtbl->UnlockRect(t2, level);
}

/* The Ex arguments that select the texture (the rest of the 15). */
struct dds_args {
    uint32_t width, height, mips, usage, format, pool, filter, mipfilter, colorkey;
};

static const struct dds_args DDS_DEFAULTS = {D3DX_DEFAULT_ARG, D3DX_DEFAULT_ARG, D3DX_DEFAULT_ARG, 0,
                                             D3DFMT_UNKNOWN,   D3DPOOL_MANAGED,  D3DX_DEFAULT_ARG, D3DX_DEFAULT_ARG,
                                             0};

static uint32_t pow2_up(uint32_t v)
{
    uint32_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* The texture (a cube texture when cube) from the DDS file f. */
static HRESULT texture_from_dds(IDirect3DDevice9 *dev, const uint8_t *f, uint32_t size, const struct dds_args *a,
                                int cube, void **out, struct dds *info, const char *fn, uint32_t ret)
{
    struct dds d;
    if (dds_parse(f, size, &d, fn, ret) != 0)
        return D3DXERR_INVALIDDATA;
    if ((d.faces == 6u) != cube) {
        diag_unsupported(fn, ret, "DDS has %u faces", d.faces);
        return D3DXERR_INVALIDDATA;
    }
    uint32_t w = a->width == D3DX_DEFAULT_ARG ? pow2_up(d.w)
                 : a->width == 0 || a->width >= D3DX_FROM_FILE ? d.w : a->width;
    uint32_t h = a->height == D3DX_DEFAULT_ARG ? pow2_up(d.h)
                 : a->height == 0 || a->height >= D3DX_FROM_FILE ? d.h : a->height;
    if (cube)
        h = w;
    uint32_t fmt = a->format == D3DFMT_UNKNOWN || a->format >= D3DX_FROM_FILE ? (uint32_t)d.fmt : a->format;
    int skip = a->mipfilter != D3DX_DEFAULT_ARG && (a->mipfilter & D3DX_SKIP_DDS_MIP_LEVELS_MASK) != 0;
    if (w != d.w || h != d.h || fmt != (uint32_t)d.fmt || a->colorkey != 0 || skip) {
        diag_unsupported(fn, ret,
                         "DDS %ux%u format %u to %ux%u format %u, colour key 0x%08X, mip filter 0x%X: resizing, "
                         "conversion, colour keys and skipped levels are not implemented",
                         d.w, d.h, (unsigned)d.fmt, w, h, fmt, a->colorkey, a->mipfilter);
        return D3DXERR_INVALIDDATA;
    }
    uint32_t full = 1;
    while ((w >> full) != 0 || (h >> full) != 0)
        full++;
    uint32_t levels = a->mips == 0 || a->mips == D3DX_DEFAULT_ARG ? full
                      : a->mips == D3DX_FROM_FILE                 ? d.levels
                      : a->mips < full                            ? a->mips
                                                                  : full;
    uint32_t from_file = d.levels < levels ? d.levels : levels;
    if (a->mipfilter == D3DX_FILTER_NONE)
        levels = from_file;

    void *tex = NULL;
    HRESULT hr = cube ? dev->lpVtbl->CreateCubeTexture(dev, w, levels, a->usage, d.fmt, (D3DPOOL)a->pool,
                                                       (IDirect3DCubeTexture9 **)&tex, NULL)
                      : dev->lpVtbl->CreateTexture(dev, w, h, levels, a->usage, d.fmt, (D3DPOOL)a->pool,
                                                   (IDirect3DTexture9 **)&tex, NULL);
    uint8_t *px = NULL, *next = NULL, *enc = NULL;
    if (SUCCEEDED(hr) && from_file < levels) {
        px = malloc((size_t)w * h * 4u);
        next = malloc((size_t)w * h * 4u);
        enc = malloc((size_t)w * h * 4u + 16u);
        if (px == NULL || next == NULL || enc == NULL)
            hr = E_OUTOFMEMORY;
    }
    for (uint32_t face = 0; SUCCEEDED(hr) && face < d.faces; face++) {
        for (uint32_t l = 0; SUCCEEDED(hr) && l < from_file; l++) {
            uint32_t rows, row_bytes;
            level_rows(&d, lvl(w, l), lvl(h, l), &rows, &row_bytes);
            hr = copy_level(tex, cube, face, l, dds_level(&d, face, l), rows, row_bytes);
        }
        if (FAILED(hr) || from_file == levels)
            continue;
        uint32_t l = from_file - 1u;
        decode_level(&d, dds_level(&d, face, l), lvl(w, l), lvl(h, l), px);
        for (l = from_file; SUCCEEDED(hr) && l < levels; l++) {
            uint32_t rows, row_bytes;
            box_down(px, lvl(w, l - 1u), lvl(h, l - 1u), next, lvl(w, l), lvl(h, l));
            memcpy(px, next, (size_t)lvl(w, l) * lvl(h, l) * 4u);
            encode_level(&d, px, lvl(w, l), lvl(h, l), enc);
            level_rows(&d, lvl(w, l), lvl(h, l), &rows, &row_bytes);
            hr = copy_level(tex, cube, face, l, enc, rows, row_bytes);
        }
    }
    free(px);
    free(next);
    free(enc);
    if (FAILED(hr)) {
        if (tex != NULL)
            ((IUnknown *)tex)->lpVtbl->Release((IUnknown *)tex);
        return hr;
    }
    *out = tex;
    *info = d;
    return D3D_OK;
}

/* D3DXIMAGE_INFO: Width, Height, Depth, MipLevels, Format, ResourceType,
 * ImageFileFormat. */
static void write_image_info(uint32_t a_info, const struct dds *d)
{
    if (a_info == 0)
        return;
    const uint32_t v[7] = {d->w, d->h, 1u, d->levels, (uint32_t)d->fmt,
                           d->faces == 6u ? (uint32_t)D3DRTYPE_CUBETEXTURE : (uint32_t)D3DRTYPE_TEXTURE, D3DXIFF_DDS};
    memcpy(guest_ptr(a_info), v, sizeof v);
}

void thunk_D3DXCreateTextureFromFileInMemory(void)
{
    uint32_t a_dev = W32_ARG(0), a_data = W32_ARG(1), size = W32_ARG(2), a_out = W32_ARG(3);
    if (a_dev == 0 || a_data == 0 || size == 0 || a_out == 0) {
        W32_RETURN(4, D3DERR_INVALIDCALL);
        return;
    }
    IDirect3DDevice9 *dev = com_guest_self(a_dev);
    const uint8_t *f = guest_ptr(a_data);
    void *tex = NULL;
    struct dds info;
    HRESULT hr = size >= 4u && memcmp(f, "DDS ", 4) == 0
                     ? texture_from_dds(dev, f, size, &DDS_DEFAULTS, 0, &tex, &info,
                                        "d3dx9_41.dll!D3DXCreateTextureFromFileInMemory", MEM32(g_esp))
                     : texture_from_tga(dev, f, size, (IDirect3DTexture9 **)&tex, MEM32(g_esp));
    if (SUCCEEDED(hr))
        MEM32(a_out) = d3d9g_wrap(tex, &d3d9g_iface_IDirect3DTexture9);
    W32_RETURN(4, (uint32_t)hr);
}

/* HRESULT D3DXCreateTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize,
 *     Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter,
 *     ColorKey, pSrcInfo, pPalette, ppTexture): DDS files only. */
void thunk_D3DXCreateTextureFromFileInMemoryEx(void)
{
    const char *fn = "d3dx9_41.dll!D3DXCreateTextureFromFileInMemoryEx";
    uint32_t a_dev = W32_ARG(0), a_data = W32_ARG(1), size = W32_ARG(2), a_info = W32_ARG(12),
             a_out = W32_ARG(14);
    if (a_dev == 0 || a_data == 0 || size == 0 || a_out == 0) {
        W32_RETURN(15, D3DERR_INVALIDCALL);
        return;
    }
    const struct dds_args a = {W32_ARG(3), W32_ARG(4), W32_ARG(5),  W32_ARG(6), W32_ARG(7),
                               W32_ARG(8), W32_ARG(9), W32_ARG(10), W32_ARG(11)};
    void *tex = NULL;
    struct dds info;
    HRESULT hr = texture_from_dds(com_guest_self(a_dev), guest_ptr(a_data), size, &a, 0, &tex, &info, fn,
                                  MEM32(g_esp));
    if (SUCCEEDED(hr)) {
        write_image_info(a_info, &info);
        MEM32(a_out) = d3d9g_wrap(tex, &d3d9g_iface_IDirect3DTexture9);
    }
    W32_RETURN(15, (uint32_t)hr);
}

void thunk_D3DXCreateCubeTextureFromFileInMemory(void)
{
    uint32_t a_dev = W32_ARG(0), a_data = W32_ARG(1), size = W32_ARG(2), a_out = W32_ARG(3);
    if (a_dev == 0 || a_data == 0 || size == 0 || a_out == 0) {
        W32_RETURN(4, D3DERR_INVALIDCALL);
        return;
    }
    void *tex = NULL;
    struct dds info;
    HRESULT hr = texture_from_dds(com_guest_self(a_dev), guest_ptr(a_data), size, &DDS_DEFAULTS, 1, &tex, &info,
                                  "d3dx9_41.dll!D3DXCreateCubeTextureFromFileInMemory", MEM32(g_esp));
    if (SUCCEEDED(hr))
        MEM32(a_out) = d3d9g_wrap(tex, &d3d9g_iface_IDirect3DCubeTexture9);
    W32_RETURN(4, (uint32_t)hr);
}

/* ---- D3DXSaveSurfaceToFileInMemory --------------------------------------- */

/* A bottom-up 32-bit BI_RGB BMP of an A8R8G8B8 surface, the layout the
 * game's reader expects (file size at +2, pixels at +54, 4 bytes
 * each). */
static HRESULT bmp_from_surface(IDirect3DSurface9 *s, uint32_t *buf, uint32_t ret)
{
    const char *fn = "d3dx9_41.dll!D3DXSaveSurfaceToFileInMemory";
    D3DSURFACE_DESC d;
    HRESULT hr = s->lpVtbl->GetDesc(s, &d);
    if (FAILED(hr))
        return hr;
    if (d.Format != D3DFMT_A8R8G8B8) {
        diag_unsupported(fn, ret, "surface format %u is not A8R8G8B8", (unsigned)d.Format);
        return D3DERR_INVALIDCALL;
    }
    uint64_t bytes = (uint64_t)d.Width * d.Height * 4u;
    if (bytes > 0x7FFFFFFFu - BMP_HEADER_SIZE)
        return E_OUTOFMEMORY;
    uint32_t size = BMP_HEADER_SIZE + (uint32_t)bytes;
    uint8_t *f = calloc(1, size);
    if (f == NULL)
        return E_OUTOFMEMORY;
    f[0] = 'B';
    f[1] = 'M';
    wr32(f + 2, size);
    wr32(f + 10, BMP_HEADER_SIZE);
    wr32(f + 14, 40u); /* biSize */
    wr32(f + 18, d.Width);
    wr32(f + 22, d.Height); /* positive: bottom row first */
    wr16(f + 26, 1u);       /* biPlanes */
    wr16(f + 28, 32u);      /* biBitCount; biCompression BI_RGB = 0 */
    wr32(f + 34, (uint32_t)bytes);

    D3DLOCKED_RECT lr;
    hr = s->lpVtbl->LockRect(s, &lr, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        for (uint32_t y = 0; y < d.Height; y++)
            memcpy(f + BMP_HEADER_SIZE + (size_t)y * d.Width * 4u,
                   (const uint8_t *)lr.pBits + (size_t)(d.Height - 1u - y) * (size_t)lr.Pitch, (size_t)d.Width * 4u);
        hr = s->lpVtbl->UnlockRect(s);
    }
    if (SUCCEEDED(hr)) {
        *buf = d3dx_buffer_new(f, size);
        if (*buf == 0)
            hr = E_OUTOFMEMORY;
    }
    free(f);
    return hr;
}

void thunk_D3DXSaveSurfaceToFileInMemory(void)
{
    uint32_t a_out = W32_ARG(0), fmt = W32_ARG(1), a_surf = W32_ARG(2), a_pal = W32_ARG(3), a_rect = W32_ARG(4);
    if (a_out == 0 || a_surf == 0) {
        W32_RETURN(5, D3DERR_INVALIDCALL);
        return;
    }
    if (fmt != D3DXIFF_BMP || a_pal != 0 || a_rect != 0) {
        diag_unsupported("d3dx9_41.dll!D3DXSaveSurfaceToFileInMemory", MEM32(g_esp),
                         "only a whole surface to BMP is implemented (format %u, palette 0x%08X, rect 0x%08X)", fmt,
                         a_pal, a_rect);
        W32_RETURN(5, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t buf = 0;
    HRESULT hr = bmp_from_surface(com_guest_self(a_surf), &buf, MEM32(g_esp));
    if (SUCCEEDED(hr))
        MEM32(a_out) = buf;
    W32_RETURN(5, (uint32_t)hr);
}

/* ---- D3DXMatrixMultiply --------------------------------------------------- */

void thunk_D3DXMatrixMultiply(void)
{
    uint32_t a_out = W32_ARG(0), a_m1 = W32_ARG(1), a_m2 = W32_ARG(2);
    float m1[16], m2[16], r[16];
    memcpy(m1, guest_ptr(a_m1), sizeof m1);
    memcpy(m2, guest_ptr(a_m2), sizeof m2);
    for (unsigned i = 0; i < 4u; i++)
        for (unsigned j = 0; j < 4u; j++)
            r[4u * i + j] = m1[4u * i] * m2[j] + m1[4u * i + 1u] * m2[4u + j] + m1[4u * i + 2u] * m2[8u + j] +
                            m1[4u * i + 3u] * m2[12u + j];
    /* pOut may be pM1 or pM2: both are read before the store. */
    memcpy(guest_ptr(a_out), r, sizeof r);
    W32_RETURN(3, a_out);
}

/* ---- D3DXMatrixLookAtLH --------------------------------------------------- */

static void vec3_normalize(float v[3])
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (unsigned i = 0; i < 3u; i++)
        v[i] = len != 0.0f ? v[i] / len : 0.0f;
}

static void vec3_cross(float out[3], const float a[3], const float b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float vec3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The D3DX documentation's left-handed view matrix:
 *   z = normal(at - eye), x = normal(cross(up, z)), y = cross(z, x)
 *   rows: (x.x y.x z.x 0) (x.y y.y z.y 0) (x.z y.z z.z 0)
 *         (-dot(x, eye) -dot(y, eye) -dot(z, eye) 1) */
void thunk_D3DXMatrixLookAtLH(void)
{
    uint32_t a_out = W32_ARG(0), a_eye = W32_ARG(1), a_at = W32_ARG(2), a_up = W32_ARG(3);
    float eye[3], at[3], up[3], x[3], y[3], z[3];
    memcpy(eye, guest_ptr(a_eye), sizeof eye);
    memcpy(at, guest_ptr(a_at), sizeof at);
    memcpy(up, guest_ptr(a_up), sizeof up);
    for (unsigned i = 0; i < 3u; i++)
        z[i] = at[i] - eye[i];
    vec3_normalize(z);
    vec3_cross(x, up, z);
    vec3_normalize(x);
    vec3_cross(y, z, x);
    float m[16] = {x[0], y[0], z[0], 0.0f, x[1], y[1], z[1], 0.0f, x[2], y[2], z[2], 0.0f,
                   -vec3_dot(x, eye), -vec3_dot(y, eye), -vec3_dot(z, eye), 1.0f};
    memcpy(guest_ptr(a_out), m, sizeof m);
    W32_RETURN(4, a_out);
}

/* ---- Imports the game never reaches (notes/d3dx_liveness.md) -------------- */

#define D3DX_UNREACHED(name, nargs)                                                                  \
    void thunk_##name(void)                                                                          \
    {                                                                                                \
        diag_unsupported("d3dx9_41.dll!" #name, MEM32(g_esp), "not implemented: the game never calls it"); \
        W32_RETURN(nargs, D3DERR_INVALIDCALL);                                                       \
    }

D3DX_UNREACHED(D3DXCreateEffectFromFileA, 8)
D3DX_UNREACHED(D3DXCreateEffect, 9)
D3DX_UNREACHED(D3DXCreateEffectCompiler, 7)
D3DX_UNREACHED(D3DXCreateFontA, 12)
D3DX_UNREACHED(D3DXCreateRenderToSurface, 7)
D3DX_UNREACHED(D3DXCreateBuffer, 2)
