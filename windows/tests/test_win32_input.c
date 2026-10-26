/* Win32 input slice test: the XInput thunks, DirectInput8Create and every
 * method of IDirectInput8A and IDirectInputDevice8A. Guest stack as in the
 * engine's THUNK_SLICES.md. Links win32_input.c with guest_mem.c,
 * host_funcs.c, guest_call.c and recomp_glue.c; com_guest.h is doubled below,
 * win32_core.c is not linked; input_script.c is. Run with SDL_VIDEODRIVER=dummy. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "com_guest.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "input_script.h"
#include "win32_ui.h"
#include "recomp_types.h"
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

/* ---- Guest memory ------------------------------------------------------ */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FE0000u
#define DATA_AREA 0x00FE0000u /* argument structs */
#define OBJ_AREA 0x00FF0000u  /* COM objects, 8 bytes each */
#define VT_AREA 0x00FF8000u   /* vtables */
#define RET_ADDR 0xDEAD0001u

static uint32_t g_data_next = DATA_AREA;
static uint32_t guest_alloc(uint32_t size)
{
    uint32_t a = g_data_next;
    memset(guest_ptr(a), 0, size);
    g_data_next += (size + 15u) & ~15u;
    return a;
}

/* ---- win32_ui.c double: no key is hidden here (test_win32_ui covers it). */

int w32_ui_key_hidden(int scancode)
{
    (void)scancode;
    return 0;
}

/* ---- com_guest.h double ------------------------------------------------ */

#define MAX_OBJS 64
static struct {
    const com_interface_t *iface;
    void *self;
    int live;
} g_objs[MAX_OBJS];
static int g_n_objs, g_freed;
static struct { const com_interface_t *iface; uint32_t va; } g_vts[4];
static uint32_t g_vt_next = VT_AREA;

uint32_t com_guest_new(const com_interface_t *iface, void *host_self)
{
    uint32_t vt = 0;
    for (int i = 0; i < 4 && !vt; i++) {
        if (g_vts[i].iface == iface) {
            vt = g_vts[i].va;
        } else if (!g_vts[i].iface) {
            vt = g_vt_next;
            for (uint32_t m = 0; m < iface->n_methods; m++)
                MEM32(vt + 4u * m) = host_func_va(iface->methods[m]);
            g_vt_next += 4u * iface->n_methods;
            g_vts[i].iface = iface;
            g_vts[i].va = vt;
        }
    }
    int i = g_n_objs++;
    g_objs[i].iface = iface;
    g_objs[i].self = host_self;
    g_objs[i].live = 1;
    uint32_t obj = OBJ_AREA + 8u * (uint32_t)i;
    MEM32(obj) = vt;
    MEM32(obj + 4) = (uint32_t)i;
    return obj;
}
void *com_guest_self(uint32_t guest_this)
{
    uint32_t i = MEM32(guest_this + 4);
    if (i >= (uint32_t)g_n_objs || !g_objs[i].live) {
        fprintf(stderr, "UNSUPPORTED com: bad object 0x%08X\n", guest_this);
        abort();
    }
    return g_objs[i].self;
}
const com_interface_t *com_guest_iface(uint32_t guest_this)
{
    return g_objs[MEM32(guest_this + 4)].iface;
}
void com_guest_free(uint32_t guest_this)
{
    g_objs[MEM32(guest_this + 4)].live = 0;
    g_freed++;
}
com_guid_t com_guid_read(uint32_t a)
{
    com_guid_t g;
    g.d1 = MEM32(a);
    g.d2 = MEM16(a + 4);
    g.d3 = MEM16(a + 6);
    memcpy(g.d4, guest_ptr(a + 8), 8);
    return g;
}
int com_guid_eq(const com_guid_t *a, const com_guid_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}
void com_guid_str(const com_guid_t *g, char out[39])
{
    snprintf(out, 39, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->d1, g->d2,
             g->d3, g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6],
             g->d4[7]);
}

/* ---- Calls ------------------------------------------------------------- */

