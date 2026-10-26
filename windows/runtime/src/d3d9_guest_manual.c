/* Hand-written part of the guest Direct3D 9 layer (notes/d3d9_marshal.md):
 * the host-object map and HWND table the generated thunks use, and the
 * methods gen_d3d9_guest.py excludes because the host hands out memory the
 * guest must write (Lock/Unlock) or a GDI handle (GetDC/ReleaseDC).
 *
 * Lock staging. The guest cannot address host memory, so a lock copies the
 * host-locked bytes into a guest buffer, and Unlock copies them back. The
 * host lock stays open from Lock to Unlock. Cost per lock: one guest
 * allocation (reused for a D3DUSAGE_DYNAMIC resource, keyed by resource,
 * level and face; freed on Unlock otherwise), one copy in (skipped for
 * D3DLOCK_DISCARD) and one copy back (skipped for D3DLOCK_READONLY), each of
 * the locked size. A write-only resource is still copied in: the guest may
 * write only part of the range, and the copy back must not put stale bytes
 * over the rest. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* DXVK Native defines THIS_ and THIS empty in C, which drops the This
 * argument from every C vtable entry; restore the MinGW definitions, as
 * gen_d3d9_guest.c does (HOST_PRELUDE in the generator). */
#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>

#include "d3d9_guest.h"
#include "d3d9_hud.h"
#include "guest_heap.h"
#include "win32_thunk.h"

static pthread_mutex_t g_d3d9g_lock = PTHREAD_MUTEX_INITIALIZER;

void d3d9g_die(const char *what)
{
    fprintf(stderr, "UNSUPPORTED d3d9: %s\n", what);
    abort();
}

/* ---- Host object -> guest object ---------------------------------------- */

#define OBJ_BUCKETS 4096u

typedef struct ObjEntry {
    void *host;
    uint32_t guest;
    struct ObjEntry *next;
} ObjEntry;

static ObjEntry *g_objs[OBJ_BUCKETS];

static uint32_t ptr_hash(const void *p, uint32_t buckets)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return (uint32_t)v & (buckets - 1u);
}

static ObjEntry **obj_slot(void *host)
{
    ObjEntry **pp = &g_objs[ptr_hash(host, OBJ_BUCKETS)];
    while (*pp != NULL && (*pp)->host != host)
        pp = &(*pp)->next;
    return pp;
}

uint32_t d3d9g_find(void *host)
{
    pthread_mutex_lock(&g_d3d9g_lock);
    ObjEntry *e = *obj_slot(host);
    uint32_t g = e != NULL ? e->guest : 0u;
    pthread_mutex_unlock(&g_d3d9g_lock);
    return g;
}

uint32_t d3d9g_wrap(void *host, const com_interface_t *iface)
{
    if (host == NULL)
        return 0;
    pthread_mutex_lock(&g_d3d9g_lock);
    ObjEntry **pp = obj_slot(host);
    uint32_t g;
    if (*pp != NULL) {
        g = (*pp)->guest;
        const com_interface_t *have = com_guest_iface(g);
        if (have != NULL && have->n_methods < iface->n_methods)
            fprintf(stderr, "UNSUPPORTED d3d9: object 0x%08X is a %s, the guest asked for a %s\n", g,
                    have->name, iface->name);
    } else {
        ObjEntry *e = malloc(sizeof *e);
        if (e == NULL)
            d3d9g_die("out of host memory for the object map");
        g = com_guest_new(iface, host);
        *e = (ObjEntry){host, g, NULL};
        *pp = e;
    }
    pthread_mutex_unlock(&g_d3d9g_lock);
    return g;
}

/* ---- Lock staging -------------------------------------------------------- */

#define STAGE_BUCKETS 1024u

typedef struct Stage {
    void *res; /* host resource */
    uint32_t sub; /* level, (face << 16) | level for a cube texture */
    uint32_t guest; /* staging buffer */
    uint32_t cap; /* its size */
    uint32_t size; /* bytes locked now */
    void *bits; /* host-locked memory */
    int locked, readonly, keep;
    struct Stage *next;
} Stage;

static Stage *g_stages[STAGE_BUCKETS];

static Stage **stage_slot(void *res, uint32_t sub)
{
    Stage **pp = &g_stages[ptr_hash(res, STAGE_BUCKETS)];
    while (*pp != NULL && ((*pp)->res != res || (*pp)->sub != sub))
        pp = &(*pp)->next;
    return pp;
}

