/* Win32 input slice: XINPUT1_3 ordinals 2-4 and DINPUT8!DirectInput8Create,
 * with the two COM interfaces DirectInput8Create hands out (IDirectInput8A,
 * IDirectInputDevice8A), on SDL2. Contract: docs/WIN32_THUNKS.md,
 * win32_thunk.h, com_guest.h.
 *
 * Pads go through XInput only. Every pad is an XInput gamepad, as every pad
 * is an Xbox 360 pad in the engine's frontend_pad.c; SDL's GameController
 * layout is positional (A is the bottom face button on a PlayStation pad
 * too), so no per-pad handling is needed. EnumDevices lists the system
 * keyboard and mouse, then each pad as an Xbox 360 controller that
 * CreateDevice does not open: a game finds pads that way and, through WMI
 * (win32_wmi.c), learns they are XInput devices.
 *
 * Every offset below is from the mingw-w64 i686 headers (dinput.h, xinput.h,
 * DIRECTINPUT_VERSION 0x0800), printed with i686-w64-mingw32-gcc. The UI slice
 * pumps SDL events on the main thread; this slice only reads state, plus one
 * event watch for mouse wheel and buffered data. */
#include <SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "com_guest.h"
#include "diag.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "input_script.h"
#include "recomp_types.h"
#include "win32_thunk.h"
#include "win32_ui.h"

/* ---- XInput ------------------------------------------------------------ */

#define XUSER_MAX_COUNT 4u
#define XI_ERROR_DEVICE_NOT_CONNECTED 1167u

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD. */
#define XI_STATE_PACKET 0x00u
#define XI_STATE_GAMEPAD 0x04u
/* XINPUT_GAMEPAD, 12 bytes. */
#define XI_GP_BUTTONS 0x00u
#define XI_GP_LT 0x02u
#define XI_GP_RT 0x03u
#define XI_GP_LX 0x04u
#define XI_GP_LY 0x06u
#define XI_GP_RX 0x08u
#define XI_GP_RY 0x0Au
#define XI_GP_SIZE 12u
/* XINPUT_VIBRATION. */
#define XI_VIB_LEFT 0x00u
#define XI_VIB_RIGHT 0x02u
/* XINPUT_CAPABILITIES, 20 bytes. */
#define XI_CAPS_TYPE 0x00u
#define XI_CAPS_SUBTYPE 0x01u
#define XI_CAPS_FLAGS 0x02u
#define XI_CAPS_GAMEPAD 0x04u
#define XI_CAPS_VIBRATION 0x10u
#define XI_CAPS_SIZE 20u
#define XINPUT_DEVTYPE_GAMEPAD 0x01u
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01u
#define XI_ALL_BUTTONS 0xF3FFu /* every XINPUT_GAMEPAD_* bit */

static const struct {
    SDL_GameControllerButton sdl;
    uint16_t xi;
} XI_BUTTONS[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001},    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004},  {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
    {SDL_CONTROLLER_BUTTON_START, 0x0010},      {SDL_CONTROLLER_BUTTON_BACK, 0x0020},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040},  {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200},
    {SDL_CONTROLLER_BUTTON_A, 0x1000},          {SDL_CONTROLLER_BUTTON_B, 0x2000},
    {SDL_CONTROLLER_BUTTON_X, 0x4000},          {SDL_CONTROLLER_BUTTON_Y, 0x8000},
};

/* User n is the n-th pad SDL opened; a pad keeps its user until unplugged. */
static struct {
    SDL_GameController *gc;
    uint8_t last[XI_GP_SIZE]; /* last XINPUT_GAMEPAD returned */
    uint32_t packet;
} s_xi[XUSER_MAX_COUNT];
static int s_xi_inited;
static pthread_mutex_t s_xi_lock = PTHREAD_MUTEX_INITIALIZER;

/* HPRECOMP_TRACE_INPUT=1: "PAD" lines on stderr for pad discovery
 * (EnumDevices, WMI) and for every change of an XInput result. */
static int s_itrace = -1;

void w32_input_trace(const char *fmt, ...)
{
    if (s_itrace < 0) {
        const char *s = getenv("HPRECOMP_TRACE_INPUT");
        s_itrace = s != NULL && *s != '\0' && *s != '0';
    }
    if (!s_itrace)
        return;
    va_list ap;
    va_start(ap, fmt);
    flockfile(stderr);
    fputs("PAD ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
}

/* Runs with s_xi_lock held: close unplugged pads, open new ones into the
 * first free user. */
static void xi_refresh(void)
{
    if (!s_xi_inited) {
        s_xi_inited = 1;
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        w32_input_trace("init: %d joysticks", SDL_NumJoysticks());
    }
    SDL_GameControllerUpdate();
    for (uint32_t i = 0; i < XUSER_MAX_COUNT; i++) {
        if (s_xi[i].gc && !SDL_GameControllerGetAttached(s_xi[i].gc)) {
            w32_input_trace("user %u closed", i);
            SDL_GameControllerClose(s_xi[i].gc);
            s_xi[i].gc = NULL;
        }
    }
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j))
            continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        int have = 0, free_user = -1;
        for (uint32_t i = 0; i < XUSER_MAX_COUNT; i++) {
            if (s_xi[i].gc &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_xi[i].gc)) == id)
                have = 1;
            else if (!s_xi[i].gc && free_user < 0)
                free_user = (int)i;
        }
        if (!have && free_user >= 0) {
            s_xi[free_user].gc = SDL_GameControllerOpen(j);
            w32_input_trace("user %d opened: joystick %d \"%s\" %s", free_user, j,
                            SDL_GameControllerNameForIndex(j), s_xi[free_user].gc ? "ok" : SDL_GetError());
        }
    }
}

/* Runs with s_xi_lock held. Returns the pad of user, or NULL. */
static SDL_GameController *xi_pad(uint32_t user)
{
    xi_refresh();
    return user < XUSER_MAX_COUNT ? s_xi[user].gc : NULL;
}

/* The number of attached pads, at most XUSER_MAX_COUNT: what EnumDevices and
 * WMI (win32_wmi.c) list. Pad n is the n-th SDL game controller in joystick
 * index order, the order xi_refresh gives pads users, and a game takes its
 * enumeration count as the XInput user. */
uint32_t w32_input_pads(void)
{
    uint32_t n = 0;
    pthread_mutex_lock(&s_xi_lock);
    xi_refresh();
    for (int j = 0; j < SDL_NumJoysticks() && n < XUSER_MAX_COUNT; j++)
        n += SDL_IsGameController(j) ? 1u : 0u;
    pthread_mutex_unlock(&s_xi_lock);
    return n;
}

/* SDL sticks are Y down positive; XInput's are Y up positive. */
static int16_t xi_invert(int16_t v)
{
    return v == INT16_MIN ? INT16_MAX : (int16_t)-v;
}

