/* Direct3D 9 guest layer test: the generated thunks (gen_d3d9_guest.c) and
 * the hand-written ones (d3d9_guest_manual.c) against a mock host backend,
 * called the way the guest calls them: `this` and the arguments pushed on the
 * guest stack, the thunk found through the guest object's vtable slot.
 * Doubles here for com_guest.c (not linked), the UI slice's d3d9g_hwnd and the
 * backend's d3d9g_backend_create9. Links the engine's guest_mem.c,
 * guest_heap.c, dlmalloc.c, host_funcs.c and recomp_glue.c. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>

#include "quiet_abort.h"
#include "recomp_types.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "com_guest.h"
#include "d3d9_guest.h"
#include "win32_thunk.h"

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

/* ---- com_guest.c double ------------------------------------------------ */

#define OBJ_MAX 64u
static struct {
    uint32_t guest;
    void *host;
    const com_interface_t *iface;
} g_obj[OBJ_MAX];
static struct {
    const com_interface_t *iface;
    uint32_t vtbl;
} g_vt[32];
static int g_frees;

uint32_t com_guest_new(const com_interface_t *iface, void *host_self)
{
    uint32_t vt = 0;
    for (unsigned i = 0; i < 32 && vt == 0u; i++) {
        if (g_vt[i].iface == iface)
            vt = g_vt[i].vtbl;
        else if (g_vt[i].iface == NULL) {
            vt = guest_malloc(4u * iface->n_methods);
            for (uint32_t m = 0; m < iface->n_methods; m++)
                MEM32(vt + 4u * m) = host_func_va(iface->methods[m]);
            g_vt[i].iface = iface;
            g_vt[i].vtbl = vt;
        }
    }
    for (uint32_t i = 1; i < OBJ_MAX; i++) {
        if (g_obj[i].guest == 0u) {
            uint32_t g = guest_malloc(8);
            MEM32(g) = vt;
            MEM32(g + 4u) = i;
            g_obj[i].guest = g;
            g_obj[i].host = host_self;
            g_obj[i].iface = iface;
            return g;
        }
    }
    abort();
}

static uint32_t obj_index(uint32_t g)
{
    uint32_t i = MEM32(g + 4u);
    return i < OBJ_MAX && g_obj[i].guest == g ? i : 0u;
}

void *com_guest_self(uint32_t guest_this)
{
    uint32_t i = obj_index(guest_this);
    if (i == 0u) {
        fprintf(stderr, "UNSUPPORTED com: bad object 0x%08X\n", guest_this);
        abort();
    }
    return g_obj[i].host;
}

const com_interface_t *com_guest_iface(uint32_t guest_this)
{
    uint32_t i = obj_index(guest_this);
    return i != 0u ? g_obj[i].iface : NULL;
}

void com_guest_free(uint32_t guest_this)
{
    uint32_t i = obj_index(guest_this);
    if (i == 0u)
        abort();
    g_obj[i].guest = 0;
    MEM32(guest_this + 4u) = 0xFFFFFFFFu;
    guest_free(guest_this);
    g_frees++;
}

/* Copies of com_guest.c's GUID helpers: the D3DX buffers (d3dx_shader.c) use them
 * in QueryInterface. */
com_guid_t com_guid_read(uint32_t guest_addr)
{
    com_guid_t g;
    g.d1 = MEM32(guest_addr);
    g.d2 = MEM16(guest_addr + 4u);
    g.d3 = MEM16(guest_addr + 6u);
    for (uint32_t i = 0; i < 8; i++)
        g.d4[i] = MEM8(guest_addr + 8u + i);
    return g;
}

int com_guid_eq(const com_guid_t *a, const com_guid_t *b)
{
    return a->d1 == b->d1 && a->d2 == b->d2 && a->d3 == b->d3 &&
           memcmp(a->d4, b->d4, sizeof a->d4) == 0;
}

static int obj_live(uint32_t g)
{
    for (uint32_t i = 1; i < OBJ_MAX; i++)
        if (g_obj[i].guest == g)
            return 1;
    return 0;
}

/* ---- UI and backend doubles -------------------------------------------- */

static void *host_window(uint32_t g)
{
    return (void *)(uintptr_t)(0xABC000000000ull | g);
}

void *d3d9g_hwnd(uint32_t guest_hwnd)
{
    return host_window(guest_hwnd);
}

/* The last window fit the windowed-presentation hook asked for. */
static struct {
    int calls;
    void *win;
    uint32_t w, h;
    int desktop;
} g_fit;

/* Windows that d3d9g_backend_show_window was asked to show. */
static struct {
    int calls;
    void *win;
} g_show;

void d3d9g_backend_show_window(void *host_window)
{
    g_show.calls++;
    g_show.win = host_window;
}

void d3d9g_backend_fit_window(void *host_window, uint32_t width, uint32_t height, int desktop)
{
    g_fit.calls++;
    g_fit.win = host_window;
    g_fit.w = width;
    g_fit.h = height;
    g_fit.desktop = desktop;
}

static int g_default_scaling;
void d3d9g_backend_screen_sizes(uint32_t *desk_w, uint32_t *desk_h, uint32_t *panel_w, uint32_t *panel_h,
                                uint32_t *refresh_hz)
{
    *desk_w = 2056u, *desk_h = 1290u, *panel_w = 3456u, *panel_h = 2168u, *refresh_hz = 120u;
    if (g_default_scaling) /* the desktop is 50% of the panel */
        *desk_w = 1728u, *desk_h = 1084u;
}

/* ---- Mock backend ------------------------------------------------------- */

