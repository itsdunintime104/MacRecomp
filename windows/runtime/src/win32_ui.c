/* Win32 UI slice: window classes, windows, the message queue and window
 * procedure dispatch driven by SDL2 events, display queries, the cursor,
 * message boxes and three GDI32 calls (notes/slice_owners.md, slice "ui").
 * Contract: runtime/include/win32_thunk.h and docs/WIN32_THUNKS.md.
 *
 * There is one message queue. It belongs to the host main thread, which runs
 * the game's main thread and is the only thread macOS lets pump SDL events.
 * Messages Windows sends rather than posts (activation, WM_SIZE, WM_CLOSE)
 * wait in the queue marked "sent" and reach the window procedure inside the
 * next PeekMessageA, as Windows delivers them. */
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_call.h"
#include "diag.h"
#include "hud_core.h"
#include "win32_thunk.h"
#include "win32_ui.h"

/* ---- Win32 constants --------------------------------------------------- */

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCOMMAND 0x0112u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MOUSEWHEEL 0x020Au

#define SC_CLOSE 0xF060u
#define HTCLIENT 1u
#define WA_INACTIVE 0u
#define WA_ACTIVE 1u
#define SIZE_RESTORED 0u
#define SIZE_MINIMIZED 1u
#define SIZE_MAXIMIZED 2u
#define PM_REMOVE 0x0001u
#define WHEEL_DELTA 120

#define MK_LBUTTON 0x0001u
#define MK_RBUTTON 0x0002u
#define MK_SHIFT 0x0004u
#define MK_CONTROL 0x0008u
#define MK_MBUTTON 0x0010u

#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define CW_USEDEFAULT 0x80000000u

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_RESTORE 9
#define SW_FORCEMINIMIZE 11

#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_SHOWWINDOW 0x0040u
#define SWP_HIDEWINDOW 0x0080u
#define HWND_TOPMOST 0xFFFFFFFFu
#define HWND_NOTOPMOST 0xFFFFFFFEu

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_MOUSEPRESENT 19
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SM_MEDIACENTER 87

#define SPI_GETSCREENSAVEACTIVE 0x0010u
#define SPI_SETSCREENSAVEACTIVE 0x0011u
#define SPI_GETFILTERKEYS 0x0032u
#define SPI_SETFILTERKEYS 0x0033u
#define SPI_GETTOGGLEKEYS 0x0034u
#define SPI_SETTOGGLEKEYS 0x0035u
#define SPI_GETSTICKYKEYS 0x003Au
#define SPI_SETSTICKYKEYS 0x003Bu

#define MB_TYPEMASK 0x000Fu
#define MB_YESNOCANCEL 3u
#define MB_YESNO 4u
#define MB_ICONMASK 0x00F0u
#define MB_ICONERROR 0x0010u
#define MB_ICONWARNING 0x0030u
#define IDOK 1u
#define IDYES 6u

#define VK_BACK 0x08u
#define VK_TAB 0x09u
#define VK_RETURN 0x0Du
#define VK_MENU 0x12u
#define VK_ESCAPE 0x1Bu
#define VK_F10 0x79u

#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_CLASS_ALREADY_EXISTS 1410u
#define ERROR_INVALID_INDEX 1413u

/* Fixed handle values: opaque to the guest, never dereferenced. */
#define UI_HWND_BASE 0x00010010u
#define UI_HWND_STEP 0x10u
#define UI_HMONITOR 0x00020000u
#define UI_HDC 0x00030000u
#define UI_HCURSOR 0x00040000u
#define UI_HICON 0x00050000u
#define UI_HKL_EN_US 0x04090409u
#define UI_ATOM_BASE 0xC000u

/* Window frame of a non-WS_POPUP window, as Windows 10 sizes it. */
#define FRAME_SIDE 8
#define FRAME_TOP 31
#define FRAME_BOTTOM 8

/* TEB ClientId.UniqueThread: the sync slice fills it for every thread. */
#define TEB_THREAD_ID 0x24u

#define UI_MAX_CLASSES 32
#define UI_MAX_WINDOWS 16
#define UI_QUEUE_MAX 1024

/* ---- Strings ----------------------------------------------------------- */

/* Windows-1252 0x80-0x9F as Unicode; the rest of the code page is Latin-1.
 * The five unassigned bytes map to the C1 control of the same value, as
 * MultiByteToWideChar does. */
static const uint16_t k_cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* Append c to out[*n] as UTF-8 if it fits in size (keeping room for NUL). */
static void utf8_put(char *out, size_t size, size_t *n, uint32_t c)
{
    char b[4];
    size_t len;
    if (c < 0x80) {
        b[0] = (char)c, len = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), len = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F)), len = 3;
    } else {
        b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), len = 4;
    }
    if (*n + len < size) {
        memcpy(out + *n, b, len);
        *n += len;
    }
}

/* The guest Windows-1252 string at a as UTF-8 in out; NULL gives dflt. */
static const char *ansi_to_utf8(uint32_t a, const char *dflt, char *out, size_t size)
{
    if (a == 0)
        return dflt;
    size_t n = 0;
    for (const uint8_t *p = guest_ptr(a); *p != 0; p++)
        utf8_put(out, size, &n, *p >= 0x80 && *p < 0xA0 ? k_cp1252_hi[*p - 0x80] : *p);
    out[n] = '\0';
    return out;
}

/* The guest UTF-16 string at a as UTF-8 in out; NULL gives dflt. */
static const char *wide_to_utf8(uint32_t a, const char *dflt, char *out, size_t size)
{
    if (a == 0)
        return dflt;
    size_t n = 0;
    for (uint32_t c; (c = MEM16(a)) != 0; a += 2) {
        uint32_t lo = MEM16(a + 2);
        if (c >= 0xD800 && c < 0xDC00 && lo >= 0xDC00 && lo < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            a += 2;
        }
        utf8_put(out, size, &n, c);
    }
    out[n] = '\0';
    return out;
}

/* ---- SDL --------------------------------------------------------------- */

/* Ctrl+Cmd+F switches the window between fullscreen and a window through
 * SDL's own Cocoa menu item "Toggle Full Screen" (SDL_cocoaevents.m,
 * CreateApplicationMenus): SDLApplication's sendEvent: hands the key to SDL
 * and then to [super sendEvent:], which runs the menu's key equivalent, so a
 * second handler here would toggle twice. No new device or window: DXVK's
 * swap chain follows the window and scales the back buffer. Cmd+F toggles
 * the frame-time overlay (hud_core.c). The game never sees Cmd or either F:
 * this SDL event filter runs before DirectInput's event watch and the
 * message pump, and w32_ui_key_hidden masks them in DirectInput's keyboard
 * state. */
static atomic_int g_f_hidden;

int w32_ui_key_hidden(int scancode)
{
    return scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI ||
           (scancode == SDL_SCANCODE_F && atomic_load(&g_f_hidden));
}

static int SDLCALL ui_event_filter(void *ud, SDL_Event *e)
{
    (void)ud;
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP)
        return 1;
    SDL_Scancode sc = e->key.keysym.scancode;
    uint16_t mod = e->key.keysym.mod;
    if (sc == SDL_SCANCODE_F && e->type == SDL_KEYDOWN && (mod & KMOD_GUI) != 0 &&
        (mod & (KMOD_SHIFT | KMOD_ALT)) == 0) {
        if (!e->key.repeat && (mod & KMOD_CTRL) == 0)
            hud_toggle(); /* Cmd+F: the frame-time overlay (d3d9_hud.c) */
        atomic_store(&g_f_hidden, 1);
        return 0;
    }
    if (sc == SDL_SCANCODE_F && e->type == SDL_KEYUP && atomic_exchange(&g_f_hidden, 0))
        return 0;
    return !w32_ui_key_hidden((int)sc);
}

void w32_ui_init(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO))
        return;
    /* WM_CLOSE comes from the window's own close event, so closing the last
     * window must not also raise SDL_QUIT (which means Cmd+Q). */
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "win32_ui: SDL_InitSubSystem: %s\n", SDL_GetError());
        abort();
    }
    SDL_DisableScreenSaver(); /* as the engine frontend does */
    SDL_SetEventFilter(ui_event_filter, NULL);
}

/* The main display's current mode, in SDL's pixels. */
static void display_size(int *w, int *h)
{
    SDL_DisplayMode m;
    w32_ui_init();
    if (SDL_GetCurrentDisplayMode(0, &m) != 0) {
        fprintf(stderr, "win32_ui: SDL_GetCurrentDisplayMode: %s\n", SDL_GetError());
        abort();
    }
    *w = m.w;
    *h = m.h;
}

