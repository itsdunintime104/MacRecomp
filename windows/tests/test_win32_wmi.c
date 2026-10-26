/* WMI slice test: the IsXInputDevice sequence a game runs on each pad
 * EnumDevices lists, through the ole32/oleaut32 thunks and guest vtables.
 * Links win32_wmi.c, win32_com.c, com_guest.c, frame_trace.c, win32_input.c
 * and input_script.c with the engine's guest_mem.c, guest_heap.c,
 * dlmalloc.c, guest_call.c, host_funcs.c and recomp_glue.c. Run with
 * SDL_VIDEODRIVER=dummy. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "com_guest.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "recomp_types.h"
#include "win32_thunk.h"

/* Generated dispatch; this test has no lifted code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* win32_ui.c double, for win32_input.c. */
int w32_ui_key_hidden(int scancode)
{
    (void)scancode;
    return 0;
}

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define DATA_AREA 0x00FF8000u
#define RET_ADDR 0xDEAD0001u

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define WBEM_E_NOT_FOUND 0x80041002u
#define WBEM_E_INVALID_CLASS 0x80041010u

static uint32_t g_data_next = DATA_AREA;
static uint32_t guest_alloc(uint32_t n)
{
    uint32_t a = g_data_next;
    memset(guest_ptr(a), 0, n);
    g_data_next += (n + 15u) & ~15u;
    return a;
}

static uint32_t guest_guid(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8])
{
    uint32_t a = guest_alloc(16);
    MEM32(a) = d1;
    MEM16(a + 4) = d2;
    MEM16(a + 6) = d3;
    memcpy(guest_ptr(a + 8), d4, 8);
    return a;
}

/* A guest UTF-16 copy of an ASCII string. */
static uint32_t guest_wstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s), a = guest_alloc(2u * n + 2u);
    for (uint32_t i = 0; i <= n; i++)
        MEM16(a + 2u * i) = (uint8_t)s[i];
    return a;
}

/* The guest UTF-16 string at w as ASCII. */
static const char *host_str(uint32_t w)
{
    static char buf[128];
    size_t i = 0;
    for (; i < sizeof buf - 1 && MEM16(w + 2u * i); i++)
        buf[i] = (char)MEM16(w + 2u * i);
    buf[i] = 0;
    return buf;
}

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

static recomp_func_t com_export(const char *name, uint16_t ordinal)
{
    for (const win32_export_t *e = win32_exports_com; e->dll; e++)
        if (ordinal ? e->ordinal == ordinal && !e->name : e->name && strcmp(e->name, name) == 0)
            return e->fn;
    CHECK(!"export missing");
    return NULL;
}

/* COM method idx of obj, through its guest vtable. */
static recomp_func_t method(uint32_t obj, unsigned idx)
{
    return guest_resolve(MEM32(MEM32(obj) + 4u * idx));
}

/* Microsoft's IsXInputDevice, minus the guidProduct match: returns the
 * number of Win32_PNPEntity objects whose DeviceID has "IG_" and
 * MAKELONG(VID, PID) == 0x028E045E, after checking each step. */
static int xinput_devices(void)
{
    static const uint8_t clsid_d4[8] = {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24};
    static const uint8_t iid_d4[8] = {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24};
    uint32_t clsid = guest_guid(0x4590F811u, 0x1D3A, 0x11D0, clsid_d4);
    uint32_t iid = guest_guid(0xDC12A687u, 0x737F, 0x11CF, iid_d4);
    uint32_t out = guest_alloc(4), arr = guest_alloc(4 * 20), ret = guest_alloc(4);
    uint32_t var = guest_alloc(16);
    recomp_func_t alloc = com_export(NULL, 2), sfree = com_export(NULL, 6);

    CALL(com_export("CoInitialize", 0), 0);
    CHECK(CALL(com_export("CoCreateInstance", 0), clsid, 0, 1, iid, out) == S_OK);
    uint32_t loc = MEM32(out);
    CHECK(loc != 0);
    uint32_t ns = CALL(alloc, guest_wstr("\\\\.\\root\\cimv2"));
    uint32_t cls = CALL(alloc, guest_wstr("Win32_PNPEntity"));
    uint32_t prop = CALL(alloc, guest_wstr("DeviceID"));
    CHECK(ns && cls && prop);
    MEM32(out) = 0;
    CHECK(CALL(method(loc, 3), loc, ns, 0, 0, 0, 0, 0, 0, out) == S_OK);
    uint32_t svc = MEM32(out);
    CHECK(svc != 0);
    CHECK(CALL(com_export("CoSetProxyBlanket", 0), svc, 10, 0, 0, 3, 3, 0, 0) == S_OK);
    MEM32(out) = 0;
    CHECK(CALL(method(svc, 18), svc, cls, 0, 0, out) == S_OK);
    uint32_t en = MEM32(out);
    CHECK(en != 0);

    int found = 0;
    for (;;) {
        MEM32(ret) = 0xFFFFFFFFu;
        uint32_t hr = CALL(method(en, 4), en, 10000, 20, arr, ret);
        uint32_t n = MEM32(ret);
        CHECK(n <= 20);
        CHECK(hr == (n == 20 ? S_OK : S_FALSE));
        if (n == 0 || n > 20)
            break;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t obj = MEM32(arr + 4 * i);
            memset(guest_ptr(var), 0xEE, 16);
            CHECK(CALL(method(obj, 4), obj, prop, 0, var, 0, 0) == S_OK);
            CHECK(MEM16(var) == 8); /* VT_BSTR */
            uint32_t bstr = MEM32(var + 8);
            CHECK(bstr != 0);
            if (bstr == 0)
                continue;
            const char *id = host_str(bstr);
            CHECK(MEM32(bstr - 4) == 2 * strlen(id)); /* the BSTR length prefix */
            unsigned vid = 0, pid = 0;
            const char *v = strstr(id, "VID_"), *p = strstr(id, "PID_");
            if (strstr(id, "IG_") && v && p && sscanf(v, "VID_%4X", &vid) == 1 &&
                sscanf(p, "PID_%4X", &pid) == 1 && (vid | pid << 16) == 0x028E045Eu)
                found++;
            else
                fprintf(stderr, "FAIL: DeviceID \"%s\"\n", id), g_fail = 1;
            CALL(sfree, bstr);
            CHECK(CALL(method(obj, 2), obj) == 0);
            CHECK(com_guest_iface(obj) == NULL); /* freed */
        }
    }
    CHECK(CALL(method(en, 2), en) == 0);
    CHECK(CALL(method(svc, 2), svc) == 0);
    CHECK(CALL(method(loc, 2), loc) == 0);
    CHECK(com_guest_iface(en) == NULL && com_guest_iface(svc) == NULL &&
          com_guest_iface(loc) == NULL);
    CALL(sfree, ns);
    CALL(sfree, cls);
    CALL(sfree, prop);
    CALL(com_export("CoUninitialize", 0));
    return found;
}

