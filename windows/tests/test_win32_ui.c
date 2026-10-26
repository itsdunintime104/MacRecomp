/* Win32 UI slice test: every one of the 32 thunks pops its arguments and keeps
 * ebx/esi/edi/ebp; window creation messages, SDL events through
 * PeekMessageA + DispatchMessageA, WM_QUIT, DefWindowProcA(WM_CLOSE), focus
 * under RECOMP_ASSUME_FOCUS=1, display, cursor, message box and GDI calls.
 * Run with SDL_VIDEODRIVER=dummy RECOMP_NO_ALERT_UI=1. Links win32_ui.c,
 * guest_mem.c, host_funcs.c, recomp_glue.c and guest_call.c; the core
 * functions it calls are doubles below. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "win32_thunk.h"
#include "hud_core.h"
#include "win32_ui.h"

/* Generated dispatch; this test has no lifted code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* ---- Core doubles ------------------------------------------------------ */

static uint32_t g_last_error;
uint32_t w32_last_error(void)
{
    return g_last_error;
}
void w32_set_last_error(uint32_t code)
{
    g_last_error = code;
}

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

/* ---- Guest memory ------------------------------------------------------ */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define DATA_AREA 0x00FF4000u
#define TIB 0x00FFE000u
#define RET_ADDR 0xDEAD0001u
#define MAIN_TID 0x1234u

static uint32_t g_data_next = DATA_AREA;
static uint32_t guest_alloc(uint32_t n)
{
    uint32_t a = g_data_next;
    memset(guest_ptr(a), 0, n);
    g_data_next += (n + 15u) & ~15u;
    return a;
}
static uint32_t guest_string(const char *s)
{
    uint32_t a = guest_alloc((uint32_t)strlen(s) + 1u);
    strcpy((char *)guest_ptr(a), s);
    return a;
}
static uint32_t guest_wstring(const char *s)
{
    uint32_t a = guest_alloc(2u * ((uint32_t)strlen(s) + 1u));
    for (uint32_t i = 0; s[i] != '\0'; i++)
        MEM16(a + 2u * i) = (uint8_t)s[i];
    return a;
}

/* ---- Calling thunks ---------------------------------------------------- */

static int g_called[64];

/* Push args right to left and the return address, call the slice's entry
 * for name, check that it popped 4 + 4*nargs bytes and kept ebx/esi/edi/ebp.
 * Returns g_eax. */
static uint32_t call(const char *name, unsigned nargs, const uint32_t *args)
{
    int k = 0;
    while (win32_exports_ui[k].dll != NULL && strcmp(win32_exports_ui[k].name, name) != 0)
        k++;
    if (win32_exports_ui[k].dll == NULL) {
        fprintf(stderr, "FAIL: %s not in win32_exports_ui\n", name);
        g_fail = 1;
        return 0;
    }
    g_called[k] = 1;
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
    win32_exports_ui[k].fn();
    if (g_esp != start + 4u + 4u * nargs) {
        fprintf(stderr, "FAIL: %s popped %d bytes, want %u\n", name, (int)(g_esp - start),
                4u + 4u * nargs);
        g_fail = 1;
    }
    if (g_ebx != 0xB0B0B0B0u || g_esi != 0x51515151u || g_edi != 0xD1D1D1D1u ||
        g_ebp != 0xBEBEBEBEu) {
        fprintf(stderr, "FAIL: %s changed a callee-saved register\n", name);
        g_fail = 1;
    }
    return g_eax;
}
#define CALL(name, ...)                                                          \
    call(name, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t),             \
         (uint32_t[]){__VA_ARGS__})

/* ---- Guest callbacks --------------------------------------------------- */

struct rec {
    uint32_t hwnd, msg, wp, lp, cs_params; /* cs_params: CREATESTRUCTA.lpCreateParams */
};
static struct rec g_log[256];
static int g_log_len;
static uint32_t g_create_result;

/* A stdcall WNDPROC: records the message and pops 16 bytes. */
static void test_wndproc(void)
{
    struct rec r = {MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16), 0};
    if (r.msg == 0x81 || r.msg == 0x01)
        r.cs_params = MEM32(r.lp);
    if (g_log_len < 256)
        g_log[g_log_len++] = r;
    g_eax = r.msg == 0x81 ? 1u : r.msg == 0x01 ? g_create_result : 0u;
    g_esp += 20;
}