/* Push args right to left and the return address, call fn, check that it
 * popped 4 + 4*nargs bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call(recomp_func_t fn, unsigned nargs, const uint32_t *args)
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
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBEBEBEBEu);
    return g_eax;
}
#define CALL(fn, ...)                                                            \
    call((fn), sizeof((uint32_t[]){__VA_ARGS__}) / 4, (const uint32_t[]){__VA_ARGS__})

static recomp_func_t export_fn(const char *name, uint16_t ordinal)
{
    for (const win32_export_t *e = win32_exports_input; e->dll; e++)
        if (name ? e->name && strcmp(e->name, name) == 0 : e->ordinal == ordinal)
            return e->fn;
    CHECK(!"export missing");
    return NULL;
}

/* COM method idx of obj, through its guest vtable. */
static recomp_func_t method(uint32_t obj, unsigned idx)
{
    return guest_resolve(MEM32(MEM32(obj) + 4u * idx));
}

static uint32_t guid_at(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8])
{
    uint32_t a = guest_alloc(16);
    MEM32(a) = d1;
    MEM16(a + 4) = d2;
    MEM16(a + 6) = d3;
    memcpy(guest_ptr(a + 8), d4, 8);
    return a;
}

static char *capture_stderr(int begin)
{
    static FILE *cap;
    static int saved;
    static char buf[4096];
    fflush(stderr);
    if (begin) {
        cap = tmpfile();
        saved = dup(2);
        dup2(fileno(cap), 2);
        return NULL;
    }
    dup2(saved, 2);
    close(saved);
    rewind(cap);
    size_t n = fread(buf, 1, sizeof buf - 1, cap);
    buf[n] = 0;
    fclose(cap);
    return buf;
}

/* ---- XInput ------------------------------------------------------------ */

#define XI_NOT_CONNECTED 1167u

static void test_xinput(void)
{
    recomp_func_t get = export_fn(NULL, 2), set = export_fn(NULL, 3),
                  caps = export_fn(NULL, 4);
    uint32_t st = guest_alloc(16), vib = guest_alloc(4), cp = guest_alloc(20);

    /* Users fill in SDL open order, so the first user past the real pads has none. */
    int real = 0;
    for (int j = 0; j < SDL_NumJoysticks(); j++)
        real += SDL_IsGameController(j);
    uint32_t none = real < 4 ? (uint32_t)real : 4u;
    CHECK(CALL(get, none, st) == XI_NOT_CONNECTED);
    CHECK(CALL(get, 4, st) == XI_NOT_CONNECTED);
    CHECK(CALL(set, none, vib) == XI_NOT_CONNECTED);
    CHECK(CALL(caps, none, 1, cp) == XI_NOT_CONNECTED);
    if (real >= 4)
        return;

    /* A virtual pad, mapped by SDL as a game controller. */
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    int index = SDL_JoystickAttachVirtualEx(&desc);
    CHECK(index >= 0);
    SDL_Joystick *js = SDL_JoystickOpen(index);
    CHECK(js != NULL && SDL_IsGameController(index));
    if (!js)
        return;
    /* Open it as user `none` first: under sdl2-compat a gamepad's triggers
     * start from their minimum when it opens, dropping earlier values. */
    CHECK(CALL(get, none, st) == 0);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTY, -32768); /* full up */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTY, 32767);  /* full down */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);

    CHECK(CALL(get, none, st) == 0);
    uint32_t packet = MEM32(st);
    CHECK(MEM16(st + 4) == (0x1000 | 0x0004 | 0x0100)); /* A, DPAD_LEFT, LEFT_SHOULDER */
    CHECK(MEM8(st + 6) == 255 && MEM8(st + 7) == 0);
    CHECK((int16_t)MEM16(st + 8) == 32767);   /* sThumbLX */
    CHECK((int16_t)MEM16(st + 10) == 32767);  /* sThumbLY: up is positive */
    CHECK((int16_t)MEM16(st + 14) == -32767); /* sThumbRY: down is negative */
    CHECK(CALL(get, none, st) == 0 && MEM32(st) == packet); /* unchanged */
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 0);
    CHECK(CALL(get, none, st) == 0 && MEM32(st) == packet + 1);
    CHECK(MEM16(st + 4) == (0x0004 | 0x0100));

    CHECK(CALL(caps, none, 1, cp) == 0);
    CHECK(MEM8(cp) == 1 && MEM8(cp + 1) == 1 && MEM16(cp + 4) == 0xF3FF);
    CHECK(MEM8(cp + 6) == 0xFF && MEM16(cp + 8) == 0xFFFF && MEM16(cp + 18) == 0xFFFF);

    MEM16(vib) = 0x8000;
    MEM16(vib + 2) = 0x4000;
    CHECK(CALL(set, none, vib) == 0);
    MEM16(vib) = MEM16(vib + 2) = 0;
    CHECK(CALL(set, none, vib) == 0);

    SDL_JoystickClose(js);
    SDL_JoystickDetachVirtual(index);
    CHECK(CALL(get, none, st) == XI_NOT_CONNECTED);
}