void d3d9g_forget(void *host)
{
    pthread_mutex_lock(&g_d3d9g_lock);
    ObjEntry **pp = obj_slot(host);
    if (*pp != NULL) {
        ObjEntry *e = *pp;
        *pp = e->next;
        free(e);
    }
    for (Stage **sp = &g_stages[ptr_hash(host, STAGE_BUCKETS)]; *sp != NULL;) {
        Stage *s = *sp;
        if (s->res == host) {
            *sp = s->next;
            guest_free(s->guest);
            free(s);
        } else {
            sp = &s->next;
        }
    }
    pthread_mutex_unlock(&g_d3d9g_lock);
}

/* Stage size bytes of host memory bits for (res, sub). Returns the guest
 * buffer, or 0 when the guest heap is full or (res, sub) is already locked. */
static uint32_t stage_lock(void *res, uint32_t sub, void *bits, uint32_t size, DWORD flags, int keep)
{
    pthread_mutex_lock(&g_d3d9g_lock);
    Stage **pp = stage_slot(res, sub);
    Stage *s = *pp;
    uint32_t g = 0;
    if (s != NULL && s->locked) {
        fprintf(stderr, "UNSUPPORTED d3d9: nested lock of %p sub %u\n", res, sub);
        goto out;
    }
    if (s == NULL) {
        s = calloc(1, sizeof *s);
        if (s == NULL)
            d3d9g_die("out of host memory for lock staging");
        s->res = res;
        s->sub = sub;
        *pp = s;
    }
    if (s->cap < size) {
        if (s->guest != 0u)
            guest_free(s->guest);
        s->guest = guest_malloc(size != 0u ? size : 1u);
        s->cap = s->guest != 0u ? size : 0u;
        if (s->guest == 0u)
            goto out;
    }
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(guest_ptr(s->guest), bits, size);
    s->size = size;
    s->bits = bits;
    s->locked = 1;
    s->readonly = (flags & D3DLOCK_READONLY) != 0;
    s->keep = keep;
    g = s->guest;
out:
    pthread_mutex_unlock(&g_d3d9g_lock);
    return g;
}

/* Copy (res, sub)'s staging back to the host memory it came from. The
 * caller unlocks the host afterwards. */
static void stage_unlock(void *res, uint32_t sub)
{
    pthread_mutex_lock(&g_d3d9g_lock);
    Stage **pp = stage_slot(res, sub);
    Stage *s = *pp;
    if (s != NULL && s->locked) {
        if (!s->readonly)
            memcpy(s->bits, guest_ptr(s->guest), s->size);
        s->locked = 0;
        if (!s->keep) {
            *pp = s->next;
            guest_free(s->guest);
            free(s);
        }
    }
    pthread_mutex_unlock(&g_d3d9g_lock);
}

/* Bytes per pixel, or per 4x4 block for a block format (*block = 1).
 * 0 for a format this table does not know. */
static uint32_t format_bytes(D3DFORMAT f, int *block)
{
    *block = 0;
    switch ((uint32_t)f) {
    case D3DFMT_DXT1:
        *block = 1;
        return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        *block = 1;
        return 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
        return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_A8P8: case D3DFMT_L16:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_R16F: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1: case D3DFMT_INDEX16:
        return 2;
    case D3DFMT_R8G8B8:
        return 3;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_G16R16F:
    case D3DFMT_R32F: case D3DFMT_V16U16: case D3DFMT_Q8W8V8U8: case D3DFMT_X8L8V8U8:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_D32F_LOCKABLE: case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
        return 8;
    case D3DFMT_A32B32G32R32F:
        return 16;
    default:
        return 0;
    }
}

/* The bytes a lock of w x h (x d) texels covers from its first byte, given
 * the host pitches: whole pitches for all but the last row and slice, which
 * end at the region's right edge. full: the whole level, whose rows span
 * whole pitches. 0 for an unknown format. */
static uint32_t locked_bytes(D3DFORMAT f, uint32_t w, uint32_t h, uint32_t d, uint32_t pitch,
                             uint32_t slice, int full)
{
    int block;
    uint32_t bpp = format_bytes(f, &block);
    uint32_t rows = block ? (h + 3u) / 4u : h;
    /* A whole level of a format outside the table is still fine unless it is
     * a FOURCC (block) format, whose row count is unknown. */
    if (full && (bpp != 0u || (uint32_t)f < 0x100u))
        return (d - 1u) * slice + rows * pitch;
    if (bpp == 0u || rows == 0u || w == 0u || d == 0u)
        return 0;
    uint32_t row_bytes = (block ? (w + 3u) / 4u : w) * bpp;
    return (d - 1u) * slice + (rows - 1u) * pitch + row_bytes;
}

static void write_locked_rect(uint32_t a, INT pitch, uint32_t guest)
{
    MEM32(a + D3D9G_GOFF_D3DLOCKED_RECT_Pitch) = (uint32_t)pitch;
    MEM32(a + D3D9G_GOFF_D3DLOCKED_RECT_pBits) = guest;
}