static uint32_t g_mon[4], g_mon_rect[4];
static int g_mon_calls;

/* A stdcall MONITORENUMPROC. */
static void test_enum_monitor(void)
{
    for (int i = 0; i < 4; i++)
        g_mon[i] = MEM32(g_esp + 4 + 4 * (uint32_t)i);
    for (int i = 0; i < 4; i++)
        g_mon_rect[i] = MEM32(g_mon[2] + 4u * (uint32_t)i);
    g_mon_calls++;
    g_eax = 1;
    g_esp += 20;
}

/* Index of the first logged message msg at or after from, or -1. */
static int log_find(int from, uint32_t msg)
{
    for (int i = from; i < g_log_len; i++)
        if (g_log[i].msg == msg)
            return i;
    return -1;
}

/* ---- stderr capture ---------------------------------------------------- */

static FILE *g_cap;
static int g_saved_fd;
static void capture_begin(void)
{
    fflush(stderr);
    g_cap = tmpfile();
    g_saved_fd = dup(2);
    dup2(fileno(g_cap), 2);
}
static char *capture_end(void)
{
    static char buf[4096];
    fflush(stderr);
    dup2(g_saved_fd, 2);
    close(g_saved_fd);
    rewind(g_cap);
    size_t n = fread(buf, 1, sizeof buf - 1, g_cap);
    buf[n] = '\0';
    fclose(g_cap);
    return buf;
}

/* ---- Tests ------------------------------------------------------------- */

#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u

static uint32_t g_wndproc_va;