/* What the last mock call received. */
static struct {
    const char *fn;
    uint64_t a[8];
    float f;
    const void *p[4];
} R;

static void rec(const char *fn)
{
    memset(&R, 0, sizeof R);
    R.fn = fn;
}

typedef struct {
    IDirect3DSurface9 iface;
    ULONG refs;
} MockSurface;
typedef struct {
    IDirect3DTexture9 iface;
    ULONG refs;
    uint8_t mem[64 * 8]; /* level 0: 16 x 8 A8R8G8B8, pitch 64 */
    int locked;
} MockTexture;
typedef struct {
    IDirect3DDevice9 iface;
    ULONG refs;
} MockDevice;
typedef struct {
    IDirect3D9 iface;
    ULONG refs;
} MockD3D;

static MockSurface g_backbuffer;
static MockTexture g_texture;
static MockDevice g_device;
static MockD3D g_d3d;
static uint32_t g_backend_sdk;

static ULONG STDMETHODCALLTYPE surf_AddRef(IDirect3DSurface9 *This)
{
    return ++((MockSurface *)This)->refs;
}
static ULONG STDMETHODCALLTYPE surf_Release(IDirect3DSurface9 *This)
{
    return --((MockSurface *)This)->refs;
}
static IDirect3DSurface9Vtbl g_surf_vtbl = {.AddRef = surf_AddRef, .Release = surf_Release};