/* ---- DirectInput ------------------------------------------------------- */

#define DI_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_ACQUIRED 0x800700AAu
#define DIK_A 0x1E

static const uint8_t SYS_D4[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static uint32_t g_guid_mouse, g_guid_kbd, g_guid_bad;

/* LPDIENUMDEVICESCALLBACKA, called from guest code: stdcall, 2 arguments. */
static struct {
    int n;
    uint32_t d1[4], product[4], type[4], size[4], ref;
    char name[4][16];
    uint32_t stop_after;
} g_enum;
static void enum_cb(void)
{
    uint32_t inst = W32_ARG(0);
    int i = g_enum.n++;
    if (i < 4) {
        g_enum.size[i] = MEM32(inst);
        g_enum.d1[i] = MEM32(inst + 4);
        g_enum.product[i] = MEM32(inst + 0x14);
        g_enum.type[i] = MEM32(inst + 0x24);
        strncpy(g_enum.name[i], (const char *)guest_ptr(inst + 0x28), 15);
    }
    g_enum.ref = W32_ARG(1);
    W32_RETURN(2, (uint32_t)g_enum.n != g_enum.stop_after); /* DIENUM_STOP is 0 */
}

static void enum_devices(uint32_t di, uint32_t filter, uint32_t flags, uint32_t stop_after)
{
    memset(&g_enum, 0, sizeof g_enum);
    g_enum.stop_after = stop_after;
    CHECK(CALL(method(di, 4), di, filter, host_func_va(enum_cb), 0x1234u, flags) == DI_OK);
}

static void push_key(SDL_Scancode sc, int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    CHECK(SDL_PushEvent(&e) == 1);
}

/* Unimplemented methods: index and argument count without `this`. */
static void check_notimpl(uint32_t obj, const char *iface, const unsigned (*m)[2], size_t n,
                          const char *const *names)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t args[6] = {obj};
        capture_stderr(1);
        uint32_t hr = call(method(obj, m[i][0]), 1 + m[i][1], args);
        char *log = capture_stderr(0);
        char want[96];
        snprintf(want, sizeof want, "UNSUPPORTED dinput8: %s::%s", iface, names[i]);
        CHECK(hr == E_NOTIMPL);
        if (!strstr(log, want)) {
            fprintf(stderr, "FAIL: no \"%s\" in \"%s\"\n", want, log);
            g_fail = 1;
        }
    }
}