/* Stage a host D3DLOCKED_RECT of a surface or texture level described by
 * desc. Returns D3D_OK, or an error after which the caller unlocks the host. */
static HRESULT stage_rect(void *res, uint32_t sub, const D3DSURFACE_DESC *desc, const D3DLOCKED_RECT *lr,
                          uint32_t a_rect, DWORD flags, uint32_t a_out, const char *who)
{
    uint32_t w = desc->Width, h = desc->Height;
    if (a_rect != 0u) {
        const RECT *r = guest_ptr(a_rect);
        w = (uint32_t)(r->right - r->left);
        h = (uint32_t)(r->bottom - r->top);
    }
    uint32_t size = locked_bytes(desc->Format, w, h, 1, (uint32_t)lr->Pitch, 0, a_rect == 0u);
    if (size == 0u) {
        fprintf(stderr, "UNSUPPORTED d3d9: %s: format %u\n", who, (unsigned)desc->Format);
        return D3DERR_INVALIDCALL;
    }
    uint32_t g = stage_lock(res, sub, lr->pBits, size, flags, (desc->Usage & D3DUSAGE_DYNAMIC) != 0);
    if (g == 0u)
        return D3DERR_INVALIDCALL;
    write_locked_rect(a_out, lr->Pitch, g);
    return D3D_OK;
}

static HRESULT stage_box(void *res, uint32_t sub, const D3DVOLUME_DESC *desc, const D3DLOCKED_BOX *lb,
                         uint32_t a_box, DWORD flags, uint32_t a_out, const char *who)
{
    uint32_t w = desc->Width, h = desc->Height, d = desc->Depth;
    if (a_box != 0u) {
        const D3DBOX *b = guest_ptr(a_box);
        w = b->Right - b->Left;
        h = b->Bottom - b->Top;
        d = b->Back - b->Front;
    }
    uint32_t size = locked_bytes(desc->Format, w, h, d, (uint32_t)lb->RowPitch, (uint32_t)lb->SlicePitch,
                                 a_box == 0u);
    if (size == 0u) {
        fprintf(stderr, "UNSUPPORTED d3d9: %s: format %u\n", who, (unsigned)desc->Format);
        return D3DERR_INVALIDCALL;
    }
    uint32_t g = stage_lock(res, sub, lb->pBits, size, flags, (desc->Usage & D3DUSAGE_DYNAMIC) != 0);
    if (g == 0u)
        return D3DERR_INVALIDCALL;
    MEM32(a_out + D3D9G_GOFF_D3DLOCKED_BOX_RowPitch) = (uint32_t)lb->RowPitch;
    MEM32(a_out + D3D9G_GOFF_D3DLOCKED_BOX_SlicePitch) = (uint32_t)lb->SlicePitch;
    MEM32(a_out + D3D9G_GOFF_D3DLOCKED_BOX_pBits) = g;
    return D3D_OK;
}

/* ---- IDirect3DSurface9 --------------------------------------------------- */

/* LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) */
void d3d9g_IDirect3DSurface9_LockRect(void)
{
    IDirect3DSurface9 *self = com_guest_self(W32_ARG(0));
    uint32_t a_out = W32_ARG(1), a_rect = W32_ARG(2);
    DWORD flags = W32_ARG(3);
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetDesc(self, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->LockRect(self, &lr, d3d9g_gp(a_rect), flags);
    if (SUCCEEDED(hr)) {
        hr = stage_rect(self, 0, &desc, &lr, a_rect, flags, a_out, "IDirect3DSurface9::LockRect");
        if (FAILED(hr))
            self->lpVtbl->UnlockRect(self);
    }
    W32_RETURN(4, hr);
}

void d3d9g_IDirect3DSurface9_UnlockRect(void)
{
    IDirect3DSurface9 *self = com_guest_self(W32_ARG(0));
    stage_unlock(self, 0);
    W32_RETURN(1, self->lpVtbl->UnlockRect(self));
}

/* GetDC(HDC *phdc) */
void d3d9g_IDirect3DSurface9_GetDC(void)
{
    static int once;
    if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "UNSUPPORTED d3d9: IDirect3DSurface9::GetDC: no GDI\n");
    W32_RETURN(2, D3DERR_INVALIDCALL);
}

/* ReleaseDC(HDC hdc) */
void d3d9g_IDirect3DSurface9_ReleaseDC(void)
{
    static int once;
    if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "UNSUPPORTED d3d9: IDirect3DSurface9::ReleaseDC: no GDI\n");
    W32_RETURN(2, D3DERR_INVALIDCALL);
}

/* ---- IDirect3DTexture9 --------------------------------------------------- */

/* LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) */
void d3d9g_IDirect3DTexture9_LockRect(void)
{
    IDirect3DTexture9 *self = com_guest_self(W32_ARG(0));
    UINT level = W32_ARG(1);
    uint32_t a_out = W32_ARG(2), a_rect = W32_ARG(3);
    DWORD flags = W32_ARG(4);
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetLevelDesc(self, level, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->LockRect(self, level, &lr, d3d9g_gp(a_rect), flags);
    if (SUCCEEDED(hr)) {
        hr = stage_rect(self, level, &desc, &lr, a_rect, flags, a_out, "IDirect3DTexture9::LockRect");
        if (FAILED(hr))
            self->lpVtbl->UnlockRect(self, level);
    }
    W32_RETURN(5, hr);
}

/* UnlockRect(UINT Level) */
void d3d9g_IDirect3DTexture9_UnlockRect(void)
{
    IDirect3DTexture9 *self = com_guest_self(W32_ARG(0));
    UINT level = W32_ARG(1);
    stage_unlock(self, level);
    W32_RETURN(2, self->lpVtbl->UnlockRect(self, level));
}

/* ---- IDirect3DCubeTexture9 ----------------------------------------------- */

/* LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect,
 *          const RECT *pRect, DWORD Flags) */
void d3d9g_IDirect3DCubeTexture9_LockRect(void)
{
    IDirect3DCubeTexture9 *self = com_guest_self(W32_ARG(0));
    D3DCUBEMAP_FACES face = (D3DCUBEMAP_FACES)W32_ARG(1);
    UINT level = W32_ARG(2);
    uint32_t a_out = W32_ARG(3), a_rect = W32_ARG(4);
    DWORD flags = W32_ARG(5);
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    uint32_t sub = ((uint32_t)face << 16) | level;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetLevelDesc(self, level, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->LockRect(self, face, level, &lr, d3d9g_gp(a_rect), flags);
    if (SUCCEEDED(hr)) {
        hr = stage_rect(self, sub, &desc, &lr, a_rect, flags, a_out, "IDirect3DCubeTexture9::LockRect");
        if (FAILED(hr))
            self->lpVtbl->UnlockRect(self, face, level);
    }
    W32_RETURN(6, hr);
}

/* UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) */
void d3d9g_IDirect3DCubeTexture9_UnlockRect(void)
{
    IDirect3DCubeTexture9 *self = com_guest_self(W32_ARG(0));
    D3DCUBEMAP_FACES face = (D3DCUBEMAP_FACES)W32_ARG(1);
    UINT level = W32_ARG(2);
    stage_unlock(self, ((uint32_t)face << 16) | level);
    W32_RETURN(3, self->lpVtbl->UnlockRect(self, face, level));
}

/* ---- IDirect3DVolume9 ---------------------------------------------------- */

/* LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) */
void d3d9g_IDirect3DVolume9_LockBox(void)
{
    IDirect3DVolume9 *self = com_guest_self(W32_ARG(0));
    uint32_t a_out = W32_ARG(1), a_box = W32_ARG(2);
    DWORD flags = W32_ARG(3);
    D3DVOLUME_DESC desc;
    D3DLOCKED_BOX lb;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetDesc(self, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->LockBox(self, &lb, d3d9g_gp(a_box), flags);
    if (SUCCEEDED(hr)) {
        hr = stage_box(self, 0, &desc, &lb, a_box, flags, a_out, "IDirect3DVolume9::LockBox");
        if (FAILED(hr))
            self->lpVtbl->UnlockBox(self);
    }
    W32_RETURN(4, hr);
}

void d3d9g_IDirect3DVolume9_UnlockBox(void)
{
    IDirect3DVolume9 *self = com_guest_self(W32_ARG(0));
    stage_unlock(self, 0);
    W32_RETURN(1, self->lpVtbl->UnlockBox(self));
}

/* ---- IDirect3DVolumeTexture9 --------------------------------------------- */

/* LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) */
void d3d9g_IDirect3DVolumeTexture9_LockBox(void)
{
    IDirect3DVolumeTexture9 *self = com_guest_self(W32_ARG(0));
    UINT level = W32_ARG(1);
    uint32_t a_out = W32_ARG(2), a_box = W32_ARG(3);
    DWORD flags = W32_ARG(4);
    D3DVOLUME_DESC desc;
    D3DLOCKED_BOX lb;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetLevelDesc(self, level, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->LockBox(self, level, &lb, d3d9g_gp(a_box), flags);
    if (SUCCEEDED(hr)) {
        hr = stage_box(self, level, &desc, &lb, a_box, flags, a_out, "IDirect3DVolumeTexture9::LockBox");
        if (FAILED(hr))
            self->lpVtbl->UnlockBox(self, level);
    }
    W32_RETURN(5, hr);
}

/* UnlockBox(UINT Level) */
void d3d9g_IDirect3DVolumeTexture9_UnlockBox(void)
{
    IDirect3DVolumeTexture9 *self = com_guest_self(W32_ARG(0));
    UINT level = W32_ARG(1);
    stage_unlock(self, level);
    W32_RETURN(2, self->lpVtbl->UnlockBox(self, level));
}

/* ---- Vertex and index buffers -------------------------------------------- */

/* Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags); a
 * size of 0 locks from the offset to the end. */
static uint32_t buffer_span(UINT total, UINT offset, UINT size)
{
    if (size != 0u)
        return size;
    return offset < total ? total - offset : 0u;
}

void d3d9g_IDirect3DVertexBuffer9_Lock(void)
{
    IDirect3DVertexBuffer9 *self = com_guest_self(W32_ARG(0));
    UINT offset = W32_ARG(1), size = W32_ARG(2);
    uint32_t a_out = W32_ARG(3);
    DWORD flags = W32_ARG(4);
    D3DVERTEXBUFFER_DESC desc;
    void *bits = NULL;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetDesc(self, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->Lock(self, offset, size, &bits, flags);
    if (SUCCEEDED(hr)) {
        uint32_t g = stage_lock(self, 0, bits, buffer_span(desc.Size, offset, size), flags,
                                (desc.Usage & D3DUSAGE_DYNAMIC) != 0);
        if (g != 0u) {
            MEM32(a_out) = g;
        } else {
            self->lpVtbl->Unlock(self);
            hr = D3DERR_INVALIDCALL;
        }
    }
    W32_RETURN(5, hr);
}

void d3d9g_IDirect3DVertexBuffer9_Unlock(void)
{
    IDirect3DVertexBuffer9 *self = com_guest_self(W32_ARG(0));
    stage_unlock(self, 0);
    W32_RETURN(1, self->lpVtbl->Unlock(self));
}

void d3d9g_IDirect3DIndexBuffer9_Lock(void)
{
    IDirect3DIndexBuffer9 *self = com_guest_self(W32_ARG(0));
    UINT offset = W32_ARG(1), size = W32_ARG(2);
    uint32_t a_out = W32_ARG(3);
    DWORD flags = W32_ARG(4);
    D3DINDEXBUFFER_DESC desc;
    void *bits = NULL;
    HRESULT hr = a_out != 0u ? self->lpVtbl->GetDesc(self, &desc) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = self->lpVtbl->Lock(self, offset, size, &bits, flags);
    if (SUCCEEDED(hr)) {
        uint32_t g = stage_lock(self, 0, bits, buffer_span(desc.Size, offset, size), flags,
                                (desc.Usage & D3DUSAGE_DYNAMIC) != 0);
        if (g != 0u) {
            MEM32(a_out) = g;
        } else {
            self->lpVtbl->Unlock(self);
            hr = D3DERR_INVALIDCALL;
        }
    }
    W32_RETURN(5, hr);
}

void d3d9g_IDirect3DIndexBuffer9_Unlock(void)
{
    IDirect3DIndexBuffer9 *self = com_guest_self(W32_ARG(0));
    stage_unlock(self, 0);
    W32_RETURN(1, self->lpVtbl->Unlock(self));
}

/* ---- HWND ---------------------------------------------------------------- */

#define HWND_MAX 64u

static struct {
    uint32_t guest;
    void *host;
} g_hwnds[HWND_MAX];
static uint32_t g_n_hwnds;

void *d3d9g_hwnd_in(uint32_t guest_hwnd)
{
    if (guest_hwnd == 0u)
        return NULL;
    void *host = d3d9g_hwnd(guest_hwnd);
    pthread_mutex_lock(&g_d3d9g_lock);
    uint32_t i = 0;
    while (i < g_n_hwnds && g_hwnds[i].guest != guest_hwnd)
        i++;
    if (i == g_n_hwnds && g_n_hwnds < HWND_MAX)
        g_n_hwnds++;
    if (i < HWND_MAX) {
        g_hwnds[i].guest = guest_hwnd;
        g_hwnds[i].host = host;
    }
    pthread_mutex_unlock(&g_d3d9g_lock);
    return host;
}

uint32_t d3d9g_hwnd_out(void *host_hwnd)
{
    if (host_hwnd == NULL)
        return 0;
    uint32_t g = 0;
    pthread_mutex_lock(&g_d3d9g_lock);
    for (uint32_t i = 0; i < g_n_hwnds && g == 0u; i++)
        if (g_hwnds[i].host == host_hwnd)
            g = g_hwnds[i].guest;
    pthread_mutex_unlock(&g_d3d9g_lock);
    if (g == 0u)
        fprintf(stderr, "UNSUPPORTED d3d9: host window %p was never a guest HWND\n", host_hwnd);
    return g;
}

/* ---- x87 control word ---------------------------------------------------- */

/* As Windows D3D9 and DXVK's SetupFPU (d3d9_device.cpp) do: single
 * precision, round to nearest, every exception masked; other bits kept. */
void d3d9g_after_create_device(uint32_t hr, uint32_t behavior_flags)
{
    if (SUCCEEDED((HRESULT)hr) && (behavior_flags & D3DCREATE_FPU_PRESERVE) == 0u)
        g_fp_control_word = (uint16_t)((g_fp_control_word & 0xF0C0u) | 0x003Fu);
}

/* ---- Presentation -------------------------------------------------------- */

/* DXVK enters exclusive fullscreen with CGDisplaySwitchToMode, which changes
 * the real display mode, so by default the host gets a borderless window
 * over the desktop (HPRECOMP_WINDOWED=1: a window the size of the back
 * buffer; HPRECOMP_FULLSCREEN=1: the game's exclusive mode). A Reset or extra
 * swap chain without hDeviceWindow uses the window CreateDevice saw. */
static void *g_device_window;

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e != NULL && strcmp(e, "1") == 0;
}

enum { PM_DESKTOP, PM_WINDOWED, PM_FULLSCREEN };

static int present_mode(void)
{
    if (env_on("HPRECOMP_FULLSCREEN"))
        return PM_FULLSCREEN;
    return env_on("HPRECOMP_WINDOWED") ? PM_WINDOWED : PM_DESKTOP;
}

void d3d9g_windowed_in(void *host_pp, d3d9g_mode_keep *keep, void *focus_window)
{
    D3DPRESENT_PARAMETERS *pp = host_pp;
    const char *tr = getenv("HP_TRACE_D3D9");
    if (pp != NULL && tr != NULL && tr[0] != '\0' && strcmp(tr, "0") != 0)
        fprintf(stderr, "D3D9 present parameters %ux%u Windowed=%d window=%p focus=%p\n", pp->BackBufferWidth,
                pp->BackBufferHeight, (int)pp->Windowed, (void *)pp->hDeviceWindow, focus_window);
    keep->vsync = pp != NULL;
    if (keep->vsync) {
        keep->interval = pp->PresentationInterval;
        pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    }
    /* Windows shows the device window of a fullscreen device (Wine:
     * wined3d_device_setup_fullscreen_window, SetWindowPos with
     * SWP_SHOWWINDOW). LEGO Indiana Jones creates its window hidden, calls
     * ShowWindow(SW_HIDE) and relies on this. */
    if (pp != NULL && !pp->Windowed)
        d3d9g_backend_show_window(pp->hDeviceWindow != NULL ? pp->hDeviceWindow
                                  : focus_window != NULL    ? focus_window
                                                            : g_device_window);
    int mode = present_mode();
    keep->forced = pp != NULL && mode != PM_FULLSCREEN;
    if (!keep->forced)
        return;
    keep->windowed = (uint32_t)pp->Windowed;
    keep->refresh_hz = pp->FullScreen_RefreshRateInHz;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    void *win = pp->hDeviceWindow != NULL ? pp->hDeviceWindow : focus_window;
    if (win != NULL)
        g_device_window = win;
    /* A window already made desktop-sized keeps the state Ctrl+Cmd+F gave
     * it across a Reset. */
    static void *desktop_window;
    if (mode == PM_DESKTOP && g_device_window == desktop_window)
        return;
    desktop_window = mode == PM_DESKTOP ? g_device_window : NULL;
    d3d9g_backend_fit_window(g_device_window, pp->BackBufferWidth, pp->BackBufferHeight,
                             mode == PM_DESKTOP);
}

void d3d9g_windowed_out(uint32_t guest_pp, const d3d9g_mode_keep *keep)
{
    if (keep->vsync)
        MEM32(guest_pp + D3D9G_GOFF_D3DPRESENT_PARAMETERS_PresentationInterval) = keep->interval;
    if (!keep->forced)
        return;
    MEM32(guest_pp + D3D9G_GOFF_D3DPRESENT_PARAMETERS_Windowed) = keep->windowed;
    MEM32(guest_pp + D3D9G_GOFF_D3DPRESENT_PARAMETERS_FullScreen_RefreshRateInHz) = keep->refresh_hz;
}

/* In the desktop default the adapter lists the sizes the LEGO Hobbit port
 * offers, all below a notch: the desktop's scaled size (points), the full
 * panel, 75% and 50% of it, without duplicates (at Default scaling 50% of
 * the panel is the desktop size). They are listed at 60 Hz only,
 * because the game's mode filter drops modes 1600 or more wide above
 * 85 Hz; the host still vsyncs at the display's rate, which the current
 * mode reports. The game takes the listed mode nearest its configured
 * screen size. With no configuration that is 1024x768, which would pick the
 * smallest listed size. So when the game repo names the two guest words that
 * hold the configured width and height (GAME_SCREEN_WIDTH_VA and
 * GAME_SCREEN_HEIGHT_VA in GAME_CFLAGS), GetAdapterModeCount, which the game
 * calls before it reads them, replaces a size that is not listed (first run,
 * an older configuration, another display) with the desktop size; a size
 * chosen in the game's menu stays. Without them the configured size is left
 * as it is. */
#define NATIVE_MODES 4u

/* The listed sizes, the desktop first; returns how many. */
static uint32_t mode_list(uint32_t wh[NATIVE_MODES][2], uint32_t *hz)
{
    static const double panel_part[NATIVE_MODES - 1u] = {1.0, 0.75, 0.5};
    uint32_t pw, ph, n = 1;
    d3d9g_backend_screen_sizes(&wh[0][0], &wh[0][1], &pw, &ph, hz);
    for (uint32_t i = 0; i < NATIVE_MODES - 1u; i++) {
        uint32_t w = (uint32_t)((double)pw * panel_part[i] + 0.5), h = (uint32_t)((double)ph * panel_part[i] + 0.5);
        uint32_t k = 0;
        while (k < n && (wh[k][0] != w || wh[k][1] != h))
            k++;
        if (k == n)
            wh[n][0] = w, wh[n][1] = h, n++;
    }
    return n;
}

static void mode_write(uint32_t guest_mode, uint32_t w, uint32_t h, uint32_t hz, uint32_t format)
{
    MEM32(guest_mode + 0) = w;
    MEM32(guest_mode + 4) = h;
    MEM32(guest_mode + 8) = hz;
    MEM32(guest_mode + 12) = format;
}

uint32_t d3d9g_mode_count(uint32_t host_count)
{
    if (present_mode() != PM_DESKTOP || host_count == 0u)
        return host_count;
    uint32_t wh[NATIVE_MODES][2], hz, n = mode_list(wh, &hz);
#if defined(GAME_SCREEN_WIDTH_VA) && defined(GAME_SCREEN_HEIGHT_VA)
    for (uint32_t i = 0; i < n; i++)
        if (MEM32(GAME_SCREEN_WIDTH_VA) == wh[i][0] && MEM32(GAME_SCREEN_HEIGHT_VA) == wh[i][1])
            return n;
    MEM32(GAME_SCREEN_WIDTH_VA) = wh[0][0];
    MEM32(GAME_SCREEN_HEIGHT_VA) = wh[0][1];
#endif
    return n;
}

uint32_t d3d9g_enum_mode(uint32_t hr, uint32_t format, uint32_t index, uint32_t guest_mode)
{
    uint32_t wh[NATIVE_MODES][2], hz;
    if (present_mode() != PM_DESKTOP)
        return hr;
    if (index >= mode_list(wh, &hz) || guest_mode == 0u)
        return (uint32_t)D3DERR_INVALIDCALL;
    mode_write(guest_mode, wh[index][0], wh[index][1], 60u, format);
    return (uint32_t)D3D_OK;
}

void d3d9g_display_mode(uint32_t hr, uint32_t guest_mode)
{
    uint32_t wh[NATIVE_MODES][2], hz;
    if (!SUCCEEDED((HRESULT)hr) || present_mode() != PM_DESKTOP || guest_mode == 0u)
        return;
    mode_list(wh, &hz);
    mode_write(guest_mode, wh[0][0], wh[0][1], hz, MEM32(guest_mode + 12));
}

/* ---- HPRECOMP_DUMP_FRAMES ----------------------------------------------- */

#define DUMP_MAX 1024
static pthread_once_t g_dump_once = PTHREAD_ONCE_INIT;
static unsigned g_dump_at[DUMP_MAX];
static int g_n_dump;
static atomic_uint g_presents;

static void dump_parse(void)
{
    const char *e = getenv("HPRECOMP_DUMP_FRAMES");
    while (e != NULL && *e != '\0' && g_n_dump < DUMP_MAX) {
        char *end;
        unsigned long n = strtoul(e, &end, 10);
        if (end == e)
            break;
        g_dump_at[g_n_dump++] = (unsigned)n;
        e = *end == ',' ? end + 1 : end;
    }
}

/* Back buffer -> system-memory copy -> PPM. 32-bit X8R8G8B8 and A8R8G8B8
 * only (the game's formats); others are reported, not written. */
static void dump_write(IDirect3DDevice9 *dev, unsigned n)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr = dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr))
        hr = bb->lpVtbl->GetDesc(bb, &d);
    if (SUCCEEDED(hr) && d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8)
        hr = E_NOTIMPL;
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->GetRenderTargetData(dev, bb, sys);
    if (SUCCEEDED(hr))
        hr = sys->lpVtbl->LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        const char *dir = getenv("HPRECOMP_DUMP_DIR");
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%u.ppm", dir != NULL ? dir : ".", n);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
                for (UINT x = 0; x < d.Width; x++) {
                    uint8_t rgb[3] = {row[4 * x + 2], row[4 * x + 1], row[4 * x]};
                    fwrite(rgb, 1, 3, f);
                }
            }
            fclose(f);
            fprintf(stderr, "D3D9 dump Present #%u %ux%u -> %s\n", n, d.Width, d.Height, path);
        } else {
            fprintf(stderr, "D3D9 dump Present #%u: cannot write %s\n", n, path);
        }
        sys->lpVtbl->UnlockRect(sys);
    } else {
        fprintf(stderr, "D3D9 dump Present #%u failed hr=%08X\n", n, (uint32_t)hr);
    }
    if (sys != NULL)
        sys->lpVtbl->Release(sys);
    if (bb != NULL)
        bb->lpVtbl->Release(bb);
}