static ULONG STDMETHODCALLTYPE tex_Release(IDirect3DTexture9 *This)
{
    return --((MockTexture *)This)->refs;
}
static HRESULT STDMETHODCALLTYPE tex_GetLevelDesc(IDirect3DTexture9 *This, UINT level, D3DSURFACE_DESC *d)
{
    (void)This;
    if (level != 0u)
        return D3DERR_INVALIDCALL;
    memset(d, 0, sizeof *d);
    d->Format = D3DFMT_A8R8G8B8;
    d->Type = D3DRTYPE_SURFACE;
    d->Pool = D3DPOOL_MANAGED;
    d->Width = 16;
    d->Height = 8;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE tex_LockRect(IDirect3DTexture9 *This, UINT level, D3DLOCKED_RECT *lr,
                                              const RECT *r, DWORD flags)
{
    MockTexture *t = (MockTexture *)This;
    rec("LockRect");
    R.a[0] = level;
    R.a[1] = flags;
    R.p[0] = r;
    lr->Pitch = 64;
    lr->pBits = t->mem + (r != NULL ? (size_t)r->top * 64u + (size_t)r->left * 4u : 0u);
    t->locked = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE tex_UnlockRect(IDirect3DTexture9 *This, UINT level)
{
    rec("UnlockRect");
    R.a[0] = level;
    ((MockTexture *)This)->locked = 0;
    return D3D_OK;
}
static IDirect3DTexture9Vtbl g_tex_vtbl = {.Release = tex_Release, .GetLevelDesc = tex_GetLevelDesc,
                                           .LockRect = tex_LockRect, .UnlockRect = tex_UnlockRect};

static ULONG STDMETHODCALLTYPE dev_Release(IDirect3DDevice9 *This)
{
    rec("Release");
    return --((MockDevice *)This)->refs;
}
static HRESULT STDMETHODCALLTYPE dev_SetRenderState(IDirect3DDevice9 *This, D3DRENDERSTATETYPE s, DWORD v)
{
    (void)This;
    rec("SetRenderState");
    R.a[0] = s;
    R.a[1] = v;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetTexture(IDirect3DDevice9 *This, DWORD stage, IDirect3DBaseTexture9 *t)
{
    (void)This;
    rec("SetTexture");
    R.a[0] = stage;
    R.p[0] = t;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantF(IDirect3DDevice9 *This, UINT reg, const float *f,
                                                              UINT n)
{
    (void)This;
    rec("SetVertexShaderConstantF");
    R.a[0] = reg;
    R.p[0] = f;
    R.a[1] = n;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_DrawIndexedPrimitive(IDirect3DDevice9 *This, D3DPRIMITIVETYPE t, INT base,
                                                          UINT minv, UINT nv, UINT start, UINT prims)
{
    (void)This;
    rec("DrawIndexedPrimitive");
    R.a[0] = t;
    R.a[1] = (uint64_t)(int64_t)base;
    R.a[2] = minv;
    R.a[3] = nv;
    R.a[4] = start;
    R.a[5] = prims;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_Clear(IDirect3DDevice9 *This, DWORD count, const D3DRECT *rects, DWORD flags,
                                           D3DCOLOR color, float z, DWORD stencil)
{
    (void)This;
    rec("Clear");
    R.a[0] = count;
    R.p[0] = rects;
    R.a[1] = flags;
    R.a[2] = color;
    R.f = z;
    R.a[3] = stencil;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateTexture(IDirect3DDevice9 *This, UINT w, UINT h, UINT levels,
                                                   DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DTexture9 **out, HANDLE *shared)
{
    (void)This;
    rec("CreateTexture");
    R.a[0] = w;
    R.a[1] = h;
    R.a[2] = levels;
    R.a[3] = usage;
    R.a[4] = fmt;
    R.a[5] = pool;
    R.p[0] = shared;
    g_texture.refs = 1;
    *out = &g_texture.iface;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetBackBuffer(IDirect3DDevice9 *This, UINT sc, UINT bb, D3DBACKBUFFER_TYPE t,
                                                   IDirect3DSurface9 **out)
{
    (void)This;
    rec("GetBackBuffer");
    R.a[0] = sc;
    R.a[1] = bb;
    R.a[2] = t;
    g_backbuffer.refs++;
    *out = &g_backbuffer.iface;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_Present(IDirect3DDevice9 *This, const RECT *src, const RECT *dst, HWND wnd,
                                             const RGNDATA *dirty)
{
    (void)This;
    rec("Present");
    R.p[0] = src;
    R.p[1] = dst;
    R.p[2] = wnd;
    R.p[3] = dirty;
    return D3D_OK;
}
static D3DPRESENT_PARAMETERS g_reset_seen;
static HRESULT STDMETHODCALLTYPE dev_Reset(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pp)
{
    (void)This;
    rec("Reset");
    g_reset_seen = *pp;
    return D3D_OK;
}
static IDirect3DDevice9Vtbl g_dev_vtbl = {
    .Release = dev_Release,
    .Reset = dev_Reset,
    .SetRenderState = dev_SetRenderState,
    .SetTexture = dev_SetTexture,
    .SetVertexShaderConstantF = dev_SetVertexShaderConstantF,
    .DrawIndexedPrimitive = dev_DrawIndexedPrimitive,
    .Clear = dev_Clear,
    .CreateTexture = dev_CreateTexture,
    .GetBackBuffer = dev_GetBackBuffer,
    .Present = dev_Present,
};

static ULONG STDMETHODCALLTYPE d3d_Release(IDirect3D9 *This)
{
    return --((MockD3D *)This)->refs;
}
static UINT STDMETHODCALLTYPE d3d_GetAdapterCount(IDirect3D9 *This)
{
    (void)This;
    rec("GetAdapterCount");
    return 1;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterIdentifier(IDirect3D9 *This, UINT adapter, DWORD flags,
                                                          D3DADAPTER_IDENTIFIER9 *id)
{
    (void)This;
    rec("GetAdapterIdentifier");
    R.a[0] = adapter;
    R.a[1] = flags;
    memset(id, 0, sizeof *id);
    strcpy(id->Driver, "mockdrv.dll");
    strcpy(id->Description, "Mock GPU");
    strcpy(id->DeviceName, "\\\\.\\DISPLAY1");
    id->DriverVersion.QuadPart = 0x0001000200030004ll;
    id->VendorId = 0x10DE;
    id->DeviceId = 0x1234;
    id->SubSysId = 0x5678;
    id->Revision = 0xA1;
    id->DeviceIdentifier.Data1 = 0xCAFEF00Du;
    id->WHQLLevel = 7;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterDisplayMode(IDirect3D9 *This, UINT adapter, D3DDISPLAYMODE *m)
{
    (void)This;
    rec("GetAdapterDisplayMode");
    R.a[0] = adapter;
    R.p[0] = m;
    m->Width = 1920;
    m->Height = 1080;
    m->RefreshRate = 60;
    m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormat(IDirect3D9 *This, UINT adapter, D3DDEVTYPE dt,
                                                       D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt,
                                                       D3DFORMAT cf)
{
    (void)This;
    rec("CheckDeviceFormat");
    R.a[0] = adapter;
    R.a[1] = dt;
    R.a[2] = af;
    R.a[3] = usage;
    R.a[4] = rt;
    R.a[5] = cf;
    return D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_GetDeviceCaps(IDirect3D9 *This, UINT adapter, D3DDEVTYPE dt, D3DCAPS9 *c)
{
    (void)This;
    rec("GetDeviceCaps");
    R.a[0] = adapter;
    R.a[1] = dt;
    R.p[0] = c;
    c->MaxTextureWidth = 4096;
    c->MaxVertexShaderConst = 256;
    c->PixelShaderVersion = D3DPS_VERSION(3, 0);
    c->MaxPointSize = 64.0f;
    return D3D_OK;
}
static D3DPRESENT_PARAMETERS g_pp_seen;
static HRESULT STDMETHODCALLTYPE d3d_CreateDevice(IDirect3D9 *This, UINT adapter, D3DDEVTYPE dt, HWND focus,
                                                  DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                                  IDirect3DDevice9 **out)
{
    (void)This;
    rec("CreateDevice");
    R.a[0] = adapter;
    R.a[1] = dt;
    R.p[0] = focus;
    R.a[2] = flags;
    g_pp_seen = *pp;
    pp->BackBufferWidth = 800; /* as D3D9 fills a 0 width in windowed mode */
    pp->BackBufferHeight = 600;
    g_device.refs = 1;
    *out = &g_device.iface;
    return D3D_OK;
}
static UINT STDMETHODCALLTYPE d3d_GetAdapterModeCount(IDirect3D9 *This, UINT adapter, D3DFORMAT f)
{
    (void)This, (void)adapter;
    return f == D3DFMT_X8R8G8B8 ? 88u : 0u;
}
static HRESULT STDMETHODCALLTYPE d3d_EnumAdapterModes(IDirect3D9 *This, UINT adapter, D3DFORMAT f, UINT i,
                                                       D3DDISPLAYMODE *m)
{
    (void)This, (void)adapter;
    if (i >= 88u)
        return D3DERR_INVALIDCALL;
    m->Width = 960u + i, m->Height = 600u, m->RefreshRate = 120u, m->Format = f;
    return D3D_OK;
}
static IDirect3D9Vtbl g_d3d_vtbl = {
    .GetAdapterModeCount = d3d_GetAdapterModeCount,
    .EnumAdapterModes = d3d_EnumAdapterModes,
    .Release = d3d_Release,
    .GetAdapterCount = d3d_GetAdapterCount,
    .GetAdapterIdentifier = d3d_GetAdapterIdentifier,
    .GetAdapterDisplayMode = d3d_GetAdapterDisplayMode,
    .CheckDeviceFormat = d3d_CheckDeviceFormat,
    .GetDeviceCaps = d3d_GetDeviceCaps,
    .CreateDevice = d3d_CreateDevice,
};

void *d3d9g_backend_create9(uint32_t sdk_version)
{
    g_backend_sdk = sdk_version;
    g_d3d.refs = 1;
    return &g_d3d.iface;
}

/* ---- Guest calls --------------------------------------------------------- */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define RET_ADDR 0xDEAD0001u

/* The vtable slot of I::M on the host; the generator checked that it equals
 * the guest's. */
#define SLOT(I, M) ((unsigned)(offsetof(I##Vtbl, M) / sizeof(void *)))

/* Push args right to left and the return address, call fn, and check that it
 * popped 4 + 4 * nargs bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call_fn(recomp_func_t fn, unsigned nargs, const uint32_t *args)
{
    g_esp = STACK_TOP;
    for (unsigned i = nargs; i-- > 0;) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = RET_ADDR;
    uint32_t start = g_esp;
    g_ebx = 0xB0B0B0B0u, g_esi = 0x51515151u, g_edi = 0xD1D1D1D1u, g_ebp = 0xBEBEBEBEu;
    g_eax = 0xCCCCCCCCu;
    fn();
    CHECK(g_esp == start + 4u + 4u * nargs);
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u && g_ebp == 0xBEBEBEBEu);
    return g_eax;
}

/* Call method slot of guest object obj with n arguments, through its
 * guest vtable. */
static uint32_t call_m(uint32_t obj, unsigned slot, unsigned n, ...)
{
    uint32_t args[16];
    args[0] = obj;
    va_list ap;
    va_start(ap, n);
    for (unsigned i = 0; i < n; i++)
        args[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    recomp_func_t fn = guest_resolve(MEM32(MEM32(obj) + 4u * slot));
    CHECK(fn != NULL);
    return fn != NULL ? call_fn(fn, 1u + n, args) : 0u;
}

static uint32_t f32(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- Tests ---------------------------------------------------------------- */

static uint32_t g_d3d_g, g_dev_g, g_tex_g;

static void test_layout(void)
{
    /* Published slots the game's code uses (notes/shader_pipeline.md). */
    CHECK(SLOT(IDirect3DDevice9, CreateVertexShader) == 91);
    CHECK(SLOT(IDirect3DDevice9, CreatePixelShader) == 106);
    CHECK(d3d9g_iface_IDirect3DDevice9.n_methods == 119);
    CHECK(d3d9g_iface_IDirect3D9.n_methods == 17);
    CHECK(d3d9g_iface_IDirect3DTexture9.n_methods == 22);
}

static void test_create9(void)
{
    recomp_func_t fn = NULL;
    for (const win32_export_t *e = win32_exports_d3d; e->dll != NULL; e++)
        if (strcmp(e->dll, "d3d9.dll") == 0 && strcmp(e->name, "Direct3DCreate9") == 0)
            fn = e->fn;
    CHECK(fn == imp_Direct3DCreate9);
    uint32_t sdk = 32;
    g_d3d_g = call_fn(fn, 1, &sdk);
    CHECK(g_backend_sdk == 32);
    CHECK(g_d3d_g != 0u && com_guest_self(g_d3d_g) == &g_d3d.iface);
    CHECK(com_guest_iface(g_d3d_g) == &d3d9g_iface_IDirect3D9);
}

static void test_d3d(void)
{
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterCount), 0) == 1u);
    CHECK(strcmp(R.fn, "GetAdapterCount") == 0);

    /* D3DADAPTER_IDENTIFIER9, converted: i686 layout (pack 4) is 1100 bytes,
     * DriverVersion at 1056 with no 8-byte padding at the end. */
    uint32_t id = guest_malloc(1104);
    memset(guest_ptr(id), 0xEE, 1104);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterIdentifier), 3, 0u, 0u, id) == D3D_OK);
    CHECK(R.a[0] == 0 && R.a[1] == 0);
    CHECK(strcmp(guest_ptr(id), "mockdrv.dll") == 0);
    CHECK(strcmp(guest_ptr(id + 512), "Mock GPU") == 0);
    CHECK(strcmp(guest_ptr(id + 1024), "\\\\.\\DISPLAY1") == 0);
    CHECK(MEM32(id + 1056) == 0x00030004u && MEM32(id + 1060) == 0x00010002u);
    CHECK(MEM32(id + 1064) == 0x10DEu && MEM32(id + 1068) == 0x1234u);
    CHECK(MEM32(id + 1072) == 0x5678u && MEM32(id + 1076) == 0xA1u);
    CHECK(MEM32(id + 1080) == 0xCAFEF00Du && MEM32(id + 1096) == 7u);
    CHECK(MEM32(id + 1100) == 0xEEEEEEEEu); /* nothing past the guest struct */

    /* D3DDISPLAYMODE: the host writes guest memory itself. Windowed passes
     * the host's modes through; the desktop default lists the panel, 75%,
     * the desktop (also the current mode, at the display's rate) and 50%,
     * at 60 Hz, and sets the game's zero ScreenWidth/ScreenHeight to the
     * desktop size. */
    uint32_t mode = guest_malloc(16);
    const uint32_t X8 = (uint32_t)D3DFMT_X8R8G8B8, R5 = (uint32_t)D3DFMT_R5G6B5;
    setenv("HPRECOMP_WINDOWED", "1", 1);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterDisplayMode), 2, 0u, mode) == D3D_OK);
    CHECK(R.p[0] == guest_ptr(mode));
    CHECK(MEM32(mode) == 1920u && MEM32(mode + 4) == 1080u && MEM32(mode + 8) == 60u && MEM32(mode + 12) == X8);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterModeCount), 2, 0u, X8) == 88u);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, 5u, mode) == D3D_OK);
    CHECK(MEM32(mode) == 965u && MEM32(mode + 4) == 600u);
    unsetenv("HPRECOMP_WINDOWED");
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterDisplayMode), 2, 0u, mode) == D3D_OK);
    CHECK(MEM32(mode) == 2056u && MEM32(mode + 4) == 1290u && MEM32(mode + 8) == 120u && MEM32(mode + 12) == X8);
    /* The configured screen size: tests/run_all.sh puts GAME_SCREEN_WIDTH_VA at
     * the start of a 16 KiB host page. */
    CHECK(mprotect(guest_ptr(GAME_SCREEN_WIDTH_VA), 0x4000u, PROT_READ | PROT_WRITE) == 0);
    MEM32(GAME_SCREEN_WIDTH_VA) = 1024u, MEM32(GAME_SCREEN_HEIGHT_VA) = 768u; /* first run */
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterModeCount), 2, 0u, X8) == 4u);
    CHECK(MEM32(GAME_SCREEN_WIDTH_VA) == 2056u && MEM32(GAME_SCREEN_HEIGHT_VA) == 1290u);
    MEM32(GAME_SCREEN_WIDTH_VA) = 2592u, MEM32(GAME_SCREEN_HEIGHT_VA) = 1626u; /* a listed choice stays */
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterModeCount), 2, 0u, X8) == 4u);
    CHECK(MEM32(GAME_SCREEN_WIDTH_VA) == 2592u && MEM32(GAME_SCREEN_HEIGHT_VA) == 1626u);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterModeCount), 2, 0u, R5) == 0u);
    static const uint32_t want[4][2] = {{2056u, 1290u}, {3456u, 2168u}, {2592u, 1626u}, {1728u, 1084u}};
    for (uint32_t i = 0; i < 4u; i++) {
        CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, i, mode) == D3D_OK);
        CHECK(MEM32(mode) == want[i][0] && MEM32(mode + 4) == want[i][1] && MEM32(mode + 8) == 60u &&
              MEM32(mode + 12) == X8);
    }
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, 4u, mode) == (uint32_t)D3DERR_INVALIDCALL);
    g_default_scaling = 1; /* 50% of the panel duplicates the desktop: listed once */
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetAdapterModeCount), 2, 0u, X8) == 3u);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, 0u, mode) == D3D_OK);
    CHECK(MEM32(mode) == 1728u && MEM32(mode + 4) == 1084u);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, 2u, mode) == D3D_OK);
    CHECK(MEM32(mode) == 2592u && MEM32(mode + 4) == 1626u);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, EnumAdapterModes), 4, 0u, X8, 3u, mode) == (uint32_t)D3DERR_INVALIDCALL);
    g_default_scaling = 0;

    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CheckDeviceFormat), 6, 0u, (uint32_t)D3DDEVTYPE_HAL,
                 (uint32_t)D3DFMT_X8R8G8B8, (uint32_t)D3DUSAGE_RENDERTARGET, (uint32_t)D3DRTYPE_TEXTURE,
                 (uint32_t)D3DFMT_DXT5) == (uint32_t)D3DERR_NOTAVAILABLE);
    CHECK(R.a[0] == 0 && R.a[1] == D3DDEVTYPE_HAL && R.a[2] == D3DFMT_X8R8G8B8 &&
          R.a[3] == D3DUSAGE_RENDERTARGET && R.a[4] == D3DRTYPE_TEXTURE && R.a[5] == D3DFMT_DXT5);

    uint32_t caps = guest_malloc(304);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, GetDeviceCaps), 3, 0u, (uint32_t)D3DDEVTYPE_HAL, caps) == D3D_OK);
    CHECK(R.p[0] == guest_ptr(caps));
    const D3DCAPS9 *c = guest_ptr(caps);
    CHECK(c->MaxTextureWidth == 4096u && c->MaxVertexShaderConst == 256u &&
          c->PixelShaderVersion == D3DPS_VERSION(3, 0) && c->MaxPointSize == 64.0f);
}