static void test_dinput(void)
{
    recomp_func_t create = export_fn("DirectInput8Create", 0);
    static const uint8_t di8a_d4[8] = {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
    static const uint8_t di8w_d4[8] = {0x84, 0x82, 0x0C, 0x7E, 0xD7, 0xD4, 0xEC, 0x79};
    uint32_t iid_a = guid_at(0xBF798030u, 0x483A, 0x4DA2, di8a_d4);
    uint32_t iid_w = guid_at(0xBF798031u, 0x483A, 0x4DA2, di8w_d4);
    g_guid_mouse = guid_at(0x6F1D2B60u, 0xD5A0, 0x11CF, SYS_D4);
    g_guid_kbd = guid_at(0x6F1D2B61u, 0xD5A0, 0x11CF, SYS_D4);
    g_guid_bad = guid_at(0x6F1D2B70u, 0xD5A0, 0x11CF, SYS_D4); /* GUID_Joystick */
    uint32_t out = guest_alloc(4);

    MEM32(out) = 0xFFFFFFFFu;
    CHECK(CALL(create, 0x00400000u, 0x0800, iid_w, out, 0) == E_NOINTERFACE);
    CHECK(MEM32(out) == 0);
    CHECK(CALL(create, 0x00400000u, 0x0800, iid_a, out, 0) == DI_OK);
    uint32_t di = MEM32(out);
    CHECK(di != 0);

    /* IUnknown */
    CHECK(CALL(method(di, 1), di) == 2);
    CHECK(CALL(method(di, 2), di) == 1);
    CHECK(CALL(method(di, 0), di, iid_a, out) == 0 && MEM32(out) == di);
    CHECK(CALL(method(di, 2), di) == 1);

    /* EnumDevices: keyboard and mouse, then the pads attached to this machine
     * (none on most), at most 4. */
    int real = 0;
    for (int j = 0; j < SDL_NumJoysticks(); j++)
        real += SDL_IsGameController(j);
    real = real < 4 ? real : 4;
    enum_devices(di, 0 /* DI8DEVCLASS_ALL */, 1 /* DIEDFL_ATTACHEDONLY */, 0);
    CHECK(g_enum.n == 2 + real && g_enum.ref == 0x1234u);
    CHECK(g_enum.d1[0] == 0x6F1D2B61u && g_enum.type[0] == 0x413 && g_enum.size[0] == 580);
    CHECK(g_enum.d1[1] == 0x6F1D2B60u && g_enum.type[1] == 0x212 && g_enum.size[1] == 580);
    CHECK(strcmp(g_enum.name[0], "Keyboard") == 0 && strcmp(g_enum.name[1], "Mouse") == 0);
    enum_devices(di, 0, 0, 1);
    CHECK(g_enum.n == 1); /* DIENUM_STOP after the first */
    enum_devices(di, 2 /* DI8DEVCLASS_POINTER */, 0, 0);
    CHECK(g_enum.n == 1 && g_enum.d1[0] == 0x6F1D2B60u);
    enum_devices(di, 0x13 /* DI8DEVTYPE_KEYBOARD */, 0, 0);
    CHECK(g_enum.n == 1 && g_enum.d1[0] == 0x6F1D2B61u);
    enum_devices(di, 4 /* DI8DEVCLASS_GAMECTRL */, 1, 0);
    CHECK(g_enum.n == real);
    enum_devices(di, 1 /* DI8DEVCLASS_DEVICE */, 1, 0);
    CHECK(g_enum.n == 0);
    if (real < 4) {
        /* A virtual pad is one more game controller, an Xbox 360 pad. */
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        int index = SDL_JoystickAttachVirtualEx(&desc);
        CHECK(index >= 0 && SDL_IsGameController(index));
        enum_devices(di, 4, 1, 0);
        CHECK(g_enum.n == real + 1);
        CHECK(g_enum.product[real] == 0x028E045Eu && g_enum.size[real] == 580);
        CHECK(g_enum.type[real] == 0x10215u); /* GAMEPAD, STANDARD, HID */
        CHECK(g_enum.d1[real] == (uint32_t)real);
        CHECK(strcmp(g_enum.name[real], "Controller (XBO") == 0); /* 15 chars kept */
        enum_devices(di, 0x15 /* DI8DEVTYPE_GAMEPAD */, 1, 0);
        CHECK(g_enum.n == real + 1);
        enum_devices(di, 0, 1, 0);
        CHECK(g_enum.n == 3 + real && g_enum.type[0] == 0x413 && g_enum.type[1] == 0x212);
        enum_devices(di, 4, 0x100 /* DIEDFL_FORCEFEEDBACK */, 0);
        CHECK(g_enum.n == 0);
        enum_devices(di, 3 /* DI8DEVCLASS_KEYBOARD */, 1, 0);
        CHECK(g_enum.n == 1 && g_enum.d1[0] == 0x6F1D2B61u);
        SDL_JoystickDetachVirtual(index);
        enum_devices(di, 4, 1, 0);
        CHECK(g_enum.n == real);
    }

    CHECK(CALL(method(di, 5), di, g_guid_kbd) == DI_OK);
    CHECK(CALL(method(di, 5), di, g_guid_mouse) == DI_OK);
    CHECK(CALL(method(di, 5), di, g_guid_bad) == DIERR_DEVICENOTREG);
    MEM32(out) = 0xFFFFFFFFu;
    CHECK(CALL(method(di, 3), di, g_guid_bad, out, 0) == DIERR_DEVICENOTREG);
    CHECK(MEM32(out) == 0);

    /* Keyboard. */
    CHECK(CALL(method(di, 3), di, g_guid_kbd, out, 0) == DI_OK);
    uint32_t kb = MEM32(out);
    uint32_t fmt = guest_alloc(24), prop = guest_alloc(20), state = guest_alloc(256);
    uint32_t data = guest_alloc(20 * 16), inout = guest_alloc(4), caps = guest_alloc(44);
    CHECK(CALL(method(kb, 9), kb, 256, state) == DIERR_NOTACQUIRED);
    CHECK(CALL(method(kb, 7), kb) == E_INVALIDARG); /* no data format yet */
    MEM32(fmt + 12) = 16;
    CHECK(CALL(method(kb, 11), kb, fmt) == E_INVALIDARG); /* mouse format on a keyboard */
    MEM32(fmt + 12) = 256;
    CHECK(CALL(method(kb, 11), kb, fmt) == DI_OK);
    CHECK(CALL(method(kb, 13), kb, 0x10000, 0x6) == DI_OK); /* NONEXCLUSIVE|FOREGROUND */
    MEM32(prop) = 20, MEM32(prop + 4) = 16, MEM32(prop + 16) = 4;
    CHECK(CALL(method(kb, 6), kb, 1 /* DIPROP_BUFFERSIZE */, prop) == DI_OK);
    MEM32(prop + 16) = 0;
    CHECK(CALL(method(kb, 5), kb, 1, prop) == DI_OK && MEM32(prop + 16) == 4);
    MEM32(caps) = 44;
    CHECK(CALL(method(kb, 3), kb, caps) == DI_OK);
    CHECK(MEM32(caps + 4) == 1 && MEM32(caps + 8) == 0x413);
    CHECK(CALL(method(kb, 7), kb) == DI_OK);
    CHECK(CALL(method(kb, 7), kb) == S_FALSE); /* already acquired */
    CHECK(CALL(method(kb, 25), kb) == S_FALSE); /* Poll: DI_NOEFFECT */

    push_key(SDL_SCANCODE_A, 1);
    SDL_PumpEvents();
    CHECK(CALL(method(kb, 9), kb, 256, state) == DI_OK);
    int state_live = MEM8(state + DIK_A) == 0x80;
    printf("keyboard state: SDL_GetKeyboardState %s injected key-downs under the dummy "
           "driver\n", state_live ? "reflects" : "does not reflect");
    CHECK(CALL(method(kb, 9), kb, 255, state) == E_INVALIDARG);

    /* The scancode -> DIK table, through the buffered path. */
    push_key(SDL_SCANCODE_RCTRL, 1);
    push_key(SDL_SCANCODE_UP, 1);
    push_key(SDL_SCANCODE_A, 0);
    MEM32(inout) = 16;
    CHECK(CALL(method(kb, 10), kb, 20, data, inout, 0) == DI_OK);
    CHECK(MEM32(inout) == 4);
    CHECK(MEM32(data) == DIK_A && MEM32(data + 4) == 0x80);
    CHECK(MEM32(data + 20) == 0x9D && MEM32(data + 40) == 0xC8); /* DIK_RCONTROL, DIK_UP */
    CHECK(MEM32(data + 60) == DIK_A && MEM32(data + 64) == 0);
    CHECK(MEM32(data + 12) + 3 == MEM32(data + 72)); /* dwSequence rises */
    for (int i = 0; i < 6; i++)
        push_key(SDL_SCANCODE_SPACE, i % 2 == 0);
    MEM32(inout) = 16;
    CHECK(CALL(method(kb, 10), kb, 16, data, inout, 0) == S_FALSE); /* DI_BUFFEROVERFLOW */
    CHECK(MEM32(inout) == 4 && MEM32(data) == 0x39 && MEM32(data + 16) == 0x39);
    MEM32(inout) = 16;
    CHECK(CALL(method(kb, 10), kb, 20, data, inout, 0) == DI_OK && MEM32(inout) == 0);

    CHECK(CALL(method(kb, 8), kb) == DI_OK);
    CHECK(CALL(method(kb, 8), kb) == S_FALSE);
    CHECK(CALL(method(kb, 9), kb, 256, state) == DIERR_NOTACQUIRED);
    CHECK(CALL(method(kb, 10), kb, 20, data, inout, 0) == DIERR_NOTACQUIRED);

    /* Mouse. */
    CHECK(CALL(method(di, 3), di, g_guid_mouse, out, 0) == DI_OK);
    uint32_t ms = MEM32(out);
    MEM32(fmt + 12) = 256;
    CHECK(CALL(method(ms, 11), ms, fmt) == E_INVALIDARG);
    MEM32(fmt + 12) = 20;
    CHECK(CALL(method(ms, 11), ms, fmt) == DI_OK);
    MEM32(prop + 16) = 8;
    CHECK(CALL(method(ms, 6), ms, 1, prop) == DI_OK);
    MEM32(caps) = 44;
    CHECK(CALL(method(ms, 3), ms, caps) == DI_OK);
    CHECK(MEM32(caps + 8) == 0x212 && MEM32(caps + 12) == 3 && MEM32(caps + 16) == 8);
    CHECK(CALL(method(ms, 7), ms) == DI_OK);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = 2;
    e.wheel.preciseY = 2.0f; /* as a backend sets both */
    CHECK(SDL_PushEvent(&e) == 1);
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_RIGHT;
    e.button.state = SDL_PRESSED;
    CHECK(SDL_PushEvent(&e) == 1);
    CHECK(CALL(method(ms, 9), ms, 20, state) == DI_OK);
    CHECK((int32_t)MEM32(state + 8) == 240); /* lZ: 2 notches */
    CHECK(CALL(method(ms, 9), ms, 20, state) == DI_OK && MEM32(state + 8) == 0);
    MEM32(inout) = 8;
    CHECK(CALL(method(ms, 10), ms, 20, data, inout, 1 /* DIGDD_PEEK */) == DI_OK);
    CHECK(MEM32(inout) == 2);
    CHECK(MEM32(data) == 8 && MEM32(data + 4) == 240);           /* DIMOFS_Z */
    CHECK(MEM32(data + 20) == 13 && MEM32(data + 24) == 0x80);   /* DIMOFS_BUTTON1 */
    MEM32(inout) = 8;
    CHECK(CALL(method(ms, 10), ms, 20, data, inout, 0) == DI_OK && MEM32(inout) == 2);

    /* DIPROP_AXISMODE ABS, as the game's mouse setup sets it: lX,
     * lY and lZ become positions, and the game takes the deltas. */
    MEM32(prop + 16) = 0; /* DIPROPAXISMODE_ABS */
    CHECK(CALL(method(ms, 6), ms, 2, prop) == DIERR_ACQUIRED);
    CHECK(CALL(method(ms, 8), ms) == DI_OK);
    capture_stderr(1);
    CHECK(CALL(method(ms, 6), ms, 2, prop) == DI_OK);
    CHECK(strstr(capture_stderr(0), "UNSUPPORTED") == NULL);
    MEM32(prop + 16) = 2;
    CHECK(CALL(method(ms, 6), ms, 2, prop) == E_INVALIDARG);
    CHECK(CALL(method(ms, 7), ms) == DI_OK);
    for (int i = 0; i < 2; i++) {
        SDL_zero(e);
        e.type = SDL_MOUSEWHEEL;
        e.wheel.y = 1;
        e.wheel.preciseY = 1.0f;
        CHECK(SDL_PushEvent(&e) == 1);
        CHECK(CALL(method(ms, 9), ms, 20, state) == DI_OK);
        CHECK((int32_t)MEM32(state + 8) == 120 * (i + 1));
    }
    CHECK(CALL(method(ms, 9), ms, 20, state) == DI_OK && MEM32(state + 8) == 240);
    CHECK(CALL(method(ms, 8), ms) == DI_OK);
    MEM32(prop + 16) = 1; /* DIPROPAXISMODE_REL */
    CHECK(CALL(method(ms, 6), ms, 2, prop) == DI_OK);
    CHECK(CALL(method(ms, 7), ms) == DI_OK);
    CHECK(CALL(method(ms, 9), ms, 20, state) == DI_OK && MEM32(state + 8) == 0);

    /* Every unimplemented method: E_NOTIMPL, UNSUPPORTED once, stack popped. */
    static const unsigned di_m[][2] = {{6, 2}, {7, 2}, {8, 3}, {9, 5}, {10, 4}};
    static const char *const di_n[] = {"RunControlPanel", "Initialize", "FindDevice",
                                       "EnumDevicesBySemantics", "ConfigureDevices"};
    check_notimpl(di, "IDirectInput8A", di_m, 5, di_n);
    static const unsigned dev_m[][2] = {
        {4, 3},  {12, 1}, {14, 3}, {15, 1}, {16, 2}, {17, 3}, {18, 4},
        {19, 3}, {20, 2}, {21, 1}, {22, 1}, {23, 3}, {24, 1}, {26, 4},
        {27, 4}, {28, 4}, {29, 3}, {30, 3}, {31, 1},
    };
    static const char *const dev_n[] = {
        "EnumObjects", "SetEventNotification", "GetObjectInfo", "GetDeviceInfo",
        "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects", "GetEffectInfo",
        "GetForceFeedbackState", "SendForceFeedbackCommand", "EnumCreatedEffectObjects",
        "Escape", "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile",
        "BuildActionMap", "SetActionMap", "GetImageInfo",
    };
    check_notimpl(kb, "IDirectInputDevice8A", dev_m, 19, dev_n);

    /* Release frees the guest objects at 0. */
    CHECK(CALL(method(ms, 1), ms) == 2);
    CHECK(CALL(method(ms, 2), ms) == 1);
    CHECK(CALL(method(ms, 2), ms) == 0);
    CHECK(CALL(method(kb, 2), kb) == 0);
    CHECK(CALL(method(di, 2), di) == 0);
    CHECK(g_freed == 3);
}

/* ---- Scripted input ---------------------------------------------------- */

static uint64_t g_ms;
static uint64_t fake_now(void) { return g_ms; }

static void test_input_script(void)
{
    char err[128];
    static const struct {
        const char *text;
        const char *err;
    } BAD[] = {
        {"0 tap RETURN\nfoo\n", "line 2: "},
        {"# c\n\n10 press A\n", "line 3: action"},
        {"5 down NOPE", "line 1: unknown name"},
        {"5 down PAD_LX=40000", "line 1: stick"},
        {"x down A", "line 1: bad ms"},
        {"1 down A B", "line 1: want"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        err[0] = 0;
        CHECK(input_script_parse(BAD[i].text, err, sizeof err) == -1);
        if (strncmp(err, BAD[i].err, strlen(BAD[i].err)) != 0)
            fprintf(stderr, "FAIL parse error %zu: got \"%s\", want \"%s...\"\n", i, err,
                    BAD[i].err), g_fail = 1;
    }

    input_script_set_clock(fake_now);
    g_ms = 1000; /* time zero: the next DirectInput8Create */
    CHECK(input_script_parse("# menu skip\n"
                             "0 tap RETURN\n"
                             "50 down LSHIFT  # held\n"
                             "200 up LSHIFT\n"
                             "300 tap PAD_A\n"
                             "300 down PAD_LX=-20000\n"
                             "400 up PAD_LX=-20000\n",
                             err, sizeof err) == 0);
    int real = 0;
    for (int j = 0; j < SDL_NumJoysticks(); j++)
        real += SDL_IsGameController(j);

    capture_stderr(1);
    static const uint8_t di8a_d4[8] = {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
    uint32_t iid_a = guid_at(0xBF798030u, 0x483A, 0x4DA2, di8a_d4);
    uint32_t out = guest_alloc(4), fmt = guest_alloc(24), state = guest_alloc(256);
    CHECK(CALL(export_fn("DirectInput8Create", 0), 0x00400000u, 0x0800, iid_a, out, 0) == DI_OK);
    uint32_t di = MEM32(out);
    CHECK(CALL(method(di, 3), di, g_guid_kbd, out, 0) == DI_OK);
    uint32_t kb = MEM32(out);
    MEM32(fmt + 12) = 256;
    CHECK(CALL(method(kb, 11), kb, fmt) == DI_OK);
    CHECK(CALL(method(kb, 7), kb) == DI_OK);
    static const struct {
        uint64_t t;
        uint8_t ret, shift;
    } KB[] = {{0, 0x80, 0}, {99, 0x80, 0x80}, {100, 0, 0x80}, {199, 0, 0x80}, {200, 0, 0}};
    for (size_t i = 0; i < sizeof KB / sizeof KB[0]; i++) {
        g_ms = 1000 + KB[i].t;
        CHECK(CALL(method(kb, 9), kb, 256, state) == DI_OK);
        CHECK(MEM8(state + 0x1C) == KB[i].ret && MEM8(state + 0x2A) == KB[i].shift);
    }

    /* Pad lines: user 0 is connected even with no SDL pad. */
    recomp_func_t get = export_fn(NULL, 2), set = export_fn(NULL, 3), caps = export_fn(NULL, 4);
    uint32_t st = guest_alloc(16), cp = guest_alloc(20), vib = guest_alloc(4);
    CHECK(CALL(caps, 0, 1, cp) == 0 && MEM8(cp) == 1 && MEM16(cp + 4) == 0xF3FF);
    CHECK(CALL(set, 0, vib) == 0);
    CHECK(CALL(get, 1, st) == (real > 1 ? 0 : XI_NOT_CONNECTED)); /* user 0 only */
    CHECK(CALL(get, 0, st) == 0);
    uint32_t packet = MEM32(st);
    if (!real)
        CHECK(MEM16(st + 4) == 0 && MEM16(st + 8) == 0);
    g_ms = 1300;
    CHECK(CALL(get, 0, st) == 0 && MEM32(st) == packet + 1);
    CHECK((MEM16(st + 4) & 0x1000) && (int16_t)MEM16(st + 8) == -20000);
    g_ms = 1350;
    CHECK(CALL(get, 0, st) == 0 && MEM32(st) == packet + 1); /* unchanged */
    g_ms = 1400;
    CHECK(CALL(get, 0, st) == 0 && MEM32(st) == packet + 2);
    if (!real)
        CHECK(MEM16(st + 4) == 0 && MEM16(st + 8) == 0);
    char *log = capture_stderr(0);
    if (g_fail)
        fputs(log, stderr); /* CHECK lines written while captured */
    CHECK(strstr(log, "INPUT t=0 down RETURN\n") != NULL);
    CHECK(strstr(log, "INPUT t=99 down LSHIFT\n") != NULL);
    CHECK(strstr(log, "INPUT t=100 up RETURN\n") != NULL);
    CHECK(strstr(log, "INPUT t=200 up LSHIFT\n") != NULL);
    CHECK(strstr(log, "INPUT t=300 down PAD_A\n") != NULL);
    CHECK(strstr(log, "INPUT t=300 down PAD_LX=-20000\n") != NULL);
    CHECK(strstr(log, "INPUT t=400 up PAD_A\n") != NULL);
    char *first = strstr(log, "down RETURN");
    CHECK(first && !strstr(first + 1, "down RETURN")); /* logged once */

    /* No script: user 0 is back to the SDL pads only. */
    CHECK(input_script_parse("", err, sizeof err) == 0);
    input_script_set_clock(NULL);
    if (!real)
        CHECK(CALL(caps, 0, 1, cp) == XI_NOT_CONNECTED && CALL(get, 0, st) == XI_NOT_CONNECTED);
    CHECK(CALL(method(kb, 2), kb) == 0);
    CHECK(CALL(method(di, 2), di) == 0);
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    test_xinput();
    test_dinput();
    test_input_script();
    SDL_Quit();
    puts(g_fail ? "test_win32_input: FAIL" : "test_win32_input: OK");
    return g_fail;
}