/* RECOMP_ASSUME_FOCUS=1: report focus that macOS does not give to an app
 * started by a background process (the engine frontend's convention). */
static int assume_focus(void)
{
    const char *v = getenv("RECOMP_ASSUME_FOCUS");
    return v != NULL && v[0] == '1';
}

/* ---- Classes and windows ----------------------------------------------- */

struct w32_class {
    char name[256]; /* "#<atom>" for a class registered by atom */
    uint16_t atom;
    uint32_t style, wndproc, wnd_extra, instance;
};

struct w32_window {
    uint32_t hwnd; /* 0: free slot */
    const struct w32_class *cls;
    uint32_t wndproc, style, exstyle, parent, menu, instance, userdata, tid;
    SDL_Window *sdl; /* NULL for a child window */
    int shown;       /* shown at least once */
    int surface;     /* handed to the D3D9 backend: DestroyWindow hides, never frees */
    uint8_t *extra;  /* cls->wnd_extra bytes for GetWindowLongA(index >= 0) */
};

static struct w32_class g_classes[UI_MAX_CLASSES];
static int g_class_count;
static struct w32_window g_windows[UI_MAX_WINDOWS];
static uint32_t g_window_serial;
static uint32_t g_active;              /* active window, 0 when none */
static uint32_t g_cursor = UI_HCURSOR; /* current SetCursor value */
static int32_t g_cursor_count;         /* ShowCursor display counter */
static uint32_t g_extra_window_flags;

void w32_ui_set_window_flags(uint32_t flags) { g_extra_window_flags = flags; }

/* Class name argument: a guest string, or an atom in the low word. */
static void class_key(uint32_t name, char *out, size_t size)
{
    if ((name >> 16) == 0)
        snprintf(out, size, "#%u", name);
    else
        ansi_to_utf8(name, "", out, size);
}

static const struct w32_class *class_find(uint32_t name)
{
    char key[256];
    class_key(name, key, sizeof key);
    for (int i = 0; i < g_class_count; i++)
        if (g_classes[i].atom == (uint16_t)name || strcasecmp(g_classes[i].name, key) == 0)
            return &g_classes[i];
    return NULL;
}

static struct w32_window *win_get(uint32_t hwnd)
{
    if (hwnd == 0)
        return NULL;
    for (int i = 0; i < UI_MAX_WINDOWS; i++)
        if (g_windows[i].hwnd == hwnd)
            return &g_windows[i];
    return NULL;
}

/* The window an SDL event belongs to: by SDL window ID, else the first
 * top-level window. */
static struct w32_window *event_window(uint32_t sdl_id)
{
    struct w32_window *first = NULL;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        struct w32_window *w = &g_windows[i];
        if (w->hwnd == 0 || w->sdl == NULL)
            continue;
        if (SDL_GetWindowID(w->sdl) == sdl_id)
            return w;
        if (first == NULL)
            first = w;
    }
    return first;
}

/* 1 when hwnd is ancestor or one of its descendants. */
static int is_self_or_child(uint32_t hwnd, uint32_t ancestor)
{
    for (struct w32_window *w = win_get(hwnd); w != NULL; w = win_get(w->parent))
        if (w->hwnd == ancestor)
            return 1;
    return 0;
}

static int has_frame(uint32_t style)
{
    return (style & WS_POPUP) == 0;
}

void *w32_hwnd_to_sdl(uint32_t hwnd)
{
    struct w32_window *w = win_get(hwnd);
    if (w == NULL)
        return NULL;
    w->surface = w->sdl != NULL;
    return w->sdl;
}

/* Call the window procedure of hwnd. 0 for an unknown window. */
static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    struct w32_window *w = win_get(hwnd);
    if (w == NULL)
        return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return guest_call_cdecl(w->wndproc, 4, args);
}

static uint32_t make_lparam(int lo, int hi)
{
    return (uint32_t)(uint16_t)lo | (uint32_t)(uint16_t)hi << 16;
}

/* ---- Message queue ----------------------------------------------------- */

struct qmsg {
    uint32_t hwnd, msg, wparam, lparam, time;
    int32_t x, y; /* cursor in screen coordinates when queued */
    int sent;     /* a sent message: dispatched inside PeekMessageA */
};

/* Host main thread only. */
static struct qmsg g_queue[UI_QUEUE_MAX];
static unsigned g_queue_len;
static int g_quit_pending;
static uint32_t g_quit_code;

/* Append a message. A full queue drops it, as PostMessage fails on Windows. */
static void queue_add(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, int sent)
{
    if (g_queue_len == UI_QUEUE_MAX) {
        diag_unsupported("win32_ui queue", 0, "queue full, message 0x%04X dropped", msg);
        return;
    }
    struct qmsg *q = &g_queue[g_queue_len++];
    int x = 0, y = 0;
    SDL_GetGlobalMouseState(&x, &y);
    *q = (struct qmsg){hwnd, msg, wp, lp, SDL_GetTicks(), x, y, sent};
}

static void queue_remove(unsigned i)
{
    memmove(&g_queue[i], &g_queue[i + 1], (g_queue_len - i - 1) * sizeof g_queue[0]);
    g_queue_len--;
}

/* Drop every message for a destroyed window. */
static void queue_drop(uint32_t hwnd)
{
    for (unsigned i = g_queue_len; i-- > 0;)
        if (g_queue[i].hwnd == hwnd)
            queue_remove(i);
}

/* Deliver now (a send from inside the current thunk) or at the next
 * PeekMessageA (a send from the SDL pump, which never calls guest code). */
static void deliver(int now, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (now)
        send_msg(hwnd, msg, wp, lp);
    else
        queue_add(hwnd, msg, wp, lp, 1);
}