/* Errors and the methods the sequence does not use. */
static void test_edges(void)
{
    static const uint8_t clsid_d4[8] = {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24};
    static const uint8_t iid_d4[8] = {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24};
    static const uint8_t svc_d4[8] = {0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x32, 0x40, 0xC7};
    uint32_t clsid = guest_guid(0x4590F811u, 0x1D3A, 0x11D0, clsid_d4);
    uint32_t iid = guest_guid(0xDC12A687u, 0x737F, 0x11CF, iid_d4);
    uint32_t iid_svc = guest_guid(0x9556DC99u, 0x828C, 0x11CF, svc_d4);
    uint32_t out = guest_alloc(4), arr = guest_alloc(4 * 4), ret = guest_alloc(4);
    uint32_t var = guest_alloc(16);

    CHECK(CALL(com_export("CoCreateInstance", 0), clsid, 0, 1, iid_svc, out) == 0x80004002u);
    CHECK(MEM32(out) == 0);
    CHECK(CALL(com_export("CoCreateInstance", 0), clsid, 0, 1, iid, out) == S_OK);
    uint32_t loc = MEM32(out);
    CHECK(CALL(method(loc, 3), loc, 0, 0, 0, 0, 0, 0, 0, out) == S_OK);
    uint32_t svc = MEM32(out);
    CHECK(CALL(method(svc, 0), svc, iid_svc, out) == S_OK && MEM32(out) == svc);
    CHECK(CALL(method(svc, 0), svc, iid, out) == 0x80004002u && MEM32(out) == 0);
    CHECK(CALL(method(svc, 1), svc) == 3);
    CHECK(CALL(method(svc, 2), svc) == 2);
    CHECK(CALL(method(svc, 2), svc) == 1);
    CHECK(CALL(method(svc, 20), svc, 0, 0, 0, 0, out) == E_NOTIMPL); /* ExecQuery */
    MEM32(out) = 0xFFFFFFFFu;
    CHECK(CALL(method(svc, 18), svc, guest_wstr("Win32_Process"), 0, 0, out) ==
          WBEM_E_INVALID_CLASS);
    CHECK(MEM32(out) == 0);
    /* Class and property names ignore case. */
    CHECK(CALL(method(svc, 18), svc, guest_wstr("win32_pnpentity"), 0, 0, out) == S_OK);
    uint32_t en = MEM32(out);
    CHECK(CALL(method(en, 3), en) == E_NOTIMPL); /* Reset */
    MEM32(ret) = 0;
    uint32_t hr = CALL(method(en, 4), en, 0, 1, arr, ret);
    if (MEM32(ret) == 1) {
        CHECK(hr == S_OK);
        uint32_t obj = MEM32(arr);
        CHECK(CALL(method(obj, 4), obj, guest_wstr("Name"), 0, var, 0, 0) == WBEM_E_NOT_FOUND);
        CHECK(CALL(method(obj, 4), obj, guest_wstr("deviceid"), 0, var, 0, 0) == S_OK);
        CHECK(CALL(method(obj, 9), obj, 0, 0, 0, 0, 0) == E_NOTIMPL); /* Next */
        CHECK(CALL(method(obj, 2), obj) == 0);
    } else {
        CHECK(hr == S_FALSE && MEM32(ret) == 0);
    }
    CHECK(CALL(method(en, 2), en) == 0);
    CHECK(CALL(method(svc, 2), svc) == 0);
    CHECK(CALL(method(loc, 2), loc) == 0);
}

int main(void)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 2;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    /* Pads attached to this machine (none on most), at most 4. */
    int real = 0;
    for (int j = 0; j < SDL_NumJoysticks(); j++)
        real += SDL_IsGameController(j);
    real = real < 4 ? real : 4;
    CHECK(xinput_devices() == real);
    if (real < 4) {
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        int index = SDL_JoystickAttachVirtualEx(&desc);
        CHECK(index >= 0 && SDL_IsGameController(index));
        CHECK(xinput_devices() == real + 1);
        test_edges();
        SDL_JoystickDetachVirtual(index);
        CHECK(xinput_devices() == real);
    }
    SDL_Quit();
    puts(g_fail ? "test_win32_wmi: FAIL" : "test_win32_wmi: OK");
    return g_fail;
}