void d3d9g_dump_frame(void *host_device)
{
    pthread_once(&g_dump_once, dump_parse);
    if (g_n_dump == 0)
        return;
    unsigned n = atomic_fetch_add(&g_presents, 1u) + 1u;
    for (int i = 0; i < g_n_dump; i++)
        if (g_dump_at[i] == n) {
            dump_write(host_device, n);
            return;
        }
}

/* ---- HP_TRACE_D3D9 ------------------------------------------------------- */

/* The summary prints from whichever call first sees the second elapse, so
 * a thread that makes no D3D9 calls prints none. */
static atomic_int g_tr_on = -1;
static atomic_uint g_tr_count[D3D9G_TR_COUNT];
static unsigned g_tr_last[D3D9G_TR_COUNT];
static atomic_uint g_tr_tcl = 0xFFFFFFFFu;
static atomic_llong g_tr_next_ns;
static int64_t g_tr_t0_ns;

static const char *const k_tr_name[D3D9G_TR_COUNT] = {
    "Present", "EndScene", "Draw", "SetRenderTarget", "Reset", "TestCooperativeLevel",
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void d3d9g_trace(enum d3d9g_trace_event ev, uint32_t hr)
{
    d3d9_hud_event((int)ev); /* the overlay counts every event, trace on or off */
    int on = atomic_load(&g_tr_on);
    if (on < 0) {
        const char *e = getenv("HP_TRACE_D3D9");
        on = e != NULL && e[0] != '\0' && strcmp(e, "0") != 0;
        atomic_store(&g_tr_on, on);
    }
    if (!on)
        return;
    unsigned n = atomic_fetch_add(&g_tr_count[ev], 1u) + 1u;
    if (ev == D3D9G_TR_PRESENT || ev == D3D9G_TR_END_SCENE || ev == D3D9G_TR_RESET)
        fprintf(stderr, "D3D9 %s #%u hr=%08X\n", k_tr_name[ev], n, hr);
    else if (ev == D3D9G_TR_TEST_COOPERATIVE_LEVEL && atomic_exchange(&g_tr_tcl, hr) != hr)
        fprintf(stderr, "D3D9 TestCooperativeLevel #%u hr=%08X\n", n, hr);

    int64_t now = now_ns();
    long long next = atomic_load(&g_tr_next_ns);
    if (next == 0) {
        if (atomic_compare_exchange_strong(&g_tr_next_ns, &next, now + 1000000000))
            g_tr_t0_ns = now;
        return;
    }
    if (now < next || !atomic_compare_exchange_strong(&g_tr_next_ns, &next, now + 1000000000))
        return;
    unsigned d[D3D9G_TR_COUNT];
    for (int i = 0; i < D3D9G_TR_COUNT; i++) {
        unsigned c = atomic_load(&g_tr_count[i]);
        d[i] = c - g_tr_last[i];
        g_tr_last[i] = c;
    }
    fprintf(stderr, "D3D9 SEC t=%.1f frames=%u draws=%u setrt=%u endscene=%u total_frames=%u\n",
            (double)(now - g_tr_t0_ns) / 1e9, d[D3D9G_TR_PRESENT], d[D3D9G_TR_DRAW],
            d[D3D9G_TR_SET_RENDER_TARGET], d[D3D9G_TR_END_SCENE], g_tr_last[D3D9G_TR_PRESENT]);
}