static void xi_read(SDL_GameController *gc, uint8_t gp[XI_GP_SIZE])
{
    uint16_t buttons = 0;
    for (size_t i = 0; i < sizeof XI_BUTTONS / sizeof XI_BUTTONS[0]; i++)
        if (SDL_GameControllerGetButton(gc, XI_BUTTONS[i].sdl))
            buttons |= XI_BUTTONS[i].xi;
    int16_t lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = xi_invert(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    int16_t rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = xi_invert(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
    /* SDL triggers are 0..32767. */
    int16_t lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int16_t rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    memcpy(gp + XI_GP_BUTTONS, &buttons, 2);
    gp[XI_GP_LT] = (uint8_t)(lt > 0 ? lt >> 7 : 0);
    gp[XI_GP_RT] = (uint8_t)(rt > 0 ? rt >> 7 : 0);
    memcpy(gp + XI_GP_LX, &lx, 2);
    memcpy(gp + XI_GP_LY, &ly, 2);
    memcpy(gp + XI_GP_RX, &rx, 2);
    memcpy(gp + XI_GP_RY, &ry, 2);
}

/* Scripted user 0 (input_script.h) on top of a real pad: buttons ORed, a
 * scripted stick value other than 0 replaces the real one. */
static void xi_script(const struct input_script_pad *sp, uint8_t gp[XI_GP_SIZE])
{
    uint16_t buttons;
    memcpy(&buttons, gp + XI_GP_BUTTONS, 2);
    buttons |= sp->buttons;
    memcpy(gp + XI_GP_BUTTONS, &buttons, 2);
    if (sp->lx)
        memcpy(gp + XI_GP_LX, &sp->lx, 2);
    if (sp->ly)
        memcpy(gp + XI_GP_LY, &sp->ly, 2);
}

/* XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState) */
static void imp_XInputGetState(void)
{
    uint32_t user = W32_ARG(0), st = W32_ARG(1);
    uint32_t r = XI_ERROR_DEVICE_NOT_CONNECTED;
    input_script_start();
    struct input_script_pad sp;
    int scripted = user == 0 && input_script_pad(&sp);
    pthread_mutex_lock(&s_xi_lock);
    SDL_GameController *gc = xi_pad(user);
    if (gc || scripted) {
        uint8_t gp[XI_GP_SIZE] = {0};
        if (gc)
            xi_read(gc, gp);
        if (scripted)
            xi_script(&sp, gp);
        if (memcmp(gp, s_xi[user].last, XI_GP_SIZE) != 0) {
            memcpy(s_xi[user].last, gp, XI_GP_SIZE);
            s_xi[user].packet++;
        }
        MEM32(st + XI_STATE_PACKET) = s_xi[user].packet;
        memcpy(guest_ptr(st + XI_STATE_GAMEPAD), gp, XI_GP_SIZE);
        r = W32_ERROR_SUCCESS;
    }
    /* Trace: the result, buttons and left stick when they change. */
    static uint32_t traced[XUSER_MAX_COUNT][2] = {{~0u}, {~0u}, {~0u}, {~0u}};
    if (user < XUSER_MAX_COUNT) {
        uint16_t b;
        int16_t lx, ly;
        memcpy(&b, s_xi[user].last + XI_GP_BUTTONS, 2);
        memcpy(&lx, s_xi[user].last + XI_GP_LX, 2);
        memcpy(&ly, s_xi[user].last + XI_GP_LY, 2);
        uint32_t key = (uint32_t)b << 16 | (uint32_t)(lx / 8192 + 4) << 4 | (uint32_t)(ly / 8192 + 4);
        if (traced[user][0] != r || traced[user][1] != key) {
            traced[user][0] = r, traced[user][1] = key;
            w32_input_trace("XInputGetState user %u -> %u buttons %04X lx %d ly %d focus %d", user, r, b, lx,
                            ly, SDL_GetKeyboardFocus() != NULL);
        }
    }
    pthread_mutex_unlock(&s_xi_lock);
    W32_RETURN(2, r);
}

/* XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration). The
 * duration is refreshed on every call; 0 on both motors stops. */
static void imp_XInputSetState(void)
{
    uint32_t user = W32_ARG(0), vib = W32_ARG(1);
    uint32_t r = XI_ERROR_DEVICE_NOT_CONNECTED;
    int scripted = user == 0 && input_script_pad(NULL);
    pthread_mutex_lock(&s_xi_lock);
    SDL_GameController *gc = xi_pad(user);
    if (gc) {
        uint16_t lo = MEM16(vib + XI_VIB_LEFT), hi = MEM16(vib + XI_VIB_RIGHT);
        SDL_GameControllerRumble(gc, lo, hi, (lo | hi) ? 0xFFFFu : 0u);
        r = W32_ERROR_SUCCESS;
    } else if (scripted) {
        r = W32_ERROR_SUCCESS; /* no motors */
    }
    pthread_mutex_unlock(&s_xi_lock);
    W32_RETURN(2, r);
}

/* XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
 *                       XINPUT_CAPABILITIES *pCapabilities) */
static void imp_XInputGetCapabilities(void)
{
    uint32_t user = W32_ARG(0), caps = W32_ARG(2);
    uint32_t r = XI_ERROR_DEVICE_NOT_CONNECTED;
    int scripted = user == 0 && input_script_pad(NULL);
    pthread_mutex_lock(&s_xi_lock);
    if (xi_pad(user) || scripted) {
        memset(guest_ptr(caps), 0, XI_CAPS_SIZE);
        MEM8(caps + XI_CAPS_TYPE) = XINPUT_DEVTYPE_GAMEPAD;
        MEM8(caps + XI_CAPS_SUBTYPE) = XINPUT_DEVSUBTYPE_GAMEPAD;
        MEM16(caps + XI_CAPS_FLAGS) = 0;
        MEM16(caps + XI_CAPS_GAMEPAD + XI_GP_BUTTONS) = XI_ALL_BUTTONS;
        MEM8(caps + XI_CAPS_GAMEPAD + XI_GP_LT) = 0xFF;
        MEM8(caps + XI_CAPS_GAMEPAD + XI_GP_RT) = 0xFF;
        for (uint32_t a = XI_GP_LX; a <= XI_GP_RY; a += 2)
            MEM16(caps + XI_CAPS_GAMEPAD + a) = 0xFFFF;
        MEM16(caps + XI_CAPS_VIBRATION + XI_VIB_LEFT) = 0xFFFF;
        MEM16(caps + XI_CAPS_VIBRATION + XI_VIB_RIGHT) = 0xFFFF;
        r = W32_ERROR_SUCCESS;
    }
    static uint32_t traced[XUSER_MAX_COUNT] = {~0u, ~0u, ~0u, ~0u};
    if (user < XUSER_MAX_COUNT && traced[user] != r) {
        traced[user] = r;
        w32_input_trace("XInputGetCapabilities user %u flags %u -> %u", user, W32_ARG(1), r);
    }
    pthread_mutex_unlock(&s_xi_lock);
    W32_RETURN(3, r);
}

/* ---- DirectInput 8: constants and layouts -------------------------------- */

#define DI_OK 0x00000000u
#define DI_NOEFFECT 0x00000001u      /* S_FALSE */
#define DI_BUFFEROVERFLOW 0x00000001u /* S_FALSE */
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_ACQUIRED 0x800700AAu
#define DIERR_NOTBUFFERED 0x80040207u
#define DIERR_DEVICENOTREG COM_REGDB_E_CLASSNOTREG
#define DIERR_INVALIDPARAM COM_E_INVALIDARG

#define DIENUM_STOP 0u
#define DIEDFL_FORCEFEEDBACK 0x00000100u
#define DI8DEVCLASS_ALL 0u
#define DI8DEVCLASS_DEVICE 1u
#define DI8DEVCLASS_POINTER 2u
#define DI8DEVCLASS_KEYBOARD 3u
#define DI8DEVCLASS_GAMECTRL 4u
#define DI8DEVTYPE_MOUSE 0x12u
#define DI8DEVTYPE_KEYBOARD 0x13u
#define DI8DEVTYPE_JOYSTICK 0x14u
#define DI8DEVTYPE_GAMEPAD 0x15u
#define DI8DEVTYPE_FLIGHT 0x17u
#define DI8DEVTYPEGAMEPAD_STANDARD 2u
#define DIDEVTYPE_HID 0x00010000u
#define DI8DEVTYPEMOUSE_TRADITIONAL 2u
#define DI8DEVTYPEKEYBOARD_PCENH 4u
#define DIDC_ATTACHED 0x00000001u
#define DIGDD_PEEK 0x00000001u
#define DIPROP_BUFFERSIZE 1u /* MAKEDIPROP(1): the REFGUID is the number */
#define DIPROP_AXISMODE 2u
#define DIPROPAXISMODE_ABS 0u
#define DIPROPAXISMODE_REL 1u

/* DIDEVICEINSTANCEA, 580 bytes. */
#define DIDI_SIZE 0x244u
#define DIDI_GUID_INSTANCE 0x004u
#define DIDI_GUID_PRODUCT 0x014u
#define DIDI_DEVTYPE 0x024u
#define DIDI_INSTANCE_NAME 0x028u /* char[260] */
#define DIDI_PRODUCT_NAME 0x12Cu  /* char[260] */
#define DIDI_USAGE_PAGE 0x240u
#define DIDI_USAGE 0x242u
/* DIDEVCAPS (44 bytes; the DirectX 3 one is 24). */
#define DIDC_SIZE 44u
#define DIDC_SIZE_DX3 24u
#define DIDC_FLAGS 0x04u
#define DIDC_DEVTYPE 0x08u
#define DIDC_AXES 0x0Cu
#define DIDC_BUTTONS 0x10u
/* DIPROPDWORD: DIPROPHEADER (16 bytes), then dwData. */
#define DIPROPDWORD_DATA 0x10u
/* DIDEVICEOBJECTDATA (20 bytes; the DirectX 3 one, without uAppData, is 16). */
#define DIDOD_SIZE 20u
#define DIDOD_SIZE_DX3 16u
#define DIDOD_OFS 0x00u
#define DIDOD_DATA 0x04u
#define DIDOD_TIME 0x08u
#define DIDOD_SEQ 0x0Cu
#define DIDOD_APPDATA 0x10u
/* DIDATAFORMAT.dwDataSize names the format: c_dfDIKeyboard, c_dfDIMouse,
 * c_dfDIMouse2. */
#define DIDF_DATASIZE 0x0Cu
#define DF_KEYBOARD 256u
#define DF_MOUSE 16u
#define DF_MOUSE2 20u
/* DIMOUSESTATE(2): lX, lY, lZ, rgbButtons[4 or 8]. DIMOFS_* are the same. */
#define DIMOFS_X 0x00u
#define DIMOFS_Y 0x04u
#define DIMOFS_Z 0x08u
#define DIMOFS_BUTTON0 0x0Cu
#define WHEEL_DELTA 120

static const com_guid_t IID_IDirectInput8A = {
    0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const com_guid_t IID_IDirectInputDevice8A = {
    0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
static const com_guid_t IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t GUID_SysMouse = {
    0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const com_guid_t GUID_SysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

/* SDL scancode to DIK code: the PC/AT set-1 make code, E0-prefixed keys as
 * 0x80 | code, values as in dinput.h. 0 means no DIK key. */
static const uint8_t DIK_OF[SDL_NUM_SCANCODES] = {
    [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03,
    [SDL_SCANCODE_3] = 0x04, [SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06,
    [SDL_SCANCODE_6] = 0x07, [SDL_SCANCODE_7] = 0x08, [SDL_SCANCODE_8] = 0x09,
    [SDL_SCANCODE_9] = 0x0A, [SDL_SCANCODE_0] = 0x0B, [SDL_SCANCODE_MINUS] = 0x0C,
    [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB] = 0x0F,
    [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_E] = 0x12,
    [SDL_SCANCODE_R] = 0x13, [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_Y] = 0x15,
    [SDL_SCANCODE_U] = 0x16, [SDL_SCANCODE_I] = 0x17, [SDL_SCANCODE_O] = 0x18,
    [SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_LEFTBRACKET] = 0x1A,
    [SDL_SCANCODE_RIGHTBRACKET] = 0x1B, [SDL_SCANCODE_RETURN] = 0x1C,
    [SDL_SCANCODE_LCTRL] = 0x1D, [SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_S] = 0x1F,
    [SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_F] = 0x21, [SDL_SCANCODE_G] = 0x22,
    [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25,
    [SDL_SCANCODE_L] = 0x26, [SDL_SCANCODE_SEMICOLON] = 0x27,
    [SDL_SCANCODE_APOSTROPHE] = 0x28, [SDL_SCANCODE_GRAVE] = 0x29,
    [SDL_SCANCODE_LSHIFT] = 0x2A, [SDL_SCANCODE_BACKSLASH] = 0x2B,
    [SDL_SCANCODE_NONUSHASH] = 0x2B, /* ISO '#': same key position as '\' */
    [SDL_SCANCODE_Z] = 0x2C, [SDL_SCANCODE_X] = 0x2D, [SDL_SCANCODE_C] = 0x2E,
    [SDL_SCANCODE_V] = 0x2F, [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_N] = 0x31,
    [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_COMMA] = 0x33, [SDL_SCANCODE_PERIOD] = 0x34,
    [SDL_SCANCODE_SLASH] = 0x35, [SDL_SCANCODE_RSHIFT] = 0x36,
    [SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_LALT] = 0x38,
    [SDL_SCANCODE_SPACE] = 0x39, [SDL_SCANCODE_CAPSLOCK] = 0x3A,
    [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D,
    [SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40,
    [SDL_SCANCODE_F7] = 0x41, [SDL_SCANCODE_F8] = 0x42, [SDL_SCANCODE_F9] = 0x43,
    [SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_NUMLOCKCLEAR] = 0x45,
    [SDL_SCANCODE_SCROLLLOCK] = 0x46, [SDL_SCANCODE_KP_7] = 0x47, [SDL_SCANCODE_KP_8] = 0x48,
    [SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_MINUS] = 0x4A, [SDL_SCANCODE_KP_4] = 0x4B,
    [SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D, [SDL_SCANCODE_KP_PLUS] = 0x4E,
    [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50, [SDL_SCANCODE_KP_3] = 0x51,
    [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53,
    [SDL_SCANCODE_NONUSBACKSLASH] = 0x56, /* DIK_OEM_102 */
    [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58, [SDL_SCANCODE_F13] = 0x64,
    [SDL_SCANCODE_F14] = 0x65, [SDL_SCANCODE_F15] = 0x66,
    [SDL_SCANCODE_INTERNATIONAL2] = 0x70, /* DIK_KANA */
    [SDL_SCANCODE_INTERNATIONAL1] = 0x73, /* DIK_ABNT_C1 (JIS Ro) */
    [SDL_SCANCODE_INTERNATIONAL4] = 0x79, /* DIK_CONVERT */
    [SDL_SCANCODE_INTERNATIONAL5] = 0x7B, /* DIK_NOCONVERT */
    [SDL_SCANCODE_INTERNATIONAL3] = 0x7D, /* DIK_YEN */
    [SDL_SCANCODE_KP_EQUALS] = 0x8D, [SDL_SCANCODE_AUDIOPREV] = 0x90,
    [SDL_SCANCODE_AUDIONEXT] = 0x99, [SDL_SCANCODE_KP_ENTER] = 0x9C,
    [SDL_SCANCODE_RCTRL] = 0x9D, [SDL_SCANCODE_MUTE] = 0xA0, [SDL_SCANCODE_AUDIOMUTE] = 0xA0,
    [SDL_SCANCODE_CALCULATOR] = 0xA1, [SDL_SCANCODE_AUDIOPLAY] = 0xA2,
    [SDL_SCANCODE_AUDIOSTOP] = 0xA4, [SDL_SCANCODE_VOLUMEDOWN] = 0xAE,
    [SDL_SCANCODE_VOLUMEUP] = 0xB0, [SDL_SCANCODE_AC_HOME] = 0xB2,
    [SDL_SCANCODE_KP_COMMA] = 0xB3, [SDL_SCANCODE_KP_DIVIDE] = 0xB5,
    [SDL_SCANCODE_PRINTSCREEN] = 0xB7, [SDL_SCANCODE_SYSREQ] = 0xB7, /* DIK_SYSRQ */
    [SDL_SCANCODE_RALT] = 0xB8, [SDL_SCANCODE_PAUSE] = 0xC5, [SDL_SCANCODE_HOME] = 0xC7,
    [SDL_SCANCODE_UP] = 0xC8, [SDL_SCANCODE_PAGEUP] = 0xC9, [SDL_SCANCODE_LEFT] = 0xCB,
    [SDL_SCANCODE_RIGHT] = 0xCD, [SDL_SCANCODE_END] = 0xCF, [SDL_SCANCODE_DOWN] = 0xD0,
    [SDL_SCANCODE_PAGEDOWN] = 0xD1, [SDL_SCANCODE_INSERT] = 0xD2,
    [SDL_SCANCODE_DELETE] = 0xD3, [SDL_SCANCODE_LGUI] = 0xDB, [SDL_SCANCODE_RGUI] = 0xDC,
    [SDL_SCANCODE_APPLICATION] = 0xDD, [SDL_SCANCODE_POWER] = 0xDE,
    [SDL_SCANCODE_SLEEP] = 0xDF, [SDL_SCANCODE_AC_SEARCH] = 0xE5,
    [SDL_SCANCODE_AC_BOOKMARKS] = 0xE6, [SDL_SCANCODE_AC_REFRESH] = 0xE7,
    [SDL_SCANCODE_AC_STOP] = 0xE8, [SDL_SCANCODE_AC_FORWARD] = 0xE9,
    [SDL_SCANCODE_AC_BACK] = 0xEA, [SDL_SCANCODE_COMPUTER] = 0xEB, [SDL_SCANCODE_MAIL] = 0xEC,
    [SDL_SCANCODE_MEDIASELECT] = 0xED,
};

/* SDL mouse button (1 left, 2 middle, 3 right, 4/5 X1/X2) to DirectInput
 * button (0 left, 1 right, 2 middle, 3/4 X1/X2). */
static int di_button_of(int sdl_button)
{
    static const int map[] = {-1, 0, 2, 1, 3, 4};
    return sdl_button > 0 && sdl_button < 6 ? map[sdl_button] : sdl_button - 1;
}

/* ---- DirectInput 8: host objects ----------------------------------------- */

enum { DEV_KEYBOARD, DEV_MOUSE };

/* One DIDEVICEOBJECTDATA, without uAppData (always 0). */
struct di_event {
    uint32_t ofs, data, time, seq;
};

struct di_device {
    struct di_device *next; /* s_devices list, for the event watch */
    int refs;
    int kind;
    uint32_t format; /* DIDATAFORMAT.dwDataSize, 0 before SetDataFormat */
    int acquired;
    uint32_t buf_size; /* DIPROP_BUFFERSIZE */
    struct di_event *buf; /* ring of buf_size events */
    uint32_t head, count;
    int overflow;
    int32_t wheel; /* lZ since the last GetDeviceState */
    int abs;        /* DIPROP_AXISMODE is DIPROPAXISMODE_ABS */
    int32_t pos[3]; /* lX, lY, lZ in DIPROPAXISMODE_ABS */
};

struct di8 {
    int refs;
};

#define DI_BUFFER_MAX 4096u

/* s_di_lock guards the device list and every device's ring and wheel. The
 * event watch takes it, so never call into SDL with it held. */
static pthread_mutex_t s_di_lock = PTHREAD_MUTEX_INITIALIZER;
static struct di_device *s_devices;
static uint32_t s_seq; /* dwSequence, shared by all devices */
static int s_watch_added;

static void di_record(struct di_device *d, uint32_t ofs, uint32_t data, uint32_t time)
{
    if (!d->acquired || d->buf_size == 0)
        return;
    if (d->count == d->buf_size) {
        d->overflow = 1; /* the newest event is lost */
        return;
    }
    struct di_event *ev = &d->buf[(d->head + d->count) % d->buf_size];
    ev->ofs = ofs;
    ev->data = data;
    ev->time = time;
    ev->seq = ++s_seq;
    d->count++;
}

/* SDL calls this on whichever thread pushes the event. */
static int di_event_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    pthread_mutex_lock(&s_di_lock);
    for (struct di_device *d = s_devices; d; d = d->next) {
        if (d->kind == DEV_KEYBOARD) {
            if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && !e->key.repeat) {
                SDL_Scancode sc = e->key.keysym.scancode;
                if (sc < SDL_NUM_SCANCODES && DIK_OF[sc])
                    di_record(d, DIK_OF[sc], e->type == SDL_KEYDOWN ? 0x80u : 0u,
                              e->key.timestamp);
            }
        } else if (e->type == SDL_MOUSEMOTION) {
            if (e->motion.xrel)
                di_record(d, DIMOFS_X, (uint32_t)e->motion.xrel, e->motion.timestamp);
            if (e->motion.yrel)
                di_record(d, DIMOFS_Y, (uint32_t)e->motion.yrel, e->motion.timestamp);
        } else if (e->type == SDL_MOUSEWHEEL && d->acquired) {
            /* preciseY, not y: sdl2-compat fills y only with whole ticks
             * SDL3 accumulated itself, and 0 for a pushed event. */
            float notches = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.preciseY
                                                                         : e->wheel.preciseY;
            int32_t z = (int32_t)lroundf(notches * WHEEL_DELTA);
            if (z) {
                d->wheel += z;
                di_record(d, DIMOFS_Z, (uint32_t)z, e->wheel.timestamp);
            }
        } else if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) {
            int b = di_button_of(e->button.button);
            if (b >= 0 && b < 8)
                di_record(d, DIMOFS_BUTTON0 + (uint32_t)b,
                          e->type == SDL_MOUSEBUTTONDOWN ? 0x80u : 0u, e->button.timestamp);
        }
    }
    pthread_mutex_unlock(&s_di_lock);
    return 0;
}

static void di_unsupported(const char *iface, const char *method)
{
    diag_unsupported("dinput8", MEM32(g_esp), "%s::%s", iface, method);
}

/* A method that is not implemented: UNSUPPORTED once, E_NOTIMPL. nargs
 * excludes `this`. */
#define DI_NOTIMPL(iface, method, nargs)              \
    static void iface##_##method(void)                \
    {                                                 \
        di_unsupported(#iface, #method);              \
        W32_RETURN(1 + (nargs), COM_E_NOTIMPL);       \
    }

/* ---- IDirectInputDevice8A ------------------------------------------------ */

static const com_interface_t IDirectInputDevice8A_iface;

static struct di_device *dev_self(void)
{
    return com_guest_self(W32_ARG(0));
}

/* QueryInterface(REFIID riid, void **ppvObject): only the interface itself. */
static void IDirectInputDevice8A_QueryInterface(void)
{
    uint32_t self = W32_ARG(0), ppv = W32_ARG(2);
    com_guid_t iid = com_guid_read(W32_ARG(1));
    uint32_t hr = COM_E_NOINTERFACE;
    if (com_guid_eq(&iid, &IID_IDirectInputDevice8A) || com_guid_eq(&iid, &IID_IUnknown)) {
        pthread_mutex_lock(&s_di_lock);
        dev_self()->refs++;
        pthread_mutex_unlock(&s_di_lock);
        hr = COM_S_OK;
    } else {
        char s[39];
        com_guid_str(&iid, s);
        diag_unsupported("dinput8", MEM32(g_esp), "IDirectInputDevice8A::QueryInterface %s", s);
        self = 0;
    }
    if (ppv)
        MEM32(ppv) = self;
    W32_RETURN(3, hr);
}

static void IDirectInputDevice8A_AddRef(void)
{
    pthread_mutex_lock(&s_di_lock);
    int refs = ++dev_self()->refs;
    pthread_mutex_unlock(&s_di_lock);
    W32_RETURN(1, refs);
}

static void IDirectInputDevice8A_Release(void)
{
    uint32_t self = W32_ARG(0);
    struct di_device *d = dev_self();
    pthread_mutex_lock(&s_di_lock);
    int refs = --d->refs;
    if (refs == 0) {
        struct di_device **p = &s_devices;
        while (*p != d)
            p = &(*p)->next;
        *p = d->next;
    }
    pthread_mutex_unlock(&s_di_lock);
    if (refs == 0) {
        com_guest_free(self);
        free(d->buf);
        free(d);
    }
    W32_RETURN(1, refs);
}

static uint32_t dev_type(int kind)
{
    return kind == DEV_KEYBOARD ? DI8DEVTYPE_KEYBOARD | (DI8DEVTYPEKEYBOARD_PCENH << 8)
                                   : DI8DEVTYPE_MOUSE | (DI8DEVTYPEMOUSE_TRADITIONAL << 8);
}

/* GetCapabilities(LPDIDEVCAPS lpDIDevCaps) */
static void IDirectInputDevice8A_GetCapabilities(void)
{
    struct di_device *d = dev_self();
    uint32_t caps = W32_ARG(1);
    uint32_t size = MEM32(caps);
    if (size != DIDC_SIZE && size != DIDC_SIZE_DX3) {
        W32_RETURN(2, DIERR_INVALIDPARAM);
        return;
    }
    memset(guest_ptr(caps + 4), 0, size - 4);
    MEM32(caps + DIDC_FLAGS) = DIDC_ATTACHED;
    MEM32(caps + DIDC_DEVTYPE) = dev_type(d->kind);
    MEM32(caps + DIDC_AXES) = d->kind == DEV_MOUSE ? 3 : 0;
    MEM32(caps + DIDC_BUTTONS) = d->kind == DEV_MOUSE ? 8 : 128;
    W32_RETURN(2, DI_OK);
}

/* GetProperty(REFGUID rguidProp, LPDIPROPHEADER pdiph) */
static void IDirectInputDevice8A_GetProperty(void)
{
    struct di_device *d = dev_self();
    uint32_t prop = W32_ARG(1), ph = W32_ARG(2);
    if (prop != DIPROP_BUFFERSIZE) {
        diag_unsupported("dinput8", MEM32(g_esp), "IDirectInputDevice8A::GetProperty 0x%X", prop);
        W32_RETURN(3, COM_E_NOTIMPL);
        return;
    }
    MEM32(ph + DIPROPDWORD_DATA) = d->buf_size;
    W32_RETURN(3, DI_OK);
}

/* SetProperty(REFGUID rguidProp, LPCDIPROPHEADER pdiph): DIPROP_BUFFERSIZE,
 * and DIPROP_AXISMODE, which only the mouse state reads. */
static void IDirectInputDevice8A_SetProperty(void)
{
    struct di_device *d = dev_self();
    uint32_t prop = W32_ARG(1), ph = W32_ARG(2);
    if (prop == DIPROP_AXISMODE) {
        uint32_t mode = MEM32(ph + DIPROPDWORD_DATA);
        uint32_t hr = d->acquired ? DIERR_ACQUIRED
                      : mode == DIPROPAXISMODE_ABS || mode == DIPROPAXISMODE_REL
                          ? DI_OK
                          : DIERR_INVALIDPARAM;
        if (hr == DI_OK)
            d->abs = mode == DIPROPAXISMODE_ABS;
        W32_RETURN(3, hr);
        return;
    }
    if (prop != DIPROP_BUFFERSIZE) {
        diag_unsupported("dinput8", MEM32(g_esp), "IDirectInputDevice8A::SetProperty 0x%X", prop);
        W32_RETURN(3, COM_E_NOTIMPL);
        return;
    }
    if (d->acquired) {
        W32_RETURN(3, DIERR_ACQUIRED);
        return;
    }
    uint32_t n = MEM32(ph + DIPROPDWORD_DATA);
    if (n > DI_BUFFER_MAX) {
        diag_unsupported("dinput8", MEM32(g_esp),
                         "IDirectInputDevice8A::SetProperty buffer size %u capped at %u", n,
                         DI_BUFFER_MAX);
        n = DI_BUFFER_MAX;
    }
    struct di_event *buf = n ? calloc(n, sizeof *buf) : NULL;
    pthread_mutex_lock(&s_di_lock);
    free(d->buf);
    d->buf = buf;
    d->buf_size = n;
    d->head = d->count = 0;
    d->overflow = 0;
    pthread_mutex_unlock(&s_di_lock);
    W32_RETURN(3, DI_OK);
}

static void IDirectInputDevice8A_Acquire(void)
{
    struct di_device *d = dev_self();
    if (d->format == 0) {
        W32_RETURN(1, DIERR_INVALIDPARAM);
        return;
    }
    if (d->acquired) {
        W32_RETURN(1, DI_NOEFFECT);
        return;
    }
    if (d->kind == DEV_MOUSE)
        SDL_GetRelativeMouseState(NULL, NULL); /* motion counts from here */
    pthread_mutex_lock(&s_di_lock);
    d->acquired = 1;
    d->head = d->count = 0;
    d->overflow = 0;
    d->wheel = 0;
    pthread_mutex_unlock(&s_di_lock);
    W32_RETURN(1, DI_OK);
}

static void IDirectInputDevice8A_Unacquire(void)
{
    struct di_device *d = dev_self();
    uint32_t hr = d->acquired ? DI_OK : DI_NOEFFECT;
    pthread_mutex_lock(&s_di_lock);
    d->acquired = 0;
    pthread_mutex_unlock(&s_di_lock);
    W32_RETURN(1, hr);
}

/* GetDeviceState(DWORD cbData, LPVOID lpvData) */
static void IDirectInputDevice8A_GetDeviceState(void)
{
    struct di_device *d = dev_self();
    uint32_t cb = W32_ARG(1), out = W32_ARG(2);
    if (!d->acquired) {
        W32_RETURN(3, DIERR_NOTACQUIRED);
        return;
    }
    if (cb != d->format) {
        W32_RETURN(3, DIERR_INVALIDPARAM);
        return;
    }
    memset(guest_ptr(out), 0, cb);
    if (d->kind == DEV_KEYBOARD) {
        int n = 0;
        const Uint8 *keys = SDL_GetKeyboardState(&n);
        for (int sc = 0; sc < n && sc < SDL_NUM_SCANCODES; sc++)
            if (keys[sc] && DIK_OF[sc] && !w32_ui_key_hidden(sc))
                MEM8(out + DIK_OF[sc]) = 0x80;
        input_script_keys((uint8_t *)guest_ptr(out));
    } else {
        int x = 0, y = 0;
        uint32_t held = SDL_GetRelativeMouseState(&x, &y);
        pthread_mutex_lock(&s_di_lock);
        int32_t z = d->wheel;
        d->wheel = 0;
        pthread_mutex_unlock(&s_di_lock);
        if (d->abs) {
            /* Wraps like DirectInput's 32-bit position. */
            x = (int32_t)((uint32_t)d->pos[0] + (uint32_t)x);
            y = (int32_t)((uint32_t)d->pos[1] + (uint32_t)y);
            z = (int32_t)((uint32_t)d->pos[2] + (uint32_t)z);
            d->pos[0] = x, d->pos[1] = y, d->pos[2] = z;
        }
        MEM32(out + DIMOFS_X) = (uint32_t)x;
        MEM32(out + DIMOFS_Y) = (uint32_t)y;
        MEM32(out + DIMOFS_Z) = (uint32_t)z;
        uint32_t n_buttons = cb - DIMOFS_BUTTON0;
        for (int sdl = 1; sdl <= 32; sdl++) {
            int b = di_button_of(sdl);
            if ((held & SDL_BUTTON(sdl)) && b >= 0 && (uint32_t)b < n_buttons)
                MEM8(out + DIMOFS_BUTTON0 + (uint32_t)b) = 0x80;
        }
    }
    W32_RETURN(3, DI_OK);
}

/* GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod,
 *               LPDWORD pdwInOut, DWORD dwFlags) */
static void IDirectInputDevice8A_GetDeviceData(void)
{
    struct di_device *d = dev_self();
    uint32_t cb = W32_ARG(1), out = W32_ARG(2), inout = W32_ARG(3), flags = W32_ARG(4);
    if (!d->acquired) {
        W32_RETURN(5, DIERR_NOTACQUIRED);
        return;
    }
    if (cb != DIDOD_SIZE && cb != DIDOD_SIZE_DX3) {
        W32_RETURN(5, DIERR_INVALIDPARAM);
        return;
    }
    if (d->buf_size == 0) {
        W32_RETURN(5, DIERR_NOTBUFFERED);
        return;
    }
    pthread_mutex_lock(&s_di_lock);
    uint32_t n = MEM32(inout) < d->count ? MEM32(inout) : d->count;
    for (uint32_t i = 0; out && i < n; i++) {
        const struct di_event *ev = &d->buf[(d->head + i) % d->buf_size];
        uint32_t o = out + i * cb;
        MEM32(o + DIDOD_OFS) = ev->ofs;
        MEM32(o + DIDOD_DATA) = ev->data;
        MEM32(o + DIDOD_TIME) = ev->time;
        MEM32(o + DIDOD_SEQ) = ev->seq;
        if (cb == DIDOD_SIZE)
            MEM32(o + DIDOD_APPDATA) = 0;
    }
    uint32_t hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
        d->head = (d->head + n) % d->buf_size;
        d->count -= n;
        d->overflow = 0;
    }
    pthread_mutex_unlock(&s_di_lock);
    MEM32(inout) = n;
    W32_RETURN(5, hr);
}

/* SetDataFormat(LPCDIDATAFORMAT lpdf): the format is known by its size. */
static void IDirectInputDevice8A_SetDataFormat(void)
{
    struct di_device *d = dev_self();
    uint32_t size = MEM32(W32_ARG(1) + DIDF_DATASIZE);
    int ok = d->kind == DEV_KEYBOARD ? size == DF_KEYBOARD
                                     : size == DF_MOUSE || size == DF_MOUSE2;
    if (d->acquired) {
        W32_RETURN(2, DIERR_ACQUIRED);
        return;
    }
    if (!ok) {
        diag_unsupported("dinput8", MEM32(g_esp),
                         "IDirectInputDevice8A::SetDataFormat dwDataSize %u", size);
        W32_RETURN(2, DIERR_INVALIDPARAM);
        return;
    }
    d->format = size;
    W32_RETURN(2, DI_OK);
}

/* SetCooperativeLevel(HWND hwnd, DWORD dwFlags): any level. */
static void IDirectInputDevice8A_SetCooperativeLevel(void)
{
    W32_RETURN(3, DI_OK);
}

static void IDirectInputDevice8A_Poll(void)
{
    W32_RETURN(1, DI_NOEFFECT);
}

DI_NOTIMPL(IDirectInputDevice8A, EnumObjects, 3)
DI_NOTIMPL(IDirectInputDevice8A, SetEventNotification, 1)
DI_NOTIMPL(IDirectInputDevice8A, GetObjectInfo, 3)
DI_NOTIMPL(IDirectInputDevice8A, GetDeviceInfo, 1)
DI_NOTIMPL(IDirectInputDevice8A, RunControlPanel, 2)
DI_NOTIMPL(IDirectInputDevice8A, Initialize, 3)
DI_NOTIMPL(IDirectInputDevice8A, CreateEffect, 4)
DI_NOTIMPL(IDirectInputDevice8A, EnumEffects, 3)
DI_NOTIMPL(IDirectInputDevice8A, GetEffectInfo, 2)
DI_NOTIMPL(IDirectInputDevice8A, GetForceFeedbackState, 1)
DI_NOTIMPL(IDirectInputDevice8A, SendForceFeedbackCommand, 1)
DI_NOTIMPL(IDirectInputDevice8A, EnumCreatedEffectObjects, 3)
DI_NOTIMPL(IDirectInputDevice8A, Escape, 1)
DI_NOTIMPL(IDirectInputDevice8A, SendDeviceData, 4)
DI_NOTIMPL(IDirectInputDevice8A, EnumEffectsInFile, 4)
DI_NOTIMPL(IDirectInputDevice8A, WriteEffectToFile, 4)
DI_NOTIMPL(IDirectInputDevice8A, BuildActionMap, 3)
DI_NOTIMPL(IDirectInputDevice8A, SetActionMap, 3)
DI_NOTIMPL(IDirectInputDevice8A, GetImageInfo, 1)

/* dinput.h order (IDirectInputDevice8A, 32 methods). */
static const recomp_func_t IDirectInputDevice8A_methods[] = {
    IDirectInputDevice8A_QueryInterface,
    IDirectInputDevice8A_AddRef,
    IDirectInputDevice8A_Release,
    IDirectInputDevice8A_GetCapabilities,
    IDirectInputDevice8A_EnumObjects,
    IDirectInputDevice8A_GetProperty,
    IDirectInputDevice8A_SetProperty,
    IDirectInputDevice8A_Acquire,
    IDirectInputDevice8A_Unacquire,
    IDirectInputDevice8A_GetDeviceState,
    IDirectInputDevice8A_GetDeviceData,
    IDirectInputDevice8A_SetDataFormat,
    IDirectInputDevice8A_SetEventNotification,
    IDirectInputDevice8A_SetCooperativeLevel,
    IDirectInputDevice8A_GetObjectInfo,
    IDirectInputDevice8A_GetDeviceInfo,
    IDirectInputDevice8A_RunControlPanel,
    IDirectInputDevice8A_Initialize,
    IDirectInputDevice8A_CreateEffect,
    IDirectInputDevice8A_EnumEffects,
    IDirectInputDevice8A_GetEffectInfo,
    IDirectInputDevice8A_GetForceFeedbackState,
    IDirectInputDevice8A_SendForceFeedbackCommand,
    IDirectInputDevice8A_EnumCreatedEffectObjects,
    IDirectInputDevice8A_Escape,
    IDirectInputDevice8A_Poll,
    IDirectInputDevice8A_SendDeviceData,
    IDirectInputDevice8A_EnumEffectsInFile,
    IDirectInputDevice8A_WriteEffectToFile,
    IDirectInputDevice8A_BuildActionMap,
    IDirectInputDevice8A_SetActionMap,
    IDirectInputDevice8A_GetImageInfo,
};
static const com_interface_t IDirectInputDevice8A_iface = {
    "IDirectInputDevice8A", IDirectInputDevice8A_methods,
    sizeof IDirectInputDevice8A_methods / sizeof IDirectInputDevice8A_methods[0]};

/* ---- IDirectInput8A ------------------------------------------------------ */

/* DEV_KEYBOARD, DEV_MOUSE or -1 for the GUID at guest address g. */
static int di_kind_of(uint32_t g)
{
    com_guid_t guid = com_guid_read(g);
    if (com_guid_eq(&guid, &GUID_SysKeyboard))
        return DEV_KEYBOARD;
    if (com_guid_eq(&guid, &GUID_SysMouse))
        return DEV_MOUSE;
    return -1;
}

static void IDirectInput8A_QueryInterface(void)
{
    uint32_t self = W32_ARG(0), ppv = W32_ARG(2);
    com_guid_t iid = com_guid_read(W32_ARG(1));
    uint32_t hr = COM_E_NOINTERFACE;
    if (com_guid_eq(&iid, &IID_IDirectInput8A) || com_guid_eq(&iid, &IID_IUnknown)) {
        pthread_mutex_lock(&s_di_lock);
        ((struct di8 *)com_guest_self(self))->refs++;
        pthread_mutex_unlock(&s_di_lock);
        hr = COM_S_OK;
    } else {
        char s[39];
        com_guid_str(&iid, s);
        diag_unsupported("dinput8", MEM32(g_esp), "IDirectInput8A::QueryInterface %s", s);
        self = 0;
    }
    if (ppv)
        MEM32(ppv) = self;
    W32_RETURN(3, hr);
}

static void IDirectInput8A_AddRef(void)
{
    struct di8 *di = com_guest_self(W32_ARG(0));
    pthread_mutex_lock(&s_di_lock);
    int refs = ++di->refs;
    pthread_mutex_unlock(&s_di_lock);
    W32_RETURN(1, refs);
}

static void IDirectInput8A_Release(void)
{
    uint32_t self = W32_ARG(0);
    struct di8 *di = com_guest_self(self);
    pthread_mutex_lock(&s_di_lock);
    int refs = --di->refs;
    pthread_mutex_unlock(&s_di_lock);
    if (refs == 0) {
        com_guest_free(self);
        free(di);
    }
    W32_RETURN(1, refs);
}

/* CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice,
 *              LPUNKNOWN pUnkOuter) */
static void IDirectInput8A_CreateDevice(void)
{
    uint32_t out = W32_ARG(2);
    int kind = di_kind_of(W32_ARG(1));
    if (kind < 0) {
        if (out)
            MEM32(out) = 0;
        W32_RETURN(4, DIERR_DEVICENOTREG);
        return;
    }
    struct di_device *d = calloc(1, sizeof *d);
    d->refs = 1;
    d->kind = kind;
    uint32_t obj = com_guest_new(&IDirectInputDevice8A_iface, d);
    pthread_mutex_lock(&s_di_lock);
    d->next = s_devices;
    s_devices = d;
    int add_watch = !s_watch_added;
    s_watch_added = 1;
    pthread_mutex_unlock(&s_di_lock);
    if (add_watch)
        SDL_AddEventWatch(di_event_watch, NULL);
    MEM32(out) = obj;
    W32_RETURN(4, DI_OK);
}

/* Filter of EnumDevices: a DI8DEVCLASS_* or a DI8DEVTYPE_* value. */
static int di_type_matches(uint32_t filter, uint32_t devtype)
{
    switch (filter & 0xFFu) {
    case DI8DEVCLASS_ALL: return 1;
    case DI8DEVCLASS_POINTER: return (devtype & 0xFFu) == DI8DEVTYPE_MOUSE;
    case DI8DEVCLASS_KEYBOARD: return (devtype & 0xFFu) == DI8DEVTYPE_KEYBOARD;
    case DI8DEVCLASS_GAMECTRL:
        return (devtype & 0xFFu) >= DI8DEVTYPE_JOYSTICK && (devtype & 0xFFu) <= DI8DEVTYPE_FLIGHT;
    case DI8DEVCLASS_DEVICE: return 0;
    default: return (filter & 0xFFu) == (devtype & 0xFFu);
    }
}

static void guid_write(uint32_t a, const com_guid_t *g)
{
    MEM32(a) = g->d1;
    MEM16(a + 4) = g->d2;
    MEM16(a + 6) = g->d3;
    memcpy(guest_ptr(a + 8), g->d4, 8);
}

/* EnumDevices(DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback,
 *             LPVOID pvRef, DWORD dwFlags): the system keyboard and mouse,
 * then the pads (w32_input_pads) as Xbox 360 controllers.
 * The callback is BOOL CALLBACK cb(LPCDIDEVICEINSTANCEA, LPVOID). */
static void IDirectInput8A_EnumDevices(void)
{
    uint32_t filter = W32_ARG(1), cb = W32_ARG(2), ref = W32_ARG(3), flags = W32_ARG(4);
    static const struct {
        const com_guid_t *guid;
        int kind;
        const char *name;
        uint16_t usage;
    } DEVS[] = {
        {&GUID_SysKeyboard, DEV_KEYBOARD, "Keyboard", 6},
        {&GUID_SysMouse, DEV_MOUSE, "Mouse", 2},
    };
    /* guidProduct of an Xbox 360 pad: Data1 is MAKELONG(VID 045E, PID 028E),
     * the value a game matches against the WMI DeviceID. guidInstance is
     * the pad number with "PADINST" after it: unique per pad, stable for the
     * session. */
    static const com_guid_t GUID_X360 = {
        0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D'}};
    static const char PAD_NAME[] = "Controller (XBOX 360 For Windows)";
    uint32_t pad_type = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    uint32_t n_devs = sizeof DEVS / sizeof DEVS[0];
    uint32_t n_pads = di_type_matches(filter, pad_type) ? w32_input_pads() : 0;
    uint32_t saved = g_esp;
    uint32_t inst = (g_esp - DIDI_SIZE) & ~15u; /* below our frame */
    g_esp = inst;
    for (uint32_t i = 0; i < n_devs + n_pads; i++) {
        int pad = i >= n_devs;
        uint32_t type = pad ? pad_type : dev_type(DEVS[i].kind);
        if (!di_type_matches(filter, type) || (flags & DIEDFL_FORCEFEEDBACK))
            continue;
        memset(guest_ptr(inst), 0, DIDI_SIZE);
        MEM32(inst) = DIDI_SIZE;
        if (pad) {
            com_guid_t id = {i - n_devs, 0, 0, {0, 'P', 'A', 'D', 'I', 'N', 'S', 'T'}};
            guid_write(inst + DIDI_GUID_INSTANCE, &id);
            guid_write(inst + DIDI_GUID_PRODUCT, &GUID_X360);
        } else {
            guid_write(inst + DIDI_GUID_INSTANCE, DEVS[i].guid);
            guid_write(inst + DIDI_GUID_PRODUCT, DEVS[i].guid);
        }
        MEM32(inst + DIDI_DEVTYPE) = type;
        strcpy((char *)guest_ptr(inst + DIDI_INSTANCE_NAME), pad ? PAD_NAME : DEVS[i].name);
        strcpy((char *)guest_ptr(inst + DIDI_PRODUCT_NAME), pad ? PAD_NAME : DEVS[i].name);
        MEM16(inst + DIDI_USAGE_PAGE) = 1; /* generic desktop */
        MEM16(inst + DIDI_USAGE) = pad ? 5 : DEVS[i].usage;
        uint32_t args[2] = {inst, ref};
        uint32_t more = guest_call_cdecl(cb, 2, args);
        if (pad)
            w32_input_trace("EnumDevices pad %u -> callback %u", i - n_devs, more);
        if (more == DIENUM_STOP)
            break;
    }
    w32_input_trace("EnumDevices filter %u flags %X: %u pads", filter, flags, n_pads);
    g_esp = saved;
    W32_RETURN(5, DI_OK);
}

/* GetDeviceStatus(REFGUID rguidInstance) */
static void IDirectInput8A_GetDeviceStatus(void)
{
    W32_RETURN(2, di_kind_of(W32_ARG(1)) >= 0 ? DI_OK : DIERR_DEVICENOTREG);
}

DI_NOTIMPL(IDirectInput8A, RunControlPanel, 2)
DI_NOTIMPL(IDirectInput8A, Initialize, 2)
DI_NOTIMPL(IDirectInput8A, FindDevice, 3)
DI_NOTIMPL(IDirectInput8A, EnumDevicesBySemantics, 5)
DI_NOTIMPL(IDirectInput8A, ConfigureDevices, 4)

/* dinput.h order (IDirectInput8A, 11 methods). */
static const recomp_func_t IDirectInput8A_methods[] = {
    IDirectInput8A_QueryInterface,
    IDirectInput8A_AddRef,
    IDirectInput8A_Release,
    IDirectInput8A_CreateDevice,
    IDirectInput8A_EnumDevices,
    IDirectInput8A_GetDeviceStatus,
    IDirectInput8A_RunControlPanel,
    IDirectInput8A_Initialize,
    IDirectInput8A_FindDevice,
    IDirectInput8A_EnumDevicesBySemantics,
    IDirectInput8A_ConfigureDevices,
};
static const com_interface_t IDirectInput8A_iface = {
    "IDirectInput8A", IDirectInput8A_methods,
    sizeof IDirectInput8A_methods / sizeof IDirectInput8A_methods[0]};

/* DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
 *                    LPVOID *ppvOut, LPUNKNOWN punkOuter) */
static void imp_DirectInput8Create(void)
{
    uint32_t version = W32_ARG(1), out = W32_ARG(3);
    com_guid_t iid = com_guid_read(W32_ARG(2));
    input_script_start();
    if (version != 0x0800) {
        THUNK_UNSUPPORTED("dwVersion 0x%X", version);
        MEM32(out) = 0;
        W32_RETURN(5, DIERR_INVALIDPARAM);
        return;
    }
    if (!com_guid_eq(&iid, &IID_IDirectInput8A)) {
        MEM32(out) = 0;
        W32_RETURN(5, COM_E_NOINTERFACE);
        return;
    }
    struct di8 *di = calloc(1, sizeof *di);
    di->refs = 1;
    MEM32(out) = com_guest_new(&IDirectInput8A_iface, di);
    W32_RETURN(5, DI_OK);
}

/* ---- Export table -------------------------------------------------------- */

const win32_export_t win32_exports_input[] = {
    {"DINPUT8.dll", "DirectInput8Create", 0, imp_DirectInput8Create},
    {"XINPUT1_3.dll", NULL, 2, imp_XInputGetState},
    {"XINPUT1_3.dll", NULL, 3, imp_XInputSetState},
    {"XINPUT1_3.dll", NULL, 4, imp_XInputGetCapabilities},
    {0},
};