/* Activation messages in Windows order. */
static void activation(uint32_t hwnd, int active, int now)
{
    g_active = active ? hwnd : 0;
    if (active) {
        deliver(now, hwnd, WM_ACTIVATEAPP, W32_TRUE, 0);
        deliver(now, hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        deliver(now, hwnd, WM_SETFOCUS, 0, 0);
    } else {
        deliver(now, hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        deliver(now, hwnd, WM_ACTIVATEAPP, W32_FALSE, 0);
        deliver(now, hwnd, WM_KILLFOCUS, 0, 0);
    }
}

/* ---- SDL event translation --------------------------------------------- */

/* Windows virtual-key code and PC/AT set-1 scan code per SDL scancode;
 * 0x100 in scan marks an extended key. vk 0: not delivered. */
#define EXT 0x100u
static const struct {
    uint8_t vk;
    uint16_t scan;
} k_keys[SDL_SCANCODE_RGUI + 1] = {
    [SDL_SCANCODE_A] = {'A', 0x1E}, [SDL_SCANCODE_B] = {'B', 0x30}, [SDL_SCANCODE_C] = {'C', 0x2E},
    [SDL_SCANCODE_D] = {'D', 0x20}, [SDL_SCANCODE_E] = {'E', 0x12}, [SDL_SCANCODE_F] = {'F', 0x21},
    [SDL_SCANCODE_G] = {'G', 0x22}, [SDL_SCANCODE_H] = {'H', 0x23}, [SDL_SCANCODE_I] = {'I', 0x17},
    [SDL_SCANCODE_J] = {'J', 0x24}, [SDL_SCANCODE_K] = {'K', 0x25}, [SDL_SCANCODE_L] = {'L', 0x26},
    [SDL_SCANCODE_M] = {'M', 0x32}, [SDL_SCANCODE_N] = {'N', 0x31}, [SDL_SCANCODE_O] = {'O', 0x18},
    [SDL_SCANCODE_P] = {'P', 0x19}, [SDL_SCANCODE_Q] = {'Q', 0x10}, [SDL_SCANCODE_R] = {'R', 0x13},
    [SDL_SCANCODE_S] = {'S', 0x1F}, [SDL_SCANCODE_T] = {'T', 0x14}, [SDL_SCANCODE_U] = {'U', 0x16},
    [SDL_SCANCODE_V] = {'V', 0x2F}, [SDL_SCANCODE_W] = {'W', 0x11}, [SDL_SCANCODE_X] = {'X', 0x2D},
    [SDL_SCANCODE_Y] = {'Y', 0x15}, [SDL_SCANCODE_Z] = {'Z', 0x2C},
    [SDL_SCANCODE_1] = {'1', 0x02}, [SDL_SCANCODE_2] = {'2', 0x03}, [SDL_SCANCODE_3] = {'3', 0x04},
    [SDL_SCANCODE_4] = {'4', 0x05}, [SDL_SCANCODE_5] = {'5', 0x06}, [SDL_SCANCODE_6] = {'6', 0x07},
    [SDL_SCANCODE_7] = {'7', 0x08}, [SDL_SCANCODE_8] = {'8', 0x09}, [SDL_SCANCODE_9] = {'9', 0x0A},
    [SDL_SCANCODE_0] = {'0', 0x0B},
    [SDL_SCANCODE_RETURN] = {VK_RETURN, 0x1C}, [SDL_SCANCODE_ESCAPE] = {VK_ESCAPE, 0x01},
    [SDL_SCANCODE_BACKSPACE] = {VK_BACK, 0x0E}, [SDL_SCANCODE_TAB] = {VK_TAB, 0x0F},
    [SDL_SCANCODE_SPACE] = {0x20, 0x39},
    [SDL_SCANCODE_MINUS] = {0xBD, 0x0C}, [SDL_SCANCODE_EQUALS] = {0xBB, 0x0D},
    [SDL_SCANCODE_LEFTBRACKET] = {0xDB, 0x1A}, [SDL_SCANCODE_RIGHTBRACKET] = {0xDD, 0x1B},
    [SDL_SCANCODE_BACKSLASH] = {0xDC, 0x2B}, [SDL_SCANCODE_NONUSHASH] = {0xDC, 0x2B},
    [SDL_SCANCODE_SEMICOLON] = {0xBA, 0x27}, [SDL_SCANCODE_APOSTROPHE] = {0xDE, 0x28},
    [SDL_SCANCODE_GRAVE] = {0xC0, 0x29}, [SDL_SCANCODE_COMMA] = {0xBC, 0x33},
    [SDL_SCANCODE_PERIOD] = {0xBE, 0x34}, [SDL_SCANCODE_SLASH] = {0xBF, 0x35},
    [SDL_SCANCODE_CAPSLOCK] = {0x14, 0x3A},
    [SDL_SCANCODE_F1] = {0x70, 0x3B}, [SDL_SCANCODE_F2] = {0x71, 0x3C}, [SDL_SCANCODE_F3] = {0x72, 0x3D},
    [SDL_SCANCODE_F4] = {0x73, 0x3E}, [SDL_SCANCODE_F5] = {0x74, 0x3F}, [SDL_SCANCODE_F6] = {0x75, 0x40},
    [SDL_SCANCODE_F7] = {0x76, 0x41}, [SDL_SCANCODE_F8] = {0x77, 0x42}, [SDL_SCANCODE_F9] = {0x78, 0x43},
    [SDL_SCANCODE_F10] = {VK_F10, 0x44}, [SDL_SCANCODE_F11] = {0x7A, 0x57},
    [SDL_SCANCODE_F12] = {0x7B, 0x58},
    [SDL_SCANCODE_PRINTSCREEN] = {0x2C, EXT | 0x37}, [SDL_SCANCODE_SCROLLLOCK] = {0x91, 0x46},
    [SDL_SCANCODE_PAUSE] = {0x13, 0x45}, [SDL_SCANCODE_INSERT] = {0x2D, EXT | 0x52},
    [SDL_SCANCODE_HOME] = {0x24, EXT | 0x47}, [SDL_SCANCODE_PAGEUP] = {0x21, EXT | 0x49},
    [SDL_SCANCODE_DELETE] = {0x2E, EXT | 0x53}, [SDL_SCANCODE_END] = {0x23, EXT | 0x4F},
    [SDL_SCANCODE_PAGEDOWN] = {0x22, EXT | 0x51}, [SDL_SCANCODE_RIGHT] = {0x27, EXT | 0x4D},
    [SDL_SCANCODE_LEFT] = {0x25, EXT | 0x4B}, [SDL_SCANCODE_DOWN] = {0x28, EXT | 0x50},
    [SDL_SCANCODE_UP] = {0x26, EXT | 0x48},
    [SDL_SCANCODE_NUMLOCKCLEAR] = {0x90, EXT | 0x45}, [SDL_SCANCODE_KP_DIVIDE] = {0x6F, EXT | 0x35},
    [SDL_SCANCODE_KP_MULTIPLY] = {0x6A, 0x37}, [SDL_SCANCODE_KP_MINUS] = {0x6D, 0x4A},
    [SDL_SCANCODE_KP_PLUS] = {0x6B, 0x4E}, [SDL_SCANCODE_KP_ENTER] = {VK_RETURN, EXT | 0x1C},
    [SDL_SCANCODE_KP_1] = {0x61, 0x4F}, [SDL_SCANCODE_KP_2] = {0x62, 0x50},
    [SDL_SCANCODE_KP_3] = {0x63, 0x51}, [SDL_SCANCODE_KP_4] = {0x64, 0x4B},
    [SDL_SCANCODE_KP_5] = {0x65, 0x4C}, [SDL_SCANCODE_KP_6] = {0x66, 0x4D},
    [SDL_SCANCODE_KP_7] = {0x67, 0x47}, [SDL_SCANCODE_KP_8] = {0x68, 0x48},
    [SDL_SCANCODE_KP_9] = {0x69, 0x49}, [SDL_SCANCODE_KP_0] = {0x60, 0x52},
    [SDL_SCANCODE_KP_PERIOD] = {0x6E, 0x53},
    [SDL_SCANCODE_NONUSBACKSLASH] = {0xE2, 0x56}, [SDL_SCANCODE_APPLICATION] = {0x5D, EXT | 0x5D},
    [SDL_SCANCODE_LCTRL] = {0x11, 0x1D}, [SDL_SCANCODE_LSHIFT] = {0x10, 0x2A},
    [SDL_SCANCODE_LALT] = {VK_MENU, 0x38}, [SDL_SCANCODE_LGUI] = {0x5B, EXT | 0x5B},
    [SDL_SCANCODE_RCTRL] = {0x11, EXT | 0x1D}, [SDL_SCANCODE_RSHIFT] = {0x10, 0x36},
    [SDL_SCANCODE_RALT] = {VK_MENU, EXT | 0x38}, [SDL_SCANCODE_RGUI] = {0x5C, EXT | 0x5C},
};

/* MapVirtualKeyEx on the US layout (kbdus): k_keys, plus the scan codes
 * whose base virtual key differs from what k_keys delivers: left/right
 * modifiers for MAPVK_VSC_TO_VK_EX, and the numeric keypad, whose keys map
 * to their navigation keys (NumLock is applied later, on Windows too). */
#define MAPVK_VK_TO_VSC 0u
#define MAPVK_VSC_TO_VK 1u
#define MAPVK_VSC_TO_VK_EX 3u
#define MAPVK_VK_TO_VSC_EX 4u
static const struct {
    uint16_t scan;
    uint8_t vk;
} k_sided[] = {
    {0x2A, 0xA0}, {0x36, 0xA1}, {0x1D, 0xA2}, {EXT | 0x1D, 0xA3}, {0x38, 0xA4}, {EXT | 0x38, 0xA5},
}, k_keypad[] = {
    {0x45, 0x90}, {0x47, 0x24}, {0x48, 0x26}, {0x49, 0x21}, {0x4B, 0x25}, {0x4C, 0x0C},
    {0x4D, 0x27}, {0x4F, 0x23}, {0x50, 0x28}, {0x51, 0x22}, {0x52, 0x2D}, {0x53, 0x2E},
};
#define N_OF(a) (sizeof(a) / sizeof((a)[0]))

/* 0 when the code has no mapping. */
static uint32_t map_virtual_key(uint32_t code, uint32_t type)
{
    if (type == MAPVK_VK_TO_VSC || type == MAPVK_VK_TO_VSC_EX) {
        uint32_t scan = 0;
        for (size_t i = 0; i < N_OF(k_sided) && scan == 0; i++)
            if (k_sided[i].vk == code)
                scan = k_sided[i].scan;
        for (unsigned sc = 0; sc <= SDL_SCANCODE_RGUI && scan == 0; sc++)
            if (code != 0 && k_keys[sc].vk == code)
                scan = k_keys[sc].scan;
        if (type == MAPVK_VK_TO_VSC_EX && (scan & EXT))
            return 0xE000u | (scan & 0xFFu);
        return scan & 0xFFu;
    }
    /* MAPVK_VSC_TO_VK(_EX): a set-1 code, 0xE0 in the high byte for an
     * extended key. */
    if ((code >> 8) != 0 && (code >> 8) != 0xE0u)
        return 0;
    uint32_t scan = (code & 0xFFu) | ((code >> 8) == 0xE0u ? EXT : 0u);
    if (type == MAPVK_VSC_TO_VK_EX)
        for (size_t i = 0; i < N_OF(k_sided); i++)
            if (k_sided[i].scan == scan)
                return k_sided[i].vk;
    for (size_t i = 0; i < N_OF(k_keypad); i++)
        if (k_keypad[i].scan == scan)
            return k_keypad[i].vk;
    for (unsigned sc = 0; sc <= SDL_SCANCODE_RGUI; sc++)
        if (k_keys[sc].vk != 0 && k_keys[sc].scan == scan)
            return k_keys[sc].vk;
    return 0;
}

/* UINT MapVirtualKeyExA(UINT code, UINT type, HKL): the US layout whatever
 * the HKL. The game uses MAPVK_VSC_TO_VK_EX only. */
static void imp_MapVirtualKeyExA(void)
{
    uint32_t code = W32_ARG(0), type = W32_ARG(1), r = 0;
    if (type == MAPVK_VK_TO_VSC || type == MAPVK_VSC_TO_VK || type == MAPVK_VSC_TO_VK_EX ||
        type == MAPVK_VK_TO_VSC_EX)
        r = map_virtual_key(code, type);
    else
        diag_unsupported("USER32.dll!MapVirtualKeyExA", MEM32(g_esp), "map type %u: returns 0",
                         type);
    W32_RETURN(3, r);
}

static uint32_t g_buttons; /* MK_LBUTTON | MK_RBUTTON | MK_MBUTTON held */

static uint32_t mk_state(void)
{
    uint32_t mk = g_buttons;
    SDL_Keymod mod = SDL_GetModState();
    if (mod & KMOD_SHIFT)
        mk |= MK_SHIFT;
    if (mod & KMOD_CTRL)
        mk |= MK_CONTROL;
    return mk;
}

/* The character TranslateMessage posts for each virtual key, recorded at its
 * last key-down: the layout's character for the scancode, shifted the US way,
 * Ctrl+letter as a control code; 0 for none. */
static uint8_t g_key_char[256];

static uint8_t key_char(SDL_Scancode sc, uint16_t mod)
{
    static const char k_plain[] = "1234567890-=[]\\;',./`", k_shift[] = "!@#$%^&*()_+{}|:\"<>?~";
    SDL_Keycode sym = SDL_GetKeyFromScancode(sc);
    if (sym == SDLK_RETURN || sym == SDLK_BACKSPACE || sym == SDLK_TAB || sym == SDLK_ESCAPE)
        return (uint8_t)sym;
    if (sym < 0x20 || sym > 0x7E)
        return 0;
    int shift = (mod & KMOD_SHIFT) != 0;
    if (sym >= 'a' && sym <= 'z') {
        if ((mod & KMOD_CTRL) != 0)
            return (uint8_t)(sym & 0x1F);
        return (uint8_t)(shift != ((mod & KMOD_CAPS) != 0) ? sym - 0x20 : sym);
    }
    const char *at = shift ? strchr(k_plain, (int)sym) : NULL;
    return at != NULL ? (uint8_t)k_shift[at - k_plain] : (uint8_t)sym;
}

static void pump_key(const SDL_KeyboardEvent *k)
{
    struct w32_window *w = event_window(k->windowID);
    SDL_Scancode sc = k->keysym.scancode;
    if (w == NULL || (unsigned)sc > SDL_SCANCODE_RGUI || k_keys[sc].vk == 0)
        return;
    uint32_t vk = k_keys[sc].vk, scan = k_keys[sc].scan;
    int down = k->type == SDL_KEYDOWN;
    int alt = (k->keysym.mod & KMOD_ALT) != 0 || vk == VK_MENU;
    /* Repeat count 1, scan code, extended bit, context (Alt) bit, previous
     * state and transition bits. */
    uint32_t lp = 1u | (scan & 0xFFu) << 16 | (scan & EXT ? 1u << 24 : 0) | (alt ? 1u << 29 : 0);
    if (!down)
        lp |= 3u << 30;
    else if (k->repeat)
        lp |= 1u << 30;
    uint32_t msg = alt || vk == VK_F10 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                       : (down ? WM_KEYDOWN : WM_KEYUP);
    if (down)
        g_key_char[vk & 0xFFu] = key_char(sc, k->keysym.mod);
    queue_add(w->hwnd, msg, vk, lp, 0);
}

/* WM_SETCURSOR, sent before each mouse message as Windows does: the game
 * hides the cursor from its handler. */
static void pump_set_cursor(uint32_t hwnd, uint32_t msg)
{
    queue_add(hwnd, WM_SETCURSOR, hwnd, HTCLIENT | msg << 16, 1);
}

static void pump_mouse_button(const SDL_MouseButtonEvent *b)
{
    struct w32_window *w = event_window(b->windowID);
    uint32_t mk, msg;
    int down = b->type == SDL_MOUSEBUTTONDOWN;
    switch (b->button) {
    case SDL_BUTTON_LEFT:
        mk = MK_LBUTTON, msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        break;
    case SDL_BUTTON_RIGHT:
        mk = MK_RBUTTON, msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        break;
    case SDL_BUTTON_MIDDLE:
        mk = MK_MBUTTON, msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        break;
    default:
        return;
    }
    g_buttons = down ? g_buttons | mk : g_buttons & ~mk;
    if (w == NULL)
        return;
    pump_set_cursor(w->hwnd, msg);
    queue_add(w->hwnd, msg, mk_state(), make_lparam(b->x, b->y), 0);
}

static void pump_wheel(const SDL_MouseWheelEvent *e)
{
    struct w32_window *w = event_window(e->windowID);
    float y = e->direction == SDL_MOUSEWHEEL_FLIPPED ? -e->preciseY : e->preciseY;
    int delta = (int)lroundf(y * WHEEL_DELTA);
    if (w == NULL || delta == 0)
        return;
    /* WM_MOUSEWHEEL carries screen coordinates. */
    int x = 0, sy = 0;
    SDL_GetGlobalMouseState(&x, &sy);
    queue_add(w->hwnd, WM_MOUSEWHEEL, (uint32_t)(uint16_t)delta << 16 | mk_state(),
              make_lparam(x, sy), 0);
}

static void pump_window_event(const SDL_WindowEvent *e)
{
    struct w32_window *w = event_window(e->windowID);
    if (w == NULL || SDL_GetWindowID(w->sdl) != e->windowID)
        return;
    int cw, ch;
    switch (e->event) {
    case SDL_WINDOWEVENT_FOCUS_GAINED:
    case SDL_WINDOWEVENT_FOCUS_LOST:
        if (!assume_focus())
            activation(w->hwnd, e->event == SDL_WINDOWEVENT_FOCUS_GAINED, 0);
        break;
    case SDL_WINDOWEVENT_CLOSE:
        queue_add(w->hwnd, WM_CLOSE, 0, 0, 1);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        queue_add(w->hwnd, WM_SIZE, SIZE_RESTORED, make_lparam(e->data1, e->data2), 1);
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        queue_add(w->hwnd, WM_SIZE, SIZE_MINIMIZED, 0, 1);
        break;
    case SDL_WINDOWEVENT_MAXIMIZED:
    case SDL_WINDOWEVENT_RESTORED:
        SDL_GetWindowSize(w->sdl, &cw, &ch);
        queue_add(w->hwnd, WM_SIZE,
                  e->event == SDL_WINDOWEVENT_MAXIMIZED ? SIZE_MAXIMIZED : SIZE_RESTORED,
                  make_lparam(cw, ch), 1);
        break;
    default:
        break;
    }
}

void w32_ui_pump(void)
{
    if (!pthread_main_np() || !SDL_WasInit(SDL_INIT_VIDEO))
        return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        struct w32_window *w;
        switch (ev.type) {
        case SDL_QUIT: /* Cmd+Q */
            w = event_window(0);
            if (w != NULL)
                queue_add(w->hwnd, WM_CLOSE, 0, 0, 1);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            pump_key(&ev.key);
            break;
        case SDL_MOUSEMOTION:
            w = event_window(ev.motion.windowID);
            if (w != NULL) {
                pump_set_cursor(w->hwnd, WM_MOUSEMOVE);
                queue_add(w->hwnd, WM_MOUSEMOVE, mk_state(),
                          make_lparam(ev.motion.x, ev.motion.y), 0);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            pump_mouse_button(&ev.button);
            break;
        case SDL_MOUSEWHEEL:
            pump_wheel(&ev.wheel);
            break;
        case SDL_WINDOWEVENT:
            pump_window_event(&ev.window);
            break;
        default:
            break;
        }
    }
}

/* ---- Window operations ------------------------------------------------- */

static uint32_t destroy_window(uint32_t hwnd)
{
    struct w32_window *w = win_get(hwnd);
    if (w == NULL) {
        w32_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return W32_FALSE;
    }
    for (int i = 0; i < UI_MAX_WINDOWS; i++)
        if (g_windows[i].hwnd != 0 && g_windows[i].parent == hwnd)
            destroy_window(g_windows[i].hwnd);
    if (g_active == hwnd)
        g_active = 0;
    send_msg(hwnd, WM_DESTROY, 0, 0);
    send_msg(hwnd, WM_NCDESTROY, 0, 0);
    queue_drop(hwnd);
    /* DXVK's Vulkan surface keeps an unretained pointer to the window's
     * CAMetalLayer (Mesa wsi_common_metal.c, surface->pLayer). The game
     * destroys its window before it releases the device, so freeing it
     * here lets the DXVK submit thread present into a freed layer
     * (notes/present_crash.md). Such a window is hidden and lives to exit. */
    if (w->sdl != NULL && w->surface)
        SDL_HideWindow(w->sdl);
    else if (w->sdl != NULL)
        SDL_DestroyWindow(w->sdl);
    free(w->extra);
    memset(w, 0, sizeof *w);
    return W32_TRUE;
}

/* ShowWindow. Returns 1 if the window was visible before. The first show of
 * a top-level window activates it under RECOMP_ASSUME_FOCUS=1. */
static int show_window(struct w32_window *w, int32_t cmd)
{
    int was_visible = (w->style & WS_VISIBLE) != 0;
    if (cmd == SW_HIDE) {
        w->style &= ~WS_VISIBLE;
        if (w->sdl != NULL)
            SDL_HideWindow(w->sdl);
        return was_visible;
    }
    w->style |= WS_VISIBLE;
    if (w->sdl == NULL)
        return was_visible;
    SDL_ShowWindow(w->sdl);
    switch (cmd) {
    case SW_SHOWMINIMIZED:
    case SW_MINIMIZE:
    case SW_SHOWMINNOACTIVE:
    case SW_FORCEMINIMIZE:
        SDL_MinimizeWindow(w->sdl);
        break;
    case SW_SHOWMAXIMIZED:
        SDL_MaximizeWindow(w->sdl);
        SDL_RaiseWindow(w->sdl);
        break;
    case SW_RESTORE:
        SDL_RestoreWindow(w->sdl);
        SDL_RaiseWindow(w->sdl);
        break;
    default:
        SDL_RaiseWindow(w->sdl);
        break;
    }
    if (!w->shown) {
        w->shown = 1;
        if (assume_focus())
            activation(w->hwnd, 1, 1);
    }
    return was_visible;
}

void w32_ui_show_device_window(void *host_window)
{
    for (int i = 0; host_window != NULL && i < UI_MAX_WINDOWS; i++)
        if (g_windows[i].hwnd != 0 && g_windows[i].sdl == host_window) {
            show_window(&g_windows[i], SW_SHOW);
            return;
        }
}

/* Place a top-level window from an outer rectangle (frame included). */
static void place_window(struct w32_window *w, int x, int y, int cx, int cy, int move, int size)
{
    if (w->sdl == NULL)
        return;
    int f = has_frame(w->style);
    if (move)
        SDL_SetWindowPosition(w->sdl, x + (f ? FRAME_SIDE : 0), y + (f ? FRAME_TOP : 0));
    if (size) {
        int cw = cx - (f ? 2 * FRAME_SIDE : 0), ch = cy - (f ? FRAME_TOP + FRAME_BOTTOM : 0);
        SDL_SetWindowSize(w->sdl, cw > 1 ? cw : 1, ch > 1 ? ch : 1);
    }
}

/* CreateWindowExA with its 12 arguments. Returns the HWND or 0. */
static uint32_t create_window(const uint32_t a[12])
{
    uint32_t exstyle = a[0], style = a[3], parent = a[8];
    const struct w32_class *cls = class_find(a[1]);
    if (cls == NULL) {
        w32_set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
        return 0;
    }
    struct w32_window *w = NULL;
    for (int i = 0; i < UI_MAX_WINDOWS && w == NULL; i++)
        if (g_windows[i].hwnd == 0)
            w = &g_windows[i];
    if (w == NULL) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    int f = has_frame(style);
    int x = (int32_t)a[4], y = (int32_t)a[5], cx = (int32_t)a[6], cy = (int32_t)a[7];
    if (a[6] == CW_USEDEFAULT) {
        cx = 640 + (f ? 2 * FRAME_SIDE : 0);
        cy = 480 + (f ? FRAME_TOP + FRAME_BOTTOM : 0);
    }
    SDL_Window *sdl = NULL;
    if ((style & WS_CHILD) == 0) {
        w32_ui_init();
        char title[512];
        int cw = cx - (f ? 2 * FRAME_SIDE : 0), ch = cy - (f ? FRAME_TOP + FRAME_BOTTOM : 0);
        int px = a[4] == CW_USEDEFAULT ? (int)SDL_WINDOWPOS_CENTERED : x + (f ? FRAME_SIDE : 0);
        int py = a[4] == CW_USEDEFAULT ? (int)SDL_WINDOWPOS_CENTERED : y + (f ? FRAME_TOP : 0);
        sdl = SDL_CreateWindow(ansi_to_utf8(a[2], "", title, sizeof title), px, py,
                               cw > 1 ? cw : 1, ch > 1 ? ch : 1,
                               SDL_WINDOW_HIDDEN | g_extra_window_flags);
        if (sdl == NULL) {
            fprintf(stderr, "win32_ui: SDL_CreateWindow: %s\n", SDL_GetError());
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        /* sdl2-compat turns SDL text input on at SDL_Init; on macOS it shows
         * the press-and-hold accent menu for a held game key. Keep it off:
         * WM_CHAR comes from TranslateMessage. */
        SDL_StopTextInput();
        SDL_GetWindowPosition(sdl, &x, &y);
        x -= f ? FRAME_SIDE : 0;
        y -= f ? FRAME_TOP : 0;
    } else if (a[4] == CW_USEDEFAULT) {
        x = y = 0;
    }

    *w = (struct w32_window){
        .hwnd = UI_HWND_BASE + UI_HWND_STEP * g_window_serial++,
        .cls = cls,
        .wndproc = cls->wndproc,
        .style = style & ~WS_VISIBLE,
        .exstyle = exstyle,
        .parent = parent,
        .menu = a[9],
        .instance = a[10],
        .tid = MEM32(g_fs_base + TEB_THREAD_ID),
        .sdl = sdl,
        .extra = calloc(1, cls->wnd_extra > 0 ? cls->wnd_extra : 1),
    };
    uint32_t hwnd = w->hwnd;

    /* CREATESTRUCTA on the guest stack below this thunk's frame. */
    uint32_t saved_esp = g_esp;
    g_esp = (g_esp - 48u) & ~15u;
    uint32_t cs = g_esp;
    const uint32_t fields[12] = {a[11], a[10], a[9], a[8], (uint32_t)cy, (uint32_t)cx,
                                 (uint32_t)y, (uint32_t)x, style, a[2], a[1], exstyle};
    for (int i = 0; i < 12; i++)
        MEM32(cs + 4u * (uint32_t)i) = fields[i];
    int ok = send_msg(hwnd, WM_NCCREATE, 0, cs) != 0;
    if (ok && (int32_t)send_msg(hwnd, WM_CREATE, 0, cs) == -1)
        ok = 0;
    g_esp = saved_esp;
    if (!ok) {
        destroy_window(hwnd);
        return 0;
    }

    /* Windows sends WM_SIZE and WM_MOVE with the client area after WM_CREATE. */
    int cw = cx - (f ? 2 * FRAME_SIDE : 0), ch = cy - (f ? FRAME_TOP + FRAME_BOTTOM : 0);
    send_msg(hwnd, WM_SIZE, SIZE_RESTORED, make_lparam(cw, ch));
    send_msg(hwnd, WM_MOVE, 0, make_lparam(x + (f ? FRAME_SIDE : 0), y + (f ? FRAME_TOP : 0)));
    w = win_get(hwnd);
    if (w != NULL && (style & WS_VISIBLE))
        show_window(w, SW_SHOWNORMAL);
    return hwnd;
}

/* PeekMessageA after its arguments are read. */
static uint32_t peek_message(uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    if (!pthread_main_np())
        return W32_FALSE; /* a thread with no window has an empty queue */
    w32_ui_pump();
    for (unsigned i = 0; i < g_queue_len;) {
        if (!g_queue[i].sent) {
            i++;
            continue;
        }
        struct qmsg m = g_queue[i];
        queue_remove(i);
        send_msg(m.hwnd, m.msg, m.wparam, m.lparam);
        i = 0; /* the procedure may have changed the queue */
    }
    int any_range = lo == 0 && hi == 0;
    struct qmsg m = {0};
    int found = 0;
    for (unsigned i = 0; i < g_queue_len && !found; i++) {
        struct qmsg *q = &g_queue[i];
        int hwnd_ok = hwnd == 0 || (hwnd == 0xFFFFFFFFu ? q->hwnd == 0 : is_self_or_child(q->hwnd, hwnd));
        if (hwnd_ok && (any_range || (q->msg >= lo && q->msg <= hi))) {
            m = *q;
            found = 1;
            if (remove)
                queue_remove(i);
        }
    }
    /* WM_QUIT is a flag that is reported once the queue holds nothing else. */
    if (!found && g_quit_pending && (any_range || (WM_QUIT >= lo && WM_QUIT <= hi))) {
        m = (struct qmsg){0, WM_QUIT, g_quit_code, 0, SDL_GetTicks(), 0, 0, 0};
        found = 1;
        if (remove)
            g_quit_pending = 0;
    }
    if (!found)
        return W32_FALSE;
    const uint32_t msg[7] = {m.hwnd, m.msg, m.wparam, m.lparam, m.time,
                             (uint32_t)m.x, (uint32_t)m.y};
    for (int i = 0; i < 7; i++)
        MEM32(out + 4u * (uint32_t)i) = msg[i];
    return W32_TRUE;
}

static uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wp)
{
    switch (msg) {
    case WM_NCCREATE:
        return W32_TRUE;
    case WM_CLOSE:
        destroy_window(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0u) == SC_CLOSE)
            send_msg(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return 0;
    }
}

static uint32_t message_box(const char *api, uint32_t hwnd, const char *text,
                            const char *caption, uint32_t type)
{
    fprintf(stderr, "%s: %s: %s\n", api, caption, text);
    const char *v = getenv("RECOMP_NO_ALERT_UI");
    if (v == NULL || strcmp(v, "1") != 0) {
        uint32_t icon = type & MB_ICONMASK;
        uint32_t flags = icon == MB_ICONERROR     ? SDL_MESSAGEBOX_ERROR
                         : icon == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING
                                                  : SDL_MESSAGEBOX_INFORMATION;
        struct w32_window *w = win_get(hwnd);
        w32_ui_init();
        SDL_ShowSimpleMessageBox(flags, caption, text,
                                 w != NULL && (w->style & WS_VISIBLE) ? w->sdl : NULL);
    }
    uint32_t buttons = type & MB_TYPEMASK;
    return buttons == MB_YESNO || buttons == MB_YESNOCANCEL ? IDYES : IDOK;
}

/* ---- USER32 imports ---------------------------------------------------- */

/* BOOL AdjustWindowRect(LPRECT, DWORD style, BOOL menu) */
static void imp_AdjustWindowRect(void)
{
    uint32_t rc = W32_ARG(0), style = W32_ARG(1);
    if (rc != 0 && has_frame(style)) {
        MEM32(rc + 0) -= FRAME_SIDE;
        MEM32(rc + 4) -= FRAME_TOP;
        MEM32(rc + 8) += FRAME_SIDE;
        MEM32(rc + 12) += FRAME_BOTTOM;
    }
    W32_RETURN(3, rc != 0 ? W32_TRUE : W32_FALSE);
}

/* HWND CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR title, DWORD style,
 *     int x, int y, int w, int h, HWND parent, HMENU, HINSTANCE, LPVOID) */
static void imp_CreateWindowExA(void)
{
    uint32_t a[12];
    for (int i = 0; i < 12; i++)
        a[i] = W32_ARG(i);
    uint32_t hwnd = create_window(a);
    W32_RETURN(12, hwnd);
}

/* LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) */
static void imp_DefWindowProcA(void)
{
    uint32_t r = def_window_proc(W32_ARG(0), W32_ARG(1), W32_ARG(2));
    W32_RETURN(4, r);
}

/* BOOL DestroyWindow(HWND) */
static void imp_DestroyWindow(void)
{
    uint32_t r = destroy_window(W32_ARG(0));
    W32_RETURN(1, r);
}

/* LRESULT DispatchMessageA(const MSG *) */
static void imp_DispatchMessageA(void)
{
    uint32_t m = W32_ARG(0);
    uint32_t r = m != 0 ? send_msg(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12)) : 0;
    W32_RETURN(1, r);
}

/* BOOL EnumDisplayMonitors(HDC, LPCRECT clip, MONITORENUMPROC, LPARAM):
 * one monitor, the main display. */
static void imp_EnumDisplayMonitors(void)
{
    uint32_t hdc = W32_ARG(0), proc = W32_ARG(2), data = W32_ARG(3);
    int w, h;
    display_size(&w, &h);
    uint32_t saved_esp = g_esp;
    g_esp = (g_esp - 16u) & ~15u; /* the RECT, below this thunk's frame */
    uint32_t rc = g_esp;
    MEM32(rc + 0) = 0;
    MEM32(rc + 4) = 0;
    MEM32(rc + 8) = (uint32_t)w;
    MEM32(rc + 12) = (uint32_t)h;
    uint32_t args[4] = {UI_HMONITOR, hdc, rc, data};
    guest_call_cdecl(proc, 4, args);
    g_esp = saved_esp;
    W32_RETURN(4, W32_TRUE);
}

/* BOOL EnumDisplayDevicesA(LPCSTR device, DWORD index, DISPLAY_DEVICEA *, DWORD flags):
 * one adapter, the primary display. DeviceString and DeviceID name the GPU
 * DXVK reports when it hides a non-classical vendor (AMD Radeon RX 6700 XT,
 * 1002:73DF), so this answer and the D3D9 adapter identifier agree. Monitor
 * queries (device != NULL) find no device. */
#define UI_DD_SIZE 424u /* cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128] */
static void imp_EnumDisplayDevicesA(void)
{
    uint32_t device = W32_ARG(0), index = W32_ARG(1), dd = W32_ARG(2);
    int found = device == 0u && index == 0u && dd != 0u && MEM32(dd) >= UI_DD_SIZE;
    if (found) {
        char *p = guest_ptr(dd);
        memset(p + 4, 0, UI_DD_SIZE - 4u);
        strcpy(p + 4, "\\\\.\\DISPLAY1");
        strcpy(p + 36, "AMD Radeon RX 6700 XT");
        MEM32(dd + 164) = 0x1u | 0x4u; /* DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
        strcpy(p + 168, "PCI\\VEN_1002&DEV_73DF");
    }
    W32_RETURN(4, found ? W32_TRUE : W32_FALSE);
}

/* HWND GetForegroundWindow(void) */
static void imp_GetForegroundWindow(void)
{
    W32_RETURN(0, g_active);
}

/* HKL GetKeyboardLayout(DWORD thread) */
static void imp_GetKeyboardLayout(void)
{
    W32_RETURN(1, UI_HKL_EN_US);
}

/* HKL LoadKeyboardLayoutA(LPCSTR klid, UINT flags): the US layout, the only
 * one the key table knows. */
static void imp_LoadKeyboardLayoutA(void)
{
    W32_RETURN(2, UI_HKL_EN_US);
}

/* int GetSystemMetrics(int) */
static void imp_GetSystemMetrics(void)
{
    int32_t index = (int32_t)W32_ARG(0);
    int w, h;
    uint32_t v = 0;
    switch (index) {
    case SM_CXSCREEN:
    case SM_CXVIRTUALSCREEN:
        display_size(&w, &h);
        v = (uint32_t)w;
        break;
    case SM_CYSCREEN:
    case SM_CYVIRTUALSCREEN:
        display_size(&w, &h);
        v = (uint32_t)h;
        break;
    case SM_MEDIACENTER: /* not a Media Center PC */
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
        v = 0;
        break;
    case SM_CMONITORS:
    case SM_MOUSEPRESENT:
        v = 1;
        break;
    default:
        THUNK_UNSUPPORTED("index %d", index);
        break;
    }
    W32_RETURN(1, v);
}

/* LONG GetWindowLongA(HWND, int index) */
static void imp_GetWindowLongA(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    int32_t index = (int32_t)W32_ARG(1);
    uint32_t v = 0;
    if (w == NULL) {
        w32_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    } else {
        switch (index) {
        case GWL_WNDPROC:
            v = w->wndproc;
            break;
        case GWL_HINSTANCE:
            v = w->instance;
            break;
        case GWL_HWNDPARENT:
            v = w->parent;
            break;
        case GWL_ID:
            v = w->menu;
            break;
        case GWL_STYLE:
            v = w->style;
            break;
        case GWL_EXSTYLE:
            v = w->exstyle;
            break;
        case GWL_USERDATA:
            v = w->userdata;
            break;
        default:
            if (index >= 0 && (uint32_t)index + 4u <= w->cls->wnd_extra)
                memcpy(&v, w->extra + index, 4);
            else
                w32_set_last_error(ERROR_INVALID_INDEX);
            break;
        }
    }
    W32_RETURN(2, v);
}

/* DWORD GetWindowThreadProcessId(HWND, LPDWORD process) */
static void imp_GetWindowThreadProcessId(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    uint32_t pid = W32_ARG(1), tid = 0;
    if (w == NULL) {
        w32_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    } else {
        tid = w->tid;
        if (pid != 0)
            MEM32(pid) = (uint32_t)getpid();
    }
    W32_RETURN(2, tid);
}

/* BOOL IsIconic(HWND) */
static void imp_IsIconic(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    int min = w != NULL && w->sdl != NULL && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED);
    W32_RETURN(1, min ? W32_TRUE : W32_FALSE);
}

/* HCURSOR LoadCursorA(HINSTANCE, LPCSTR) */
static void imp_LoadCursorA(void)
{
    W32_RETURN(2, UI_HCURSOR);
}

/* HICON LoadIconA(HINSTANCE, LPCSTR) */
static void imp_LoadIconA(void)
{
    W32_RETURN(2, UI_HICON);
}

/* int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) */
static void imp_MessageBoxA(void)
{
    char text[1024], caption[256];
    uint32_t r = message_box("MessageBoxA", W32_ARG(0),
                             ansi_to_utf8(W32_ARG(1), "", text, sizeof text),
                             ansi_to_utf8(W32_ARG(2), "Error", caption, sizeof caption),
                             W32_ARG(3));
    W32_RETURN(4, r);
}

/* int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT type) */
static void imp_MessageBoxW(void)
{
    char text[1024], caption[256];
    uint32_t r = message_box("MessageBoxW", W32_ARG(0),
                             wide_to_utf8(W32_ARG(1), "", text, sizeof text),
                             wide_to_utf8(W32_ARG(2), "Error", caption, sizeof caption),
                             W32_ARG(3));
    W32_RETURN(4, r);
}

/* BOOL MoveWindow(HWND, int x, int y, int w, int h, BOOL repaint) */
static void imp_MoveWindow(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    if (w == NULL)
        w32_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    else
        place_window(w, (int32_t)W32_ARG(1), (int32_t)W32_ARG(2), (int32_t)W32_ARG(3),
                     (int32_t)W32_ARG(4), 1, 1);
    W32_RETURN(6, w != NULL ? W32_TRUE : W32_FALSE);
}

/* BOOL PeekMessageA(LPMSG, HWND, UINT min, UINT max, UINT remove) */
static void imp_PeekMessageA(void)
{
    uint32_t r = peek_message(W32_ARG(0), W32_ARG(1), W32_ARG(2), W32_ARG(3),
                              (W32_ARG(4) & PM_REMOVE) != 0);
    W32_RETURN(5, r);
}

/* void PostQuitMessage(int code): only the main thread has a queue. */
static void imp_PostQuitMessage(void)
{
    if (pthread_main_np()) {
        g_quit_pending = 1;
        g_quit_code = W32_ARG(0);
    }
    W32_RETURN(1, 0);
}

/* ATOM RegisterClassExA(const WNDCLASSEXA *) */
static void imp_RegisterClassExA(void)
{
    uint32_t wc = W32_ARG(0);
    uint32_t atom = 0;
    char key[256];
    class_key(MEM32(wc + 40), key, sizeof key);
    if (class_find(MEM32(wc + 40)) != NULL) {
        w32_set_last_error(ERROR_CLASS_ALREADY_EXISTS);
    } else if (g_class_count == UI_MAX_CLASSES) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    } else {
        struct w32_class *c = &g_classes[g_class_count];
        atom = UI_ATOM_BASE + (uint32_t)g_class_count++;
        snprintf(c->name, sizeof c->name, "%s", key);
        c->atom = (uint16_t)atom;
        c->style = MEM32(wc + 4);
        c->wndproc = MEM32(wc + 8);
        c->wnd_extra = MEM32(wc + 16);
        c->instance = MEM32(wc + 20);
    }
    W32_RETURN(1, atom);
}

/* LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM) */
static void imp_SendMessageA(void)
{
    uint32_t r = send_msg(W32_ARG(0), W32_ARG(1), W32_ARG(2), W32_ARG(3));
    W32_RETURN(4, r);
}

/* The host cursor shows when the guest has a cursor set and the ShowCursor
 * counter is 0 or more. */
static void cursor_apply(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_ShowCursor(g_cursor != 0 && g_cursor_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
}

/* HCURSOR SetCursor(HCURSOR): NULL removes the cursor from the screen. */
static void imp_SetCursor(void)
{
    uint32_t prev = g_cursor;
    g_cursor = W32_ARG(0);
    cursor_apply();
    W32_RETURN(1, prev);
}

/* int ShowCursor(BOOL show): add 1 or -1 to the display counter (0 at
 * start) and return the new value; the cursor is hidden below 0. */
static void imp_ShowCursor(void)
{
    g_cursor_count += W32_ARG(0) != 0 ? 1 : -1;
    cursor_apply();
    W32_RETURN(1, (uint32_t)g_cursor_count);
}

/* BOOL SetCursorPos(int x, int y), screen coordinates */
static void imp_SetCursorPos(void)
{
    w32_ui_init();
    int ok = SDL_WarpMouseGlobal((int32_t)W32_ARG(0), (int32_t)W32_ARG(1)) == 0;
    W32_RETURN(2, ok ? W32_TRUE : W32_FALSE);
}

/* BOOL SetForegroundWindow(HWND) */
static void imp_SetForegroundWindow(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    if (w != NULL && w->sdl != NULL)
        SDL_RaiseWindow(w->sdl);
    W32_RETURN(1, w != NULL ? W32_TRUE : W32_FALSE);
}

/* BOOL SetWindowPos(HWND, HWND after, int x, int y, int cx, int cy, UINT flags) */
static void imp_SetWindowPos(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    uint32_t after = W32_ARG(1), flags = W32_ARG(6);
    if (w == NULL) {
        w32_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    } else {
        place_window(w, (int32_t)W32_ARG(2), (int32_t)W32_ARG(3), (int32_t)W32_ARG(4),
                     (int32_t)W32_ARG(5), !(flags & SWP_NOMOVE), !(flags & SWP_NOSIZE));
        if (w->sdl != NULL && !(flags & SWP_NOZORDER) &&
            (after == HWND_TOPMOST || after == HWND_NOTOPMOST))
            SDL_SetWindowAlwaysOnTop(w->sdl, after == HWND_TOPMOST ? SDL_TRUE : SDL_FALSE);
        if (flags & SWP_SHOWWINDOW)
            show_window(w, SW_SHOW);
        else if (flags & SWP_HIDEWINDOW)
            show_window(w, SW_HIDE);
    }
    W32_RETURN(7, w != NULL ? W32_TRUE : W32_FALSE);
}

/* BOOL ShowWindow(HWND, int cmd): nonzero if it was visible before */
static void imp_ShowWindow(void)
{
    struct w32_window *w = win_get(W32_ARG(0));
    int was_visible = w != NULL ? show_window(w, (int32_t)W32_ARG(1)) : 0;
    W32_RETURN(2, was_visible ? W32_TRUE : W32_FALSE);
}

/* BOOL SystemParametersInfoA(UINT action, UINT param, PVOID, UINT winini).
 * The game saves and turns off the accessibility hot keys and the screen
 * saver (docs/STARTUP_REQS.md); the hot keys do not exist here. */
static void imp_SystemParametersInfoA(void)
{
    uint32_t action = W32_ARG(0), param = W32_ARG(1), pv = W32_ARG(2);
    uint32_t ok = W32_TRUE;
    switch (action) {
    case SPI_GETSTICKYKEYS: /* STICKYKEYS: cbSize, dwFlags */
    case SPI_GETTOGGLEKEYS: /* TOGGLEKEYS: cbSize, dwFlags */
        MEM32(pv + 4) = 0;
        break;
    case SPI_GETFILTERKEYS: /* FILTERKEYS: cbSize, dwFlags and four times */
        for (uint32_t off = 4; off < 24; off += 4)
            MEM32(pv + off) = 0;
        break;
    case SPI_SETSTICKYKEYS:
    case SPI_SETTOGGLEKEYS:
    case SPI_SETFILTERKEYS:
        break;
    case SPI_GETSCREENSAVEACTIVE:
        w32_ui_init();
        MEM32(pv) = SDL_IsScreenSaverEnabled() ? W32_TRUE : W32_FALSE;
        break;
    case SPI_SETSCREENSAVEACTIVE:
        w32_ui_init();
        if (param)
            SDL_EnableScreenSaver();
        else
            SDL_DisableScreenSaver();
        break;
    default:
        THUNK_UNSUPPORTED("action 0x%X", action);
        ok = W32_FALSE;
        break;
    }
    W32_RETURN(4, ok);
}

/* BOOL TranslateMessage(const MSG *): as Windows, a WM_KEYDOWN posts a WM_CHAR
 * with the key's character (g_key_char) and the same lParam. */
static void imp_TranslateMessage(void)
{
    uint32_t m = W32_ARG(0);
    if (m != 0u && MEM32(m + 4) == WM_KEYDOWN && MEM32(m + 8) < 256u && g_key_char[MEM32(m + 8)] != 0)
        queue_add(MEM32(m), WM_CHAR, g_key_char[MEM32(m + 8)], MEM32(m + 12), 0);
    W32_RETURN(1, W32_TRUE);
}

/* BOOL UpdateWindow(HWND): nothing paints through GDI here. */
static void imp_UpdateWindow(void)
{
    W32_RETURN(1, win_get(W32_ARG(0)) != NULL ? W32_TRUE : W32_FALSE);
}

/* ---- GDI32 imports ----------------------------------------------------- */

/* HDC CreateDCA(LPCSTR driver, LPCSTR device, LPCSTR port, const DEVMODEA *) */
static void imp_CreateDCA(void)
{
    char driver[64], device[64];
    ansi_to_utf8(W32_ARG(0), "", driver, sizeof driver);
    ansi_to_utf8(W32_ARG(1), "", device, sizeof device);
    int display = strcasecmp(driver, "DISPLAY") == 0 || strcasecmp(device, "\\\\.\\DISPLAY1") == 0;
    W32_RETURN(4, display ? UI_HDC : 0u);
}

/* BOOL DeleteObject(HGDIOBJ). This runtime creates no GDI objects, and the
 * game passes an event handle, so: FALSE, as Windows returns for
 * a handle that is not a GDI object, and the handle stays open. */
static void imp_DeleteObject(void)
{
    diag_unsupported("GDI32.dll!DeleteObject", MEM32(g_esp),
                     "not a GDI object of this runtime; returns FALSE, handle left open");
    W32_RETURN(1, W32_FALSE);
}

/* BOOL DeleteDC(HDC) */
static void imp_DeleteDC(void)
{
    W32_RETURN(1, W32_TRUE);
}

/* int ExtEscape(HDC, int escape, int in_size, LPCSTR in, int out_size, LPSTR out) */
static void imp_ExtEscape(void)
{
    if (getenv("RECOMP_STARTUP_VERBOSE") != NULL)
        fprintf(stderr, "ExtEscape: escape %d not supported\n", (int32_t)W32_ARG(1));
    W32_RETURN(6, 0);
}

/* NTSTATUS D3DKMTOpenAdapterFromHdc(D3DKMT_OPENADAPTERFROMHDC *) and
 * NTSTATUS D3DKMTEscape(const D3DKMT_ESCAPE *): no kernel adapter here, so
 * both fail with STATUS_NOT_SUPPORTED and write nothing. The game's AMD
 * CrossFire query then reports one GPU. Without these exports
 * GetProcAddress fails, that query returns a "success" code without writing
 * its count, and the uninitialised count (the back buffer height) becomes
 * the GPU count, which makes every scene change flush that many frames. */
#define W32_STATUS_NOT_SUPPORTED 0xC00000BBu
static void imp_D3DKMTOpenAdapterFromHdc(void)
{
    W32_RETURN(1, W32_STATUS_NOT_SUPPORTED);
}

static void imp_D3DKMTEscape(void)
{
    W32_RETURN(1, W32_STATUS_NOT_SUPPORTED);
}

const win32_export_t win32_exports_ui[] = {
    {"GDI32.dll", "CreateDCA", 0, imp_CreateDCA},
    {"GDI32.dll", "D3DKMTEscape", 0, imp_D3DKMTEscape},
    {"GDI32.dll", "D3DKMTOpenAdapterFromHdc", 0, imp_D3DKMTOpenAdapterFromHdc},
    {"GDI32.dll", "DeleteDC", 0, imp_DeleteDC},
    {"GDI32.dll", "DeleteObject", 0, imp_DeleteObject},
    {"GDI32.dll", "ExtEscape", 0, imp_ExtEscape},
    {"USER32.dll", "AdjustWindowRect", 0, imp_AdjustWindowRect},
    {"USER32.dll", "CreateWindowExA", 0, imp_CreateWindowExA},
    {"USER32.dll", "DefWindowProcA", 0, imp_DefWindowProcA},
    {"USER32.dll", "DestroyWindow", 0, imp_DestroyWindow},
    {"USER32.dll", "DispatchMessageA", 0, imp_DispatchMessageA},
    {"USER32.dll", "EnumDisplayDevicesA", 0, imp_EnumDisplayDevicesA},
    {"USER32.dll", "EnumDisplayMonitors", 0, imp_EnumDisplayMonitors},
    {"USER32.dll", "GetForegroundWindow", 0, imp_GetForegroundWindow},
    {"USER32.dll", "GetKeyboardLayout", 0, imp_GetKeyboardLayout},
    {"USER32.dll", "GetSystemMetrics", 0, imp_GetSystemMetrics},
    {"USER32.dll", "GetWindowLongA", 0, imp_GetWindowLongA},
    {"USER32.dll", "GetWindowThreadProcessId", 0, imp_GetWindowThreadProcessId},
    {"USER32.dll", "IsIconic", 0, imp_IsIconic},
    {"USER32.dll", "LoadCursorA", 0, imp_LoadCursorA},
    {"USER32.dll", "LoadIconA", 0, imp_LoadIconA},
    {"USER32.dll", "LoadKeyboardLayoutA", 0, imp_LoadKeyboardLayoutA},
    {"USER32.dll", "MapVirtualKeyExA", 0, imp_MapVirtualKeyExA},
    {"USER32.dll", "MessageBoxA", 0, imp_MessageBoxA},
    {"USER32.dll", "MessageBoxW", 0, imp_MessageBoxW},
    {"USER32.dll", "MoveWindow", 0, imp_MoveWindow},
    {"USER32.dll", "PeekMessageA", 0, imp_PeekMessageA},
    {"USER32.dll", "PostQuitMessage", 0, imp_PostQuitMessage},
    {"USER32.dll", "RegisterClassExA", 0, imp_RegisterClassExA},
    {"USER32.dll", "SendMessageA", 0, imp_SendMessageA},
    {"USER32.dll", "SetCursor", 0, imp_SetCursor},
    {"USER32.dll", "SetCursorPos", 0, imp_SetCursorPos},
    {"USER32.dll", "SetForegroundWindow", 0, imp_SetForegroundWindow},
    {"USER32.dll", "SetWindowPos", 0, imp_SetWindowPos},
    {"USER32.dll", "ShowCursor", 0, imp_ShowCursor},
    {"USER32.dll", "ShowWindow", 0, imp_ShowWindow},
    {"USER32.dll", "SystemParametersInfoA", 0, imp_SystemParametersInfoA},
    {"USER32.dll", "TranslateMessage", 0, imp_TranslateMessage},
    {"USER32.dll", "UpdateWindow", 0, imp_UpdateWindow},
    {0},
};