static void test_create_device(void)
{
    /* D3DPRESENT_PARAMETERS in the i686 layout: 56 bytes, hDeviceWindow a
     * 32-bit slot at 28. */
    uint32_t pp = guest_malloc(56);
    uint32_t v[14] = {0, 0, D3DFMT_X8R8G8B8, 1, D3DMULTISAMPLE_NONE, 0, D3DSWAPEFFECT_DISCARD, 0x00010030u,
                      TRUE, TRUE, D3DFMT_D24S8, D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL, 0,
                      D3DPRESENT_INTERVAL_IMMEDIATE};
    for (unsigned i = 0; i < 14; i++)
        MEM32(pp + 4u * i) = v[i];
    uint32_t out = guest_malloc(4);
    MEM32(out) = 0xDDDDDDDDu;
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CreateDevice), 6, 0u, (uint32_t)D3DDEVTYPE_HAL, 0x00010020u,
                 (uint32_t)D3DCREATE_HARDWARE_VERTEXPROCESSING, pp, out) == D3D_OK);
    CHECK(R.a[0] == 0 && R.a[1] == D3DDEVTYPE_HAL && R.a[2] == D3DCREATE_HARDWARE_VERTEXPROCESSING);
    CHECK(R.p[0] == host_window(0x00010020u));
    CHECK(g_pp_seen.BackBufferFormat == D3DFMT_X8R8G8B8 && g_pp_seen.BackBufferCount == 1u &&
          g_pp_seen.SwapEffect == D3DSWAPEFFECT_DISCARD && g_pp_seen.hDeviceWindow == host_window(0x00010030u) &&
          g_pp_seen.Windowed == TRUE && g_pp_seen.EnableAutoDepthStencil == TRUE &&
          g_pp_seen.AutoDepthStencilFormat == D3DFMT_D24S8 &&
          g_pp_seen.Flags == D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL &&
          g_pp_seen.PresentationInterval == D3DPRESENT_INTERVAL_ONE);
    /* Copied back, HWND mapped back to the guest's. */
    CHECK(MEM32(pp) == 800u && MEM32(pp + 4) == 600u && MEM32(pp + 28) == 0x00010030u &&
          MEM32(pp + 52) == D3DPRESENT_INTERVAL_IMMEDIATE);
    g_dev_g = MEM32(out);
    CHECK(g_dev_g != 0u && com_guest_self(g_dev_g) == &g_device.iface);
    CHECK(com_guest_iface(g_dev_g) == &d3d9g_iface_IDirect3DDevice9);
}

