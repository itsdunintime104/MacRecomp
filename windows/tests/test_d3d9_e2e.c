/* End-to-end guest Direct3D 9 on DXVK Native: guest-stack calls to the UI
 * slice's CreateWindowExA, the guest COM layer's Direct3DCreate9 and
 * IDirect3D9/IDirect3DDevice9/IDirect3DSurface9 methods, and the real
 * backend (d3d9_backend.c). Clears the back buffer, reads it back through
 * GetRenderTargetData and a guest LockRect, checks every pixel, presents 60
 * frames and releases everything. Needs a GPU and a visible window; run with
 * VK_DRIVER_FILES=<KosmicKrisp ICD> DXVK_WSI_DRIVER=SDL2
 * DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib (notes/dxvk_triangle.md).
 * Links win32_ui.c, gen_d3d9_guest.c, d3d9_guest_manual.c, d3d9_backend.c,
 * d3dx_shader.c, d3dx_misc.c (gen_d3d9_guest.c exports their 13 D3DX thunks),
 * com_guest.c, win32_core.c, win32_unsupported.c and the engine's
 * guest_mem.c, guest_heap.c, dlmalloc.c, host_funcs.c, recomp_glue.c and
 * guest_call.c, plus libdxvk_d3d9, vkd3d-shader and SDL2. */
#include <SDL2/SDL.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>

#include "recomp_types.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "com_guest.h"
#include "d3d9_guest.h"
#include "win32_thunk.h"
#include "win32_ui.h"

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

/* A crash or abort() prints the signal and exits 2, so macOS shows no crash
 * dialog. */
static void on_fatal(int sig)
{
    char msg[64];
    int n = snprintf(msg, sizeof msg, "FATAL signal %d\n", sig);
    if (write(2, msg, (size_t)n) < 0)
        _exit(3);
    _exit(2);
}

/* ---- Guest memory ------------------------------------------------------ */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define TIB 0x00FFE000u
#define RET_ADDR 0xDEAD0001u
#define MAIN_TID 0x1234u

static uint32_t galloc(uint32_t n)
{
    uint32_t a = guest_malloc(n);
    CHECK(a != 0u);
    memset(guest_ptr(a), 0, n);
    return a;
}

static uint32_t gstring(const char *s)
{
    uint32_t a = galloc((uint32_t)strlen(s) + 1u);
    strcpy((char *)guest_ptr(a), s);
    return a;
}

/* ---- Guest calls --------------------------------------------------------- */

/* Push args right to left and the return address, call fn, and check that it
 * popped 4 + 4 * nargs bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call_fn(const char *what, recomp_func_t fn, unsigned nargs, const uint32_t *args)
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
    if (g_esp != start + 4u + 4u * nargs) {
        fprintf(stderr, "FAIL: %s popped %d bytes, want %u\n", what, (int)(g_esp - start), 4u + 4u * nargs);
        g_fail = 1;
    }
    if (g_ebx != 0xB0B0B0B0u || g_esi != 0x51515151u || g_edi != 0xD1D1D1D1u || g_ebp != 0xBEBEBEBEu) {
        fprintf(stderr, "FAIL: %s changed a callee-saved register\n", what);
        g_fail = 1;
    }
    return g_eax;
}

static recomp_func_t export_fn(const win32_export_t *table, const char *name)
{
    for (const win32_export_t *e = table; e->dll != NULL; e++)
        if (e->name != NULL && strcmp(e->name, name) == 0)
            return e->fn;
    fprintf(stderr, "FATAL: %s not exported\n", name);
    _exit(2);
}

static uint32_t call_ui(const char *name, unsigned nargs, const uint32_t *args)
{
    return call_fn(name, export_fn(win32_exports_ui, name), nargs, args);
}
#define UI(name, ...) \
    call_ui(name, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t), (uint32_t[]){__VA_ARGS__})

/* The vtable slot of I::M on the host; the generator checked that it equals
 * the guest's. */
#define SLOT(I, M) ((unsigned)(offsetof(I##Vtbl, M) / sizeof(void *)))

/* Call method slot of guest object obj with n arguments, through its guest
 * vtable. */