static uint32_t create(uint32_t style, int x, int y, int w, int h, uint32_t param)
{
    return CALL("CreateWindowExA", 0, guest_string("ttaleswindow"), guest_string("Title"), style,
                (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 0, 0, 0x00400000u, param);
}

static void push_key(uint32_t type, SDL_Scancode sc, uint32_t window_id)
{
    SDL_Event ev = {0};
    ev.type = type;
    ev.key.windowID = window_id;
    ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    CHECK(SDL_PushEvent(&ev) == 1);
}

/* PeekMessageA(PM_REMOVE) until message want; 1 when found. */
static int peek_until(uint32_t msgp, uint32_t want)
{
    for (int i = 0; i < 64; i++) {
        if (!CALL("PeekMessageA", msgp, 0, 0, 0, 1))
            return 0;
        if (MEM32(msgp + 4) == want)
            return 1;
    }
    return 0;
}

static void test_class(void)
{
    g_wndproc_va = host_func_va(test_wndproc);
    uint32_t wc = guest_alloc(48);
    MEM32(wc + 0) = 48;
    MEM32(wc + 4) = 3;
    MEM32(wc + 8) = g_wndproc_va;
    MEM32(wc + 16) = 8; /* cbWndExtra */
    MEM32(wc + 20) = 0x00400000u;
    MEM32(wc + 40) = guest_string("TTalesWindow");
    uint32_t atom = CALL("RegisterClassExA", wc);
    CHECK(atom >= 0xC000u && atom <= 0xFFFFu);
    CHECK(CALL("RegisterClassExA", wc) == 0);
    CHECK(CALL("LoadIconA", 0x00400000u, 101) != 0);
    CHECK(CALL("LoadCursorA", 0, 32512) != 0);
}

static void test_adjust(void)
{
    uint32_t rc = guest_alloc(16);
    MEM32(rc + 8) = 640, MEM32(rc + 12) = 480;
    CHECK(CALL("AdjustWindowRect", rc, WS_OVERLAPPEDWINDOW, 0) == 1);
    CHECK((int32_t)MEM32(rc) == -8 && (int32_t)MEM32(rc + 4) == -31 && MEM32(rc + 8) == 648 &&
          MEM32(rc + 12) == 488);
    CHECK(CALL("AdjustWindowRect", rc, WS_POPUP, 0) == 1);
    CHECK((int32_t)MEM32(rc) == -8 && MEM32(rc + 8) == 648);
}

static uint32_t test_create(void)
{
    g_log_len = 0;
    uint32_t hwnd = create(WS_OVERLAPPEDWINDOW, 100, 100, 656, 519, 0x5151u);
    CHECK(hwnd == 0x00010010u);
    CHECK(g_log_len >= 2 && g_log[0].msg == 0x81 && g_log[1].msg == 0x01); /* NCCREATE, CREATE */
    CHECK(g_log[0].hwnd == hwnd && g_log[0].cs_params == 0x5151u && g_log[1].cs_params == 0x5151u);
    int sz = log_find(0, 0x05);
    CHECK(sz > 1 && g_log[sz].lp == (640u | 480u << 16)); /* WM_SIZE, client area */
    CHECK(log_find(0, 0x03) > sz);                         /* WM_MOVE */
    CHECK(log_find(0, 0x1C) < 0);                          /* no activation: hidden */
    SDL_Window *sdl = w32_hwnd_to_sdl(hwnd);
    CHECK(sdl != NULL);
    int w = 0, h = 0;
    if (sdl != NULL)
        SDL_GetWindowSize(sdl, &w, &h);
    CHECK(w == 640 && h == 480);
    CHECK(w32_hwnd_to_sdl(0x00999990u) == NULL);

    CHECK(CALL("GetWindowLongA", hwnd, (uint32_t)-4) == g_wndproc_va);
    CHECK(CALL("GetWindowLongA", hwnd, (uint32_t)-16) == WS_OVERLAPPEDWINDOW);
    CHECK(CALL("GetWindowLongA", hwnd, (uint32_t)-21) == 0);
    CHECK(CALL("GetWindowLongA", hwnd, 4) == 0);
    g_last_error = 0;
    CHECK(CALL("GetWindowLongA", hwnd, 8) == 0 && g_last_error == 1413u);
    uint32_t pid = guest_alloc(4);
    CHECK(CALL("GetWindowThreadProcessId", hwnd, pid) == MAIN_TID);
    CHECK(MEM32(pid) == (uint32_t)getpid());

    g_log_len = 0;
    CHECK(CALL("ShowWindow", hwnd, 10) == 0);
    CHECK(CALL("ShowWindow", hwnd, 5) != 0);
    CHECK(log_find(0, 0x1C) < 0); /* RECOMP_ASSUME_FOCUS unset */
    CHECK(CALL("UpdateWindow", hwnd) == 1);
    CHECK(CALL("IsIconic", hwnd) == 0);
    CHECK(CALL("SendMessageA", hwnd, 0x80, 1, 0x00050000u) == 0);
    CHECK(g_log_len > 0 && g_log[g_log_len - 1].msg == 0x80);
    return hwnd;
}

static void test_input(uint32_t hwnd)
{
    uint32_t id = SDL_GetWindowID(w32_hwnd_to_sdl(hwnd));
    uint32_t msg = guest_alloc(28);
    while (CALL("PeekMessageA", msg, 0, 0, 0, 1)) /* drain SDL's own window events */
        ;

    push_key(SDL_KEYDOWN, SDL_SCANCODE_A, id);
    CHECK(CALL("PeekMessageA", msg, 0, 0, 0, 0) == 1); /* PM_NOREMOVE leaves it */
    CHECK(peek_until(msg, 0x100));
    CHECK(MEM32(msg) == hwnd && MEM32(msg + 8) == 0x41u && MEM32(msg + 12) == (1u | 0x1Eu << 16));
    CHECK(CALL("TranslateMessage", msg) == 1);
    g_log_len = 0;
    CALL("DispatchMessageA", msg);
    CHECK(g_log_len == 1 && g_log[0].hwnd == hwnd && g_log[0].msg == 0x100 &&
          g_log[0].wp == 0x41u && g_log[0].lp == (1u | 0x1Eu << 16));

    push_key(SDL_KEYUP, SDL_SCANCODE_RIGHT, id);
    CHECK(peek_until(msg, 0x101));
    CHECK(MEM32(msg + 8) == 0x27u && MEM32(msg + 12) == (1u | 0x4Du << 16 | 1u << 24 | 3u << 30));

    /* SDL text input is off (no macOS accent menu for a held key), and
     * TranslateMessage posts a WM_CHAR for each WM_KEYDOWN, repeats included. */
    CHECK(!SDL_IsTextInputActive());
    push_key(SDL_KEYDOWN, SDL_SCANCODE_W, id);
    CHECK(peek_until(msg, 0x100) && MEM32(msg + 8) == 0x57u && (MEM32(msg + 12) >> 30 & 1u) == 0u);
    CALL("TranslateMessage", msg);
    CHECK(peek_until(msg, 0x102) && MEM32(msg + 8) == 'w');
    SDL_Event rep = {0};
    rep.type = SDL_KEYDOWN;
    rep.key.windowID = id;
    rep.key.state = SDL_PRESSED;
    rep.key.repeat = 1;
    rep.key.keysym.scancode = SDL_SCANCODE_W;
    rep.key.keysym.mod = KMOD_LSHIFT;
    CHECK(SDL_PushEvent(&rep) == 1);
    CHECK(peek_until(msg, 0x100) && (MEM32(msg + 12) >> 30 & 1u) == 1u);
    CALL("TranslateMessage", msg);
    CHECK(peek_until(msg, 0x102) && MEM32(msg + 8) == 'W');
    push_key(SDL_KEYUP, SDL_SCANCODE_W, id);
    CHECK(peek_until(msg, 0x101));
    CALL("TranslateMessage", msg);
    CHECK(!peek_until(msg, 0x102)); /* a key-up posts nothing */

    /* No SDL_TEXTINPUT here: sdl2-compat 2.32.74 crashes pushing one. */
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = id;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = 5, ev.button.y = 6;
    CHECK(SDL_PushEvent(&ev) == 1);
    g_log_len = 0;
    CHECK(peek_until(msg, 0x201));
    CHECK(MEM32(msg + 8) == 1u && MEM32(msg + 12) == (5u | 6u << 16));
    CHECK(log_find(0, 0x20) >= 0); /* WM_SETCURSOR was sent first */

    CALL("PostQuitMessage", 7);
    CHECK(peek_until(msg, 0x12) && MEM32(msg + 8) == 7u);
    CHECK(!peek_until(msg, 0x12));
}

static void test_close(uint32_t hwnd)
{
    CHECK(CALL("DefWindowProcA", hwnd, 0x14, 0, 0) == 1); /* WM_ERASEBKGND */
    CHECK(CALL("DefWindowProcA", hwnd, 0x81, 0, 0) == 1); /* WM_NCCREATE */
    g_log_len = 0;
    CHECK(CALL("DefWindowProcA", hwnd, 0x10, 0, 0) == 0); /* WM_CLOSE */
    int d = log_find(0, 0x02);
    CHECK(d >= 0 && log_find(d, 0x82) > d); /* WM_DESTROY, WM_NCDESTROY */
    CHECK(w32_hwnd_to_sdl(hwnd) == NULL);
    CHECK(CALL("DestroyWindow", hwnd) == 0);

    g_create_result = (uint32_t)-1;
    g_log_len = 0;
    CHECK(create(WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, 0) == 0);
    CHECK(log_find(0, 0x02) >= 0);
    g_create_result = 0;
}

static void test_focus(void)
{
    setenv("RECOMP_ASSUME_FOCUS", "1", 1);
    g_log_len = 0;
    uint32_t hwnd = create(WS_POPUP | WS_VISIBLE, 0, 0, 800, 600, 0);
    CHECK(hwnd != 0);
    int a = log_find(0, 0x1C);
    CHECK(a >= 0 && g_log[a].wp == 1); /* WM_ACTIVATEAPP(TRUE) */
    int b = log_find(a, 0x06);
    CHECK(b > a && g_log[b].wp == 1); /* WM_ACTIVATE(WA_ACTIVE) */
    CHECK(log_find(b, 0x07) > b);     /* WM_SETFOCUS */
    CHECK(CALL("GetForegroundWindow") == hwnd);
    CHECK(CALL("SetForegroundWindow", hwnd) == 1);

    SDL_Window *sdl = w32_hwnd_to_sdl(hwnd);
    int w = 0, h = 0;
    CHECK(CALL("SetWindowPos", hwnd, 0, 10, 10, 1024, 768, 0x4u) == 1);
    SDL_GetWindowSize(sdl, &w, &h);
    CHECK(w == 1024 && h == 768); /* WS_POPUP: no frame */
    CHECK(CALL("MoveWindow", hwnd, 0, 0, 640, 480, 1) == 1);
    SDL_GetWindowSize(sdl, &w, &h);
    CHECK(w == 640 && h == 480);
    CHECK(CALL("DestroyWindow", hwnd) == 1);
    CHECK(CALL("GetForegroundWindow") == 0);
}

/* Windows Direct3D 9 shows the device window of a fullscreen device; LEGO
 * Indiana Jones creates WS_OVERLAPPEDWINDOW without WS_VISIBLE, calls
 * ShowWindow(SW_HIDE) and relies on that. w32_ui_show_device_window, which
 * the D3D9 backend calls then, makes the hidden window visible to the game
 * (WS_VISIBLE) and on screen (SDL shown). */
static void test_device_window(void)
{
    uint32_t hwnd = create(WS_OVERLAPPEDWINDOW, 0, 0, 976, 639, 0);
    CHECK(hwnd != 0);
    CHECK(CALL("ShowWindow", hwnd, 0) == 0);
    SDL_Window *sdl = w32_hwnd_to_sdl(hwnd);
    CHECK(sdl != NULL && (SDL_GetWindowFlags(sdl) & SDL_WINDOW_SHOWN) == 0);
    CHECK((CALL("GetWindowLongA", hwnd, (uint32_t)-16) & WS_VISIBLE) == 0);
    w32_ui_show_device_window(NULL); /* no window: nothing happens */
    w32_ui_show_device_window(sdl);
    CHECK((CALL("GetWindowLongA", hwnd, (uint32_t)-16) & WS_VISIBLE) != 0);
    CHECK(sdl != NULL && (SDL_GetWindowFlags(sdl) & SDL_WINDOW_SHOWN) != 0);
    printf("device window: hidden window shown for a fullscreen device\n");
    CHECK(CALL("DestroyWindow", hwnd) == 1);
}

static void test_display(void)
{
    SDL_DisplayMode m;
    CHECK(SDL_GetCurrentDisplayMode(0, &m) == 0);
    CHECK(CALL("GetSystemMetrics", 0) == (uint32_t)m.w);
    CHECK(CALL("GetSystemMetrics", 1) == (uint32_t)m.h);
    CHECK(CALL("GetKeyboardLayout", 0) == 0x04090409u);
    CHECK(CALL("EnumDisplayMonitors", 0, 0, host_func_va(test_enum_monitor), 0x55) == 1);
    CHECK(g_mon_calls == 1 && g_mon[0] == 0x00020000u && g_mon[1] == 0 && g_mon[3] == 0x55);
    CHECK(g_mon_rect[0] == 0 && g_mon_rect[1] == 0 && g_mon_rect[2] == (uint32_t)m.w &&
          g_mon_rect[3] == (uint32_t)m.h);

    uint32_t dd = guest_alloc(424);
    MEM32(dd) = 424;
    CHECK(CALL("EnumDisplayDevicesA", 0, 0, dd, 0) == 1);
    CHECK(strcmp((const char *)guest_ptr(dd + 4), "\\\\.\\DISPLAY1") == 0);
    CHECK(strcmp((const char *)guest_ptr(dd + 36), "AMD Radeon RX 6700 XT") == 0);
    CHECK(MEM32(dd + 164) == 5u);
    CHECK(strcmp((const char *)guest_ptr(dd + 168), "PCI\\VEN_1002&DEV_73DF") == 0);
    CHECK(CALL("EnumDisplayDevicesA", 0, 1, dd, 0) == 0);
    MEM32(dd) = 4;
    CHECK(CALL("EnumDisplayDevicesA", 0, 0, dd, 0) == 0);
    uint32_t sk = guest_alloc(24);
    MEM32(sk) = 8, MEM32(sk + 4) = 0x1FFu;
    CHECK(CALL("SystemParametersInfoA", 0x3A, 8, sk, 0) == 1 && MEM32(sk) == 8 && MEM32(sk + 4) == 0);
    CHECK(CALL("SystemParametersInfoA", 0x3B, 8, sk, 0) == 1);
    CHECK(CALL("SystemParametersInfoA", 0x32, 24, sk, 0) == 1);
    CHECK(CALL("SystemParametersInfoA", 0x10, 0, sk, 0) == 1);
    CHECK(CALL("SystemParametersInfoA", 0x11, 0, 0, 0) == 1);
}

static void test_misc(void)
{
    CHECK(CALL("SetCursor", 0) == 0x00040000u);
    CHECK(CALL("SetCursor", 0x00040000u) == 0);
    CALL("SetCursorPos", 10, 10);

    capture_begin();
    uint32_t ok = CALL("MessageBoxA", 0, guest_string("Insufficient \x93Permissions\x94"),
                       guest_string("Cap"), 0x10);
    uint32_t yes = CALL("MessageBoxA", 0, guest_string("Quit?"), 0, 4);
    uint32_t w = CALL("MessageBoxW", 0, guest_wstring("Disc"), guest_wstring("Wide"), 0);
    char *out = capture_end();
    CHECK(ok == 1 && yes == 6 && w == 1);
    CHECK(strstr(out, "MessageBoxA: Cap: Insufficient \xE2\x80\x9CPermissions\xE2\x80\x9D\n") != NULL);
    CHECK(strstr(out, "MessageBoxA: Error: Quit?\n") != NULL);
    CHECK(strstr(out, "MessageBoxW: Wide: Disc\n") != NULL);

    uint32_t hdc = CALL("CreateDCA", guest_string("DISPLAY"), 0, 0, 0);
    CHECK(hdc == 0x00030000u);
    CHECK(CALL("CreateDCA", guest_string("WINSPOOL"), 0, 0, 0) == 0);
    CHECK(CALL("ExtEscape", hdc, 0x1000, 0, 0, 0, 0) == 0);
    CHECK(CALL("D3DKMTOpenAdapterFromHdc", 0x00200000u) == 0xC00000BBu);
    CHECK(CALL("D3DKMTEscape", 0x00200000u) == 0xC00000BBu);
    CHECK(CALL("DeleteDC", hdc) == 1);

    /* DeleteObject on a non-GDI handle (the game passes an event): FALSE,
     * one log line for any number of calls. */
    capture_begin();
    CHECK(CALL("DeleteObject", 0xFF800010u) == 0);
    CHECK(CALL("DeleteObject", 0xFF800014u) == 0);
    out = capture_end();
    CHECK(strstr(out, "UNSUPPORTED GDI32.dll!DeleteObject") != NULL);
    CHECK(strstr(strstr(out, "DeleteObject") + 1, "DeleteObject") == NULL);

    /* ShowCursor: a display counter from 0; the host cursor hides below 0.
     * The game loops on ShowCursor(FALSE) until the count is negative and
     * on ShowCursor(TRUE) until it is not. */
    CHECK(CALL("ShowCursor", 0) == 0xFFFFFFFFu);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE);
    CHECK(CALL("ShowCursor", 0) == 0xFFFFFFFEu);
    CHECK(CALL("ShowCursor", 1) == 0xFFFFFFFFu);
    CHECK(CALL("ShowCursor", 1) == 0);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE);
    CHECK(CALL("ShowCursor", 7) == 1);
    CHECK(CALL("ShowCursor", 0) == 0);

    /* Keyboard layout and MapVirtualKeyExA, US layout (Windows values). */
    uint32_t hkl = CALL("LoadKeyboardLayoutA", guest_string("00000409"), 1);
    CHECK(hkl == 0x04090409u);
    static const uint32_t vk_cases[][3] = {
        /* code, type, Windows result */
        {0x1E, 3, 'A'},  {0x02, 3, '1'},  {0x39, 3, 0x20}, {0x1C, 3, 0x0D},
        {0x2A, 3, 0xA0}, {0x36, 3, 0xA1}, {0x1D, 3, 0xA2}, {0xE01D, 3, 0xA3},
        {0x38, 3, 0xA4}, {0xE038, 3, 0xA5}, {0x4B, 3, 0x25}, {0xE04B, 3, 0x25},
        {0x0C, 3, 0xBD}, {0x56, 3, 0xE2}, {0x00, 3, 0},    {0x7F, 3, 0},
        {0x2A, 1, 0x10}, {0x1D, 1, 0x11}, {0x38, 1, 0x12}, {0x45, 1, 0x90},
        {'A', 0, 0x1E},  {0x10, 0, 0x2A}, {0xA1, 0, 0x36}, {0x25, 0, 0x4B},
        {0x25, 4, 0xE04B}, {'A', 4, 0x1E}, {0xA3, 4, 0xE01D}, {0x07, 0, 0},
    };
    for (size_t i = 0; i < sizeof vk_cases / sizeof vk_cases[0]; i++) {
        uint32_t r = CALL("MapVirtualKeyExA", vk_cases[i][0], vk_cases[i][1], hkl);
        if (r != vk_cases[i][2]) {
            fprintf(stderr, "FAIL: MapVirtualKeyExA(0x%X, %u) = 0x%X, want 0x%X\n", vk_cases[i][0],
                    vk_cases[i][1], r, vk_cases[i][2]);
            g_fail = 1;
        }
    }
    capture_begin();
    CHECK(CALL("MapVirtualKeyExA", 'A', 2, hkl) == 0);
    out = capture_end();
    CHECK(strstr(out, "UNSUPPORTED USER32.dll!MapVirtualKeyExA: map type 2") != NULL);
}