/* Windows D3D9 sets the calling thread's x87 control word in CreateDevice
 * unless D3DCREATE_FPU_PRESERVE (0x2) is set: (cw & 0xF0C0) | 0x003F. */
static void test_create_device_fpu(void)
{
    uint32_t pp = guest_malloc(56);
    uint32_t out = guest_malloc(4);
    memset(guest_ptr(pp), 0, 56);
    g_fp_control_word = 0x027F;
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CreateDevice), 6, 0u, (uint32_t)D3DDEVTYPE_HAL, 0u, 0x52u, pp,
                 out) == D3D_OK);
    CHECK(g_fp_control_word == 0x027F);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CreateDevice), 6, 0u, (uint32_t)D3DDEVTYPE_HAL, 0u, 0x50u, pp,
                 out) == D3D_OK);
    CHECK(g_fp_control_word == 0x007F);
    /* Rounding control (bits 10-11) goes back to nearest too. */
    g_fp_control_word = 0x0E7F;
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CreateDevice), 6, 0u, (uint32_t)D3DDEVTYPE_HAL, 0u, 0x50u, pp,
                 out) == D3D_OK);
    CHECK(g_fp_control_word == 0x007F);
    CHECK(MEM32(out) == g_dev_g);
}

/* The game asks for exclusive fullscreen (Windowed FALSE, 60 Hz, 960x600,
 * no vsync). HPRECOMP_WINDOWED=1 fits a window to the back buffer, the
 * default asks for the desktop, HPRECOMP_FULLSCREEN=1 passes the mode
 * through; the host always gets interval ONE, and the game's struct keeps
 * its own values. */