static uint32_t call_m(const char *what, uint32_t obj, unsigned slot, unsigned n, ...)
{
    uint32_t args[16];
    args[0] = obj;
    va_list ap;
    va_start(ap, n);
    for (unsigned i = 0; i < n; i++)
        args[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    recomp_func_t fn = guest_resolve(MEM32(MEM32(obj) + 4u * slot));
    if (fn == NULL) {
        fprintf(stderr, "FATAL: %s: no thunk in slot %u\n", what, slot);
        _exit(2);
    }
    return call_fn(what, fn, 1u + n, args);
}
#define M(obj, I, Meth, n, ...) call_m(#I "::" #Meth, obj, SLOT(I, Meth), n, ##__VA_ARGS__)

static uint32_t f32(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- Window ------------------------------------------------------------ */

#define WS_OVERLAPPEDWINDOW_ 0x00CF0000u
#define WS_VISIBLE_ 0x10000000u
#define CW_USEDEFAULT_ 0x80000000u
#define PM_REMOVE_ 1u

static int g_wndproc_calls;
static recomp_func_t g_defwndproc;

/* A stdcall WNDPROC that returns DefWindowProcA(hwnd, msg, wp, lp): its
 * frame is already DefWindowProcA's, so it tail-calls it. */
static void test_wndproc(void)
{
    g_wndproc_calls++;
    g_defwndproc();
}

static uint32_t g_msg;

/* PeekMessageA(PM_REMOVE) + DispatchMessageA until the queue is empty. */
static int pump(void)
{
    int n = 0;
    while (n < 1000 && UI("PeekMessageA", g_msg, 0, 0, 0, PM_REMOVE_)) {
        UI("DispatchMessageA", g_msg);
        n++;
    }
    CHECK(n < 1000);
    return n;
}

static uint32_t make_window(void)
{
    g_defwndproc = export_fn(win32_exports_ui, "DefWindowProcA");
    uint32_t wc = galloc(48);
    MEM32(wc + 0) = 48;
    MEM32(wc + 4) = 3; /* CS_HREDRAW | CS_VREDRAW */
    MEM32(wc + 8) = host_func_va(test_wndproc);
    MEM32(wc + 20) = 0x00400000u;
    MEM32(wc + 40) = gstring("D3D9E2E");
    CHECK(UI("RegisterClassExA", wc) != 0u);
    uint32_t hwnd = UI("CreateWindowExA", 0, gstring("D3D9E2E"), gstring("d3d9 e2e"),
                       WS_OVERLAPPEDWINDOW_ | WS_VISIBLE_, CW_USEDEFAULT_, CW_USEDEFAULT_, 640, 480, 0, 0,
                       0x00400000u, 0);
    CHECK(hwnd != 0u);
    CHECK(w32_hwnd_to_sdl(hwnd) != NULL);
    CHECK(g_wndproc_calls >= 2); /* WM_NCCREATE, WM_CREATE, ... */
    int n = pump();
    printf("window 0x%08X: %d wndproc calls, %d messages pumped\n", hwnd, g_wndproc_calls, n);
    return hwnd;
}

/* ---- Direct3D ------------------------------------------------------------- */

#define W 640u
#define H 480u
#define COLOR 0xFF204060u

static uint32_t create_d3d(void)
{
    uint32_t sdk = D3D_SDK_VERSION;
    uint32_t d3d = call_fn("Direct3DCreate9", export_fn(win32_exports_d3d, "Direct3DCreate9"), 1, &sdk);
    if (d3d == 0u) {
        fprintf(stderr, "FATAL: Direct3DCreate9 returned NULL\n");
        _exit(2);
    }
    CHECK(com_guest_iface(d3d) == &d3d9g_iface_IDirect3D9);
    /* Screen sizes: the desktop in points, less a notch band of at most
     * 64 points (2056x1290 on a 2056x1329 notched screen). */
    uint32_t dw, dh, pw, ph, hz;
    SDL_DisplayMode dm;
    d3d9g_backend_screen_sizes(&dw, &dh, &pw, &ph, &hz);
    printf("screen sizes: desktop %ux%u panel %ux%u %u Hz\n", dw, dh, pw, ph, hz);
    CHECK(SDL_GetDesktopDisplayMode(0, &dm) == 0);
    CHECK(dw == (uint32_t)dm.w && dh <= (uint32_t)dm.h && dh + 64u >= (uint32_t)dm.h && pw >= dw && hz >= 1u);
    uint32_t n = M(d3d, IDirect3D9, GetAdapterCount, 0);
    printf("adapters: %u\n", n);
    CHECK(n >= 1u);
    uint32_t id = galloc(1100);
    CHECK(M(d3d, IDirect3D9, GetAdapterIdentifier, 3, 0u, 0u, id) == D3D_OK);
    const char *desc = guest_ptr(id + 512);
    CHECK(desc[0] != '\0');
    printf("adapter 0: %.512s / %.512s\n", desc, (const char *)guest_ptr(id));
    return d3d;
}

static uint32_t create_device(uint32_t d3d, uint32_t hwnd)
{
    /* D3DPRESENT_PARAMETERS in the i686 layout: 56 bytes. */
    uint32_t pp = galloc(56);
    const uint32_t v[14] = {W, H, D3DFMT_X8R8G8B8, 1, D3DMULTISAMPLE_NONE, 0, D3DSWAPEFFECT_DISCARD, hwnd,
                            TRUE, FALSE, 0, 0, 0, D3DPRESENT_INTERVAL_IMMEDIATE};
    for (unsigned i = 0; i < 14; i++)
        MEM32(pp + 4u * i) = v[i];
    uint32_t out = galloc(4);
    uint32_t hr = M(d3d, IDirect3D9, CreateDevice, 6, 0u, (uint32_t)D3DDEVTYPE_HAL, hwnd,
                    (uint32_t)D3DCREATE_HARDWARE_VERTEXPROCESSING, pp, out);
    printf("CreateDevice: 0x%08X\n", hr);
    if (hr != D3D_OK || MEM32(out) == 0u) {
        fprintf(stderr, "FATAL: CreateDevice failed\n");
        _exit(2);
    }
    CHECK(MEM32(pp) == W && MEM32(pp + 4) == H && MEM32(pp + 28) == hwnd);
    CHECK(com_guest_iface(MEM32(out)) == &d3d9g_iface_IDirect3DDevice9);
    return MEM32(out);
}

static void clear_and_read(uint32_t dev)
{
    CHECK(M(dev, IDirect3DDevice9, Clear, 6, 0u, 0u, (uint32_t)D3DCLEAR_TARGET, COLOR, f32(1.0f), 0u) == D3D_OK);
    CHECK(M(dev, IDirect3DDevice9, BeginScene, 0) == D3D_OK);
    CHECK(M(dev, IDirect3DDevice9, EndScene, 0) == D3D_OK);

    uint32_t out = galloc(4);
    CHECK(M(dev, IDirect3DDevice9, GetBackBuffer, 4, 0u, 0u, (uint32_t)D3DBACKBUFFER_TYPE_MONO, out) == D3D_OK);
    uint32_t bb = MEM32(out);
    CHECK(bb != 0u && com_guest_iface(bb) == &d3d9g_iface_IDirect3DSurface9);
    MEM32(out) = 0;
    CHECK(M(dev, IDirect3DDevice9, CreateOffscreenPlainSurface, 6, W, H, (uint32_t)D3DFMT_X8R8G8B8,
            (uint32_t)D3DPOOL_SYSTEMMEM, out, 0u) == D3D_OK);
    uint32_t sys = MEM32(out);
    CHECK(sys != 0u && com_guest_iface(sys) == &d3d9g_iface_IDirect3DSurface9);
    CHECK(M(dev, IDirect3DDevice9, GetRenderTargetData, 2, bb, sys) == D3D_OK);

    uint32_t lr = galloc(8);
    CHECK(M(sys, IDirect3DSurface9, LockRect, 3, lr, 0u, (uint32_t)D3DLOCK_READONLY) == D3D_OK);
    uint32_t pitch = MEM32(lr), bits = MEM32(lr + 4);
    printf("LockRect: pitch %u, guest bits 0x%08X\n", pitch, bits);
    CHECK(pitch >= 4u * W && bits != 0u);
    if (pitch >= 4u * W && bits != 0u) {
        unsigned bad = 0;
        uint32_t first = 0, fx = 0, fy = 0;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint32_t p = MEM32(bits + y * pitch + 4u * x) & 0x00FFFFFFu;
                if (p != (COLOR & 0x00FFFFFFu) && bad++ == 0)
                    first = p, fx = x, fy = y;
            }
        printf("%s clear: %u/%u pixels 0x%06X; (0,0)=0x%06X (639,479)=0x%06X; first mismatch 0x%06X at (%u,%u)\n",
               bad == 0 ? "PASS" : "FAIL", W * H - bad, W * H, COLOR & 0x00FFFFFFu, MEM32(bits) & 0x00FFFFFFu,
               MEM32(bits + (H - 1u) * pitch + 4u * (W - 1u)) & 0x00FFFFFFu, first, fx, fy);
        CHECK(bad == 0);
    }
    CHECK(M(sys, IDirect3DSurface9, UnlockRect, 0) == D3D_OK);
    CHECK(M(sys, IDirect3DSurface9, Release, 0) == 0u);
    CHECK(M(bb, IDirect3DSurface9, Release, 0) == 0u);
}

static void present(uint32_t dev)
{
    int ok = 0;
    for (int i = 0; i < 60; i++) {
        CHECK(M(dev, IDirect3DDevice9, Clear, 6, 0u, 0u, (uint32_t)D3DCLEAR_TARGET, COLOR, f32(1.0f), 0u) ==
              D3D_OK);
        uint32_t hr = M(dev, IDirect3DDevice9, Present, 4, 0u, 0u, 0u, 0u);
        if (hr == D3D_OK)
            ok++;
        else
            fprintf(stderr, "FAIL: Present %d: 0x%08X\n", i, hr);
        pump();
    }
    printf("%s present: %d/60 D3D_OK\n", ok == 60 ? "PASS" : "FAIL", ok);
    CHECK(ok == 60);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (guest_mem_init() != 0 || guest_heap_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 2;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    MEM32(TIB + 0x24) = MAIN_TID; /* TEB ClientId.UniqueThread */
    g_fs_base = TIB;
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGABRT, on_fatal);
    g_msg = galloc(28);

    w32_ui_init();
    w32_ui_set_window_flags(SDL_WINDOW_VULKAN);
    uint32_t hwnd = make_window();
    uint32_t d3d = create_d3d();
    uint32_t dev = create_device(d3d, hwnd);
    clear_and_read(dev);
    present(dev);

    CHECK(M(dev, IDirect3DDevice9, Release, 0) == 0u);
    CHECK(M(d3d, IDirect3D9, Release, 0) == 0u);
    CHECK(UI("DestroyWindow", hwnd) == 1u);
    pump();

    if (g_fail) {
        printf("test_d3d9_e2e: FAILED\n");
        return 1;
    }
    printf("test_d3d9_e2e: PASS\n");
    return 0;
}
