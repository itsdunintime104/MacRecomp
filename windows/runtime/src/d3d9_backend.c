/* D3D9 backend: the hooks runtime/include/d3d9_guest.h asks for, on
 * DXVK Native with its SDL2 window backend (notes/dxvk_triangle.md, "Linking
 * the game"). DXVK takes an SDL_Window * where Win32 takes an HWND. */
#include <stdint.h>

#include <CoreGraphics/CoreGraphics.h>
#include <SDL2/SDL.h>

#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>

#include "d3d9_guest.h"
#include "win32_ui.h"

void *d3d9g_hwnd(uint32_t guest_hwnd)
{
    return w32_hwnd_to_sdl(guest_hwnd);
}

void *d3d9g_backend_create9(uint32_t sdk_version)
{
    w32_ui_init(); /* DXVK's SDL2 backend crashes without SDL video */
    return Direct3DCreate9((UINT)sdk_version);
}

void d3d9g_backend_show_window(void *host_window)
{
    w32_ui_show_device_window(host_window);
}

/* Points, not pixels: on a Retina display DXVK scales the back buffer up. */
void d3d9g_backend_fit_window(void *host_window, uint32_t width, uint32_t height, int desktop)
{
    SDL_Window *w = host_window;
    if (w == NULL)
        return;
    if (desktop)
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (width != 0u && height != 0u)
        SDL_SetWindowSize(w, (int)width, (int)height);
    SDL_RaiseWindow(w);
}

typedef struct objc_object *objc_obj;
typedef struct objc_selector *objc_sel;
/* libobjc, declared here: <objc/objc.h>'s BOOL clashes with DXVK's. */
extern objc_obj objc_getClass(const char *name);
extern objc_sel sel_registerName(const char *name);
extern _Bool class_respondsToSelector(objc_obj cls, objc_sel sel);
extern void objc_msgSend(void);

/* Points a fullscreen Space keeps out of the window at the top of display
 * d: [NSScreen safeAreaInsets].top of the NSScreen whose NSScreenNumber is
 * d, plus 1. AppKit's fullscreen window on this 2056x1329-point screen is
 * 2056x1290 with an inset of 38 (measured with a native window; DXVK's
 * swap chain agrees). 0 without a notch or AppKit. */
static double notch_points(CGDirectDisplayID d)
{
    typedef struct {
        double top, left, bottom, right;
    } edge_insets;
    typedef objc_obj (*get_fn)(objc_obj, objc_sel);
    objc_obj c = objc_getClass("NSScreen");
    objc_sel inset = sel_registerName("safeAreaInsets");
    if (c == NULL || !class_respondsToSelector(c, inset))
        return 0.0;
    objc_obj all = ((get_fn)objc_msgSend)(c, sel_registerName("screens"));
    unsigned long n = all == NULL ? 0ul : ((unsigned long (*)(objc_obj, objc_sel))objc_msgSend)(all, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++) {
        objc_obj s = ((objc_obj(*)(objc_obj, objc_sel, unsigned long))objc_msgSend)(all, sel_registerName("objectAtIndex:"), i);
        objc_obj desc = ((get_fn)objc_msgSend)(s, sel_registerName("deviceDescription"));
        objc_obj num = ((objc_obj(*)(objc_obj, objc_sel, objc_obj))objc_msgSend)(desc, sel_registerName("objectForKey:"),
                                                                                 (objc_obj)CFSTR("NSScreenNumber"));
        if (num == NULL || ((unsigned (*)(objc_obj, objc_sel))objc_msgSend)(num, sel_registerName("unsignedIntValue")) != d)
            continue;
        double top = ((edge_insets(*)(objc_obj, objc_sel))objc_msgSend)(s, inset).top;
        return top > 0.0 ? top + 1.0 : 0.0;
    }
    return 0.0;
}

/* The panel's own mode (kDisplayModeNativeFlag) in pixels, 0 if none. */
static void panel_pixels(CGDirectDisplayID d, uint32_t *w, uint32_t *h)
{
    CFArrayRef all = CGDisplayCopyAllDisplayModes(d, NULL);
    *w = *h = 0u;
    for (CFIndex i = 0; all != NULL && i < CFArrayGetCount(all); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
        if ((CGDisplayModeGetIOFlags(m) & 0x02000000u) != 0u) {
            *w = (uint32_t)CGDisplayModeGetPixelWidth(m);
            *h = (uint32_t)CGDisplayModeGetPixelHeight(m);
        }
    }
    if (all != NULL)
        CFRelease(all);
}

void d3d9g_backend_screen_sizes(uint32_t *desk_w, uint32_t *desk_h, uint32_t *panel_w, uint32_t *panel_h,
                                uint32_t *refresh_hz)
{
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    double hz = 0.0, pts = 0.0, notch = notch_points(d);
    *desk_w = *desk_h = 0u;
    if (m != NULL) {
        pts = (double)CGDisplayModeGetHeight(m);
        *desk_w = (uint32_t)CGDisplayModeGetWidth(m);
        *desk_h = (uint32_t)(pts - notch + 0.5);
        hz = CGDisplayModeGetRefreshRate(m);
        CGDisplayModeRelease(m);
    }
    panel_pixels(d, panel_w, panel_h);
    if (*panel_w == 0u)
        *panel_w = *desk_w, *panel_h = *desk_h;
    else if (pts > 0.0)
        *panel_h -= (uint32_t)(notch * (double)*panel_h / pts + 0.5);
    *refresh_hz = hz >= 1.0 ? (uint32_t)(hz + 0.5) : 60u;
}