static void test_windowed(void)
{
    uint32_t pp = guest_malloc(56);
    uint32_t out = guest_malloc(4);
    memset(guest_ptr(pp), 0, 56);
    MEM32(pp + 0) = 960u, MEM32(pp + 4) = 600u, MEM32(pp + 28) = 0x00010030u;
    MEM32(pp + 32) = FALSE, MEM32(pp + 48) = 60u, MEM32(pp + 52) = D3DPRESENT_INTERVAL_IMMEDIATE;
    memset(&g_fit, 0, sizeof g_fit);
    memset(&g_show, 0, sizeof g_show);
    setenv("HPRECOMP_WINDOWED", "1", 1);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, CreateDevice), 6, 0u, (uint32_t)D3DDEVTYPE_HAL, 0x00010020u, 0x52u,
                 pp, out) == D3D_OK);
    unsetenv("HPRECOMP_WINDOWED");
    CHECK(g_pp_seen.Windowed == TRUE && g_pp_seen.FullScreen_RefreshRateInHz == 0u);
    CHECK(g_pp_seen.PresentationInterval == D3DPRESENT_INTERVAL_ONE &&
          MEM32(pp + 52) == D3DPRESENT_INTERVAL_IMMEDIATE);
    CHECK(g_pp_seen.BackBufferWidth == 960u && g_pp_seen.BackBufferHeight == 600u);
    CHECK(MEM32(pp + 32) == FALSE && MEM32(pp + 48) == 60u);
    CHECK(g_fit.calls == 1 && g_fit.win == host_window(0x00010030u) && g_fit.w == 960u && g_fit.h == 600u &&
          g_fit.desktop == 0);
    /* A fullscreen request shows the device window, as Windows does. */
    CHECK(g_show.calls == 1 && g_show.win == host_window(0x00010030u));

    /* Reset without hDeviceWindow fits the CreateDevice window; the default
     * asks for the desktop. */
    MEM32(pp + 0) = 640u, MEM32(pp + 4) = 480u, MEM32(pp + 28) = 0u;
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Reset), 1, pp) == D3D_OK);
    CHECK(g_reset_seen.Windowed == TRUE && g_reset_seen.FullScreen_RefreshRateInHz == 0u);
    CHECK(MEM32(pp + 32) == FALSE && MEM32(pp + 48) == 60u);
    CHECK(g_fit.calls == 2 && g_fit.win == host_window(0x00010030u) && g_fit.w == 640u && g_fit.h == 480u &&
          g_fit.desktop == 1);
    CHECK(g_show.calls == 2 && g_show.win == host_window(0x00010030u));
    /* A later Reset leaves the window as Ctrl+Cmd+F left it. */
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Reset), 1, pp) == D3D_OK);
    CHECK(g_fit.calls == 2 && g_reset_seen.Windowed == TRUE);

    setenv("HPRECOMP_FULLSCREEN", "1", 1);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Reset), 1, pp) == D3D_OK);
    CHECK(g_reset_seen.Windowed == FALSE && g_reset_seen.FullScreen_RefreshRateInHz == 60u);
    CHECK(g_reset_seen.PresentationInterval == D3DPRESENT_INTERVAL_ONE);
    unsetenv("HPRECOMP_FULLSCREEN");
    CHECK(MEM32(pp + 52) == D3DPRESENT_INTERVAL_IMMEDIATE);
    CHECK(g_fit.calls == 2 && g_show.calls == 4);

    /* A windowed Reset shows nothing. */
    MEM32(pp + 32) = TRUE;
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Reset), 1, pp) == D3D_OK);
    CHECK(g_show.calls == 4);
}