/* Ctrl+Cmd+F toggles fullscreen: the SDL event filter drops the F and Cmd
 * events before any event watch (DirectInput's) and before the message
 * queue, and hides F from the keyboard state until it is released. Cmd+F
 * and a plain F still reach both (Cmd itself never does). */
static int g_watch_keys;
static int SDLCALL count_keys(void *ud, SDL_Event *e)
{
    (void)ud;
    g_watch_keys += e->type == SDL_KEYDOWN || e->type == SDL_KEYUP;
    return 1;
}

static void push_mod_key(uint32_t type, SDL_Scancode sc, uint16_t mod, uint32_t window_id)
{
    SDL_Event ev = {0};
    ev.type = type;
    ev.key.windowID = window_id;
    ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.mod = mod;
    SDL_PushEvent(&ev);
}

static void test_toggle_keys(uint32_t hwnd)
{
    uint32_t id = SDL_GetWindowID(w32_hwnd_to_sdl(hwnd));
    uint32_t msg = guest_alloc(28);
    while (CALL("PeekMessageA", msg, 0, 0, 0, 1))
        ;
    SDL_AddEventWatch(count_keys, NULL);
    g_watch_keys = 0;
    push_mod_key(SDL_KEYDOWN, SDL_SCANCODE_LGUI, KMOD_LGUI, id);
    push_mod_key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_LGUI | KMOD_LCTRL, id);
    CHECK(w32_ui_key_hidden(SDL_SCANCODE_F) && w32_ui_key_hidden(SDL_SCANCODE_LGUI));
    push_mod_key(SDL_KEYUP, SDL_SCANCODE_LGUI, KMOD_LCTRL, id);
    push_mod_key(SDL_KEYUP, SDL_SCANCODE_F, KMOD_LCTRL, id);
    CHECK(!w32_ui_key_hidden(SDL_SCANCODE_F));
    push_mod_key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_LCTRL | KMOD_RGUI, id);
    CHECK(w32_ui_key_hidden(SDL_SCANCODE_F));
    push_mod_key(SDL_KEYUP, SDL_SCANCODE_F, KMOD_LCTRL, id);
    CHECK(g_watch_keys == 0);
    CHECK(!peek_until(msg, 0x100) && !peek_until(msg, 0x101));
    /* Cmd+F toggles the frame-time overlay; the game sees neither key. */
    int hud = hud_on();
    push_mod_key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_LGUI, id);
    CHECK(hud_on() == !hud && w32_ui_key_hidden(SDL_SCANCODE_F));
    push_mod_key(SDL_KEYUP, SDL_SCANCODE_F, KMOD_LGUI, id);
    CHECK(g_watch_keys == 0 && !w32_ui_key_hidden(SDL_SCANCODE_F));
    CHECK(!peek_until(msg, 0x100) && !peek_until(msg, 0x101));
    SDL_DelEventWatch(count_keys, NULL);
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    MEM32(TIB + 0x24) = MAIN_TID; /* TEB ClientId.UniqueThread */
    g_fs_base = TIB;

    int n = 0;
    while (win32_exports_ui[n].dll != NULL)
        n++;
    CHECK(n == 39);

    test_class();
    test_adjust();
    uint32_t hwnd = test_create();
    test_input(hwnd);
    test_toggle_keys(hwnd);
    test_close(hwnd);
    test_focus();
    test_device_window();
    test_display();
    test_misc();

    for (int k = 0; k < n; k++)
        if (!g_called[k]) {
            fprintf(stderr, "FAIL: %s not called\n", win32_exports_ui[k].name);
            g_fail = 1;
        }
    if (g_fail) {
        fprintf(stderr, "test_win32_ui: FAILED\n");
        return 1;
    }
    printf("test_win32_ui: all checks passed (%d thunks)\n", n);
    return 0;
}