static void test_device(void)
{
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, SetRenderState), 2, (uint32_t)D3DRS_ZENABLE, 1u) == D3D_OK);
    CHECK(strcmp(R.fn, "SetRenderState") == 0 && R.a[0] == D3DRS_ZENABLE && R.a[1] == 1);

    /* CreateTexture's out-parameter yields a guest texture object. */
    uint32_t out = guest_malloc(4);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, CreateTexture), 8, 16u, 8u, 1u, 0u,
                 (uint32_t)D3DFMT_A8R8G8B8, (uint32_t)D3DPOOL_MANAGED, out, 0u) == D3D_OK);
    CHECK(R.a[0] == 16 && R.a[1] == 8 && R.a[2] == 1 && R.a[3] == 0 && R.a[4] == D3DFMT_A8R8G8B8 &&
          R.a[5] == D3DPOOL_MANAGED && R.p[0] == NULL);
    g_tex_g = MEM32(out);
    CHECK(g_tex_g != 0u && com_guest_self(g_tex_g) == &g_texture.iface);
    CHECK(com_guest_iface(g_tex_g) == &d3d9g_iface_IDirect3DTexture9);

    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, SetTexture), 2, 3u, g_tex_g) == D3D_OK);
    CHECK(R.a[0] == 3 && R.p[0] == &g_texture.iface);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, SetTexture), 2, 1u, 0u) == D3D_OK);
    CHECK(R.a[0] == 1 && R.p[0] == NULL);

    uint32_t consts = guest_malloc(32);
    for (unsigned i = 0; i < 8; i++)
        MEM32(consts + 4u * i) = f32(0.5f * (float)i);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, SetVertexShaderConstantF), 3, 4u, consts, 2u) == D3D_OK);
    CHECK(R.a[0] == 4 && R.a[1] == 2 && R.p[0] == guest_ptr(consts) && ((const float *)R.p[0])[7] == 3.5f);

    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, DrawIndexedPrimitive), 6, (uint32_t)D3DPT_TRIANGLELIST,
                 (uint32_t)-5, 2u, 100u, 30u, 50u) == D3D_OK);
    CHECK(R.a[0] == D3DPT_TRIANGLELIST && (int64_t)R.a[1] == -5 && R.a[2] == 2 && R.a[3] == 100 &&
          R.a[4] == 30 && R.a[5] == 50);

    uint32_t rects = guest_malloc(32);
    for (unsigned i = 0; i < 8; i++)
        MEM32(rects + 4u * i) = 10u * i;
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Clear), 6, 2u, rects,
                 (uint32_t)(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), 0xFF112233u, f32(0.75f), 9u) == D3D_OK);
    CHECK(R.a[0] == 2 && R.p[0] == guest_ptr(rects) && ((const D3DRECT *)R.p[0])[1].x2 == 60 &&
          R.a[1] == (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) && R.a[2] == 0xFF112233u && R.f == 0.75f &&
          R.a[3] == 9);

    /* GetBackBuffer twice: the same guest object. */
    uint32_t b1 = guest_malloc(4), b2 = guest_malloc(4);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, GetBackBuffer), 4, 0u, 0u, (uint32_t)D3DBACKBUFFER_TYPE_MONO,
                 b1) == D3D_OK);
    CHECK(R.a[0] == 0 && R.a[1] == 0 && R.a[2] == D3DBACKBUFFER_TYPE_MONO);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, GetBackBuffer), 4, 0u, 0u, (uint32_t)D3DBACKBUFFER_TYPE_MONO,
                 b2) == D3D_OK);
    uint32_t surf = MEM32(b1);
    CHECK(surf != 0u && MEM32(b2) == surf && g_backbuffer.refs == 2u);
    CHECK(com_guest_iface(surf) == &d3d9g_iface_IDirect3DSurface9);

    /* Release to zero frees the guest object; the next GetBackBuffer makes a
     * new one. */
    int frees = g_frees;
    CHECK(call_m(surf, SLOT(IDirect3DSurface9, Release), 0) == 1u && obj_live(surf));
    CHECK(call_m(surf, SLOT(IDirect3DSurface9, Release), 0) == 0u);
    CHECK(!obj_live(surf) && g_frees == frees + 1 && d3d9g_find(&g_backbuffer.iface) == 0u);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, GetBackBuffer), 4, 0u, 0u, (uint32_t)D3DBACKBUFFER_TYPE_MONO,
                 b1) == D3D_OK);
    CHECK(MEM32(b1) != 0u && obj_live(MEM32(b1)) && com_guest_self(MEM32(b1)) == &g_backbuffer.iface);

    /* Present: NULLs stay NULL; a window override is converted. */
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Present), 4, 0u, 0u, 0u, 0u) == D3D_OK);
    CHECK(strcmp(R.fn, "Present") == 0 && R.p[0] == NULL && R.p[1] == NULL && R.p[2] == NULL && R.p[3] == NULL);
    uint32_t src = guest_malloc(16);
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Present), 4, src, 0u, 0x00010030u, 0u) == D3D_OK);
    CHECK(R.p[0] == guest_ptr(src) && R.p[2] == host_window(0x00010030u));
}

static void test_lock(void)
{
    for (unsigned i = 0; i < sizeof g_texture.mem; i++)
        g_texture.mem[i] = (uint8_t)i;
    uint32_t lr = guest_malloc(8);
    CHECK(call_m(g_tex_g, SLOT(IDirect3DTexture9, LockRect), 4, 0u, lr, 0u, 0u) == D3D_OK);
    CHECK(strcmp(R.fn, "LockRect") == 0 && R.a[0] == 0 && R.p[0] == NULL && g_texture.locked);
    uint32_t pitch = MEM32(lr), bits = MEM32(lr + 4);
    CHECK(pitch == 64u && bits != 0u);
    uint8_t *gb = guest_ptr(bits);
    CHECK(gb[0] == 0 && gb[100] == 100 && gb[511] == (uint8_t)511); /* copied in */
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 64; x++)
            gb[y * pitch + x] = (uint8_t)(0xA0 + y);
    CHECK(g_texture.mem[0] == 0); /* host memory untouched until Unlock */
    CHECK(call_m(g_tex_g, SLOT(IDirect3DTexture9, UnlockRect), 1, 0u) == D3D_OK);
    CHECK(strcmp(R.fn, "UnlockRect") == 0 && !g_texture.locked);
    CHECK(g_texture.mem[0] == 0xA0 && g_texture.mem[64 * 7 + 63] == 0xA7 && g_texture.mem[64 * 3] == 0xA3);

    /* A rect lock covers rows 1-2, texels 2-5: 64 + 16 bytes from (2, 1). */
    uint32_t rect = guest_malloc(16);
    MEM32(rect) = 2;
    MEM32(rect + 4) = 1;
    MEM32(rect + 8) = 6;
    MEM32(rect + 12) = 3;
    CHECK(call_m(g_tex_g, SLOT(IDirect3DTexture9, LockRect), 4, 0u, lr, rect, (uint32_t)D3DLOCK_DISCARD) ==
          D3D_OK);
    CHECK(R.p[0] == guest_ptr(rect) && R.a[1] == D3DLOCK_DISCARD);
    gb = guest_ptr(MEM32(lr + 4));
    for (unsigned i = 0; i < 16; i++) {
        gb[i] = 0x11;
        gb[64 + i] = 0x22;
    }
    CHECK(call_m(g_tex_g, SLOT(IDirect3DTexture9, UnlockRect), 1, 0u) == D3D_OK);
    CHECK(g_texture.mem[64 + 8] == 0x11 && g_texture.mem[64 + 23] == 0x11 && g_texture.mem[128 + 8] == 0x22 &&
          g_texture.mem[128 + 23] == 0x22 && g_texture.mem[128 + 24] == 0xA2 && g_texture.mem[64 + 7] == 0xA1);

    /* Release the texture to zero. */
    CHECK(call_m(g_tex_g, SLOT(IDirect3DTexture9, Release), 0) == 0u && !obj_live(g_tex_g));
}

/* CreateTexture with a non-NULL pSharedHandle aborts with an UNSUPPORTED
 * line (in a child, stderr to a pipe). */
static void test_shared_handle(void)
{
    int fd[2];
    if (pipe(fd) != 0)
        abort();
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(fd[1], 2);
        uint32_t out = guest_malloc(4), shared = guest_malloc(4);
        call_m(g_dev_g, SLOT(IDirect3DDevice9, CreateTexture), 8, 16u, 8u, 1u, 0u, (uint32_t)D3DFMT_A8R8G8B8,
               (uint32_t)D3DPOOL_MANAGED, out, shared);
        _exit(0);
    }
    close(fd[1]);
    char buf[512] = {0};
    ssize_t n = read(fd[0], buf, sizeof buf - 1);
    (void)n;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(CHILD_ABORTED(st));
    CHECK(strstr(buf, "UNSUPPORTED d3d9: IDirect3DDevice9::CreateTexture: pSharedHandle is not NULL") != NULL);
}

static void test_release(void)
{
    int frees = g_frees;
    CHECK(call_m(g_dev_g, SLOT(IDirect3DDevice9, Release), 0) == 0u);
    CHECK(strcmp(R.fn, "Release") == 0 && !obj_live(g_dev_g) && g_frees == frees + 1);
    CHECK(call_m(g_d3d_g, SLOT(IDirect3D9, Release), 0) == 0u && !obj_live(g_d3d_g));
}

int main(void)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    g_backbuffer.iface.lpVtbl = &g_surf_vtbl;
    g_texture.iface.lpVtbl = &g_tex_vtbl;
    g_device.iface.lpVtbl = &g_dev_vtbl;
    g_d3d.iface.lpVtbl = &g_d3d_vtbl;

    test_layout();
    test_create9();
    test_d3d();
    test_create_device();
    test_create_device_fpu();
    test_windowed();
    test_device();
    test_lock();
    test_shared_handle();
    test_release();
    if (g_fail) {
        printf("test_d3d9_guest: FAILED\n");
        return 1;
    }
    printf("test_d3d9_guest: all checks passed\n");
    return 0;
}
