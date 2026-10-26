/* Win32 audio slice test: the two X3DAudio1_6 thunks on FAudio. Checks the
 * cdecl stack pop, register preservation, the guest-owned handle, and that
 * X3DAudioCalculate through guest structs gives the same results as
 * F3DAudioCalculate on host structs (stereo output, mono emitter 10 units to
 * the right, moving toward the listener). Links win32_audio.c, guest_mem.c
 * and recomp_glue.c only; no win32_core.c or com_guest.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <F3DAudio.h>

#include "recomp_types.h"
#include "guest_mem.h"
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

/* ---- Guest stack ------------------------------------------------------- */

#define GUEST_LO 0x00A00000u
#define DATA_AREA GUEST_LO
#define STACK_TOP 0x00FF0000u
#define RET_ADDR 0xDEAD0001u

static recomp_func_t audio(const char *name)
{
    for (const win32_export_t *e = win32_exports_audio; e->dll != NULL; e++)
        if (strcmp(e->dll, "X3DAudio1_6.dll") == 0 && strcmp(e->name, name) == 0)
            return e->fn;
    CHECK(!"export missing");
    exit(1);
}

/* Push args right to left and the return address, call fn, check that it
 * popped only the return address (cdecl) and kept ebx/esi/edi/ebp. */
static void call_cdecl_thunk(recomp_func_t fn, unsigned nargs, const uint32_t *args)
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
    fn();
    CHECK(g_esp == start + 4u);
    CHECK(MEM32(start) == RET_ADDR);
    for (unsigned i = 0; i < nargs; i++)
        CHECK(MEM32(start + 4u + 4u * i) == args[i]); /* caller's slots untouched */
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBEBEBEBEu);
}

/* ---- Guest memory writers ---------------------------------------------- */

static uint32_t g_next = DATA_AREA;
static uint32_t galloc(uint32_t n)
{
    uint32_t a = g_next;
    memset(guest_ptr(a), 0, n);
    g_next += (n + 15u) & ~15u;
    return a;
}
static void put32(uint32_t a, uint32_t v)
{
    MEM32(a) = v;
}
static void putf(uint32_t a, float f)
{
    memcpy(guest_ptr(a), &f, 4);
}
static void putvec(uint32_t a, F3DAUDIO_VECTOR v)
{
    putf(a, v.x), putf(a + 4, v.y), putf(a + 8, v.z);
}
static float getf(uint32_t a)
{
    float f;
    memcpy(&f, guest_ptr(a), 4);
    return f;
}
static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

#define SPEED_OF_SOUND 343.5f /* the game's constant */
#define FLAGS                                                                          \
    (F3DAUDIO_CALCULATE_MATRIX | F3DAUDIO_CALCULATE_DELAY | F3DAUDIO_CALCULATE_LPF_DIRECT | \
     F3DAUDIO_CALCULATE_DOPPLER | F3DAUDIO_CALCULATE_EMITTER_ANGLE)

int main(void)
{
    if (guest_mem_init() != 0) {
        fprintf(stderr, "guest_mem_init failed\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(GUEST_LO), STACK_TOP - GUEST_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }

    /* ---- X3DAudioInitialize: handle in guest memory, same bytes as host -- */
    uint32_t ghandle = galloc(F3DAUDIO_HANDLE_BYTESIZE);
    memset(guest_ptr(ghandle), 0xA5, F3DAUDIO_HANDLE_BYTESIZE);
    uint32_t guard = galloc(4);
    put32(guard, 0x600DF00Du);
    uint32_t init_args[3] = {SPEAKER_STEREO, fbits(SPEED_OF_SOUND), ghandle};
    call_cdecl_thunk(audio("X3DAudioInitialize"), 3, init_args);
    CHECK(g_eax == 0);
    CHECK(MEM32(guard) == 0x600DF00Du);

    F3DAUDIO_HANDLE hhandle;
    F3DAudioInitialize(SPEAKER_STEREO, SPEED_OF_SOUND, hhandle);
    CHECK(memcmp(guest_ptr(ghandle), hhandle, F3DAUDIO_HANDLE_BYTESIZE) == 0);
    CHECK(MEM32(ghandle) == SPEAKER_STEREO); /* no host pointer inside */

    /* ---- Host reference setup ------------------------------------------ */
    static F3DAUDIO_DISTANCE_CURVE_POINT vol_points[2] = {{0.0f, 1.0f}, {40.0f, 0.0f}};
    _Alignas(8) static F3DAUDIO_DISTANCE_CURVE vol_curve = {vol_points, 2};
    static F3DAUDIO_CONE cone = {1.0f, 3.0f, 1.0f, 0.5f, 1.0f, 0.75f, 1.0f, 0.5f};
    static float azimuths[1] = {0.0f};

    F3DAUDIO_LISTENER hl;
    memset(&hl, 0, sizeof hl);
    hl.OrientFront = (F3DAUDIO_VECTOR){0, 0, 1};
    hl.OrientTop = (F3DAUDIO_VECTOR){0, 1, 0};
    hl.Velocity = (F3DAUDIO_VECTOR){0, 0, 0};

    F3DAUDIO_EMITTER he;
    memset(&he, 0, sizeof he);
    he.pCone = &cone;
    he.OrientFront = (F3DAUDIO_VECTOR){-1, 0, 0}; /* facing the listener */
    he.OrientTop = (F3DAUDIO_VECTOR){0, 1, 0};
    he.Position = (F3DAUDIO_VECTOR){10, 0, 0};
    he.Velocity = (F3DAUDIO_VECTOR){-20, 0, 0}; /* approaching: Doppler > 1 */
    he.ChannelCount = 1;
    he.pChannelAzimuths = azimuths;
    he.pVolumeCurve = &vol_curve;
    he.CurveDistanceScaler = 1.0f;
    he.DopplerScaler = 1.0f;

    float hmatrix[2] = {-1, -1}, hdelay[2] = {-1, -1};
    F3DAUDIO_DSP_SETTINGS hd;
    memset(&hd, 0, sizeof hd);
    hd.pMatrixCoefficients = hmatrix;
    hd.pDelayTimes = hdelay;
    hd.SrcChannelCount = 1;
    hd.DstChannelCount = 2;
    hd.ReverbLevel = 7.0f; /* not computed for FLAGS: must come back unchanged */
    F3DAudioCalculate(hhandle, &hl, &he, FLAGS, &hd);

    /* ---- The same setup as guest structs (offsets: x3daudio.h, i686) ----- */
    uint32_t gpoints = galloc(16);
    putf(gpoints + 0, 0.0f), putf(gpoints + 4, 1.0f), putf(gpoints + 8, 40.0f), putf(gpoints + 12, 0.0f);
    uint32_t gcurve = galloc(8);
    put32(gcurve + 0, gpoints), put32(gcurve + 4, 2);
    uint32_t gcone = galloc(32);
    memcpy(guest_ptr(gcone), &cone, 32);
    uint32_t gaz = galloc(4);
    putf(gaz, 0.0f);

    uint32_t gl = galloc(52);
    putvec(gl + 0, hl.OrientFront), putvec(gl + 12, hl.OrientTop);
    putvec(gl + 24, hl.Position), putvec(gl + 36, hl.Velocity);
    put32(gl + 48, 0);

    uint32_t ge = galloc(100);
    put32(ge + 0, gcone);
    putvec(ge + 4, he.OrientFront), putvec(ge + 16, he.OrientTop);
    putvec(ge + 28, he.Position), putvec(ge + 40, he.Velocity);
    putf(ge + 52, 0.0f), putf(ge + 56, 0.0f);
    put32(ge + 60, 1), putf(ge + 64, 0.0f);
    put32(ge + 68, gaz), put32(ge + 72, gcurve);
    put32(ge + 76, 0), put32(ge + 80, 0), put32(ge + 84, 0), put32(ge + 88, 0);
    putf(ge + 92, 1.0f), putf(ge + 96, 1.0f);

    uint32_t gmatrix = galloc(8), gdelay = galloc(8);
    putf(gmatrix, -1), putf(gmatrix + 4, -1), putf(gdelay, -1), putf(gdelay + 4, -1);
    uint32_t gd = galloc(48);
    put32(gd + 0, gmatrix), put32(gd + 4, gdelay);
    put32(gd + 8, 1), put32(gd + 12, 2);
    putf(gd + 24, 7.0f);

    uint32_t calc_args[5] = {ghandle, gl, ge, FLAGS, gd};
    call_cdecl_thunk(audio("X3DAudioCalculate"), 5, calc_args);

    /* Pointers in the guest struct are untouched; outputs match the host. */
    CHECK(MEM32(gd + 0) == gmatrix && MEM32(gd + 4) == gdelay);
    CHECK(getf(gmatrix) == hmatrix[0] && getf(gmatrix + 4) == hmatrix[1]);
    CHECK(getf(gdelay) == hdelay[0] && getf(gdelay + 4) == hdelay[1]);
    CHECK(MEM32(gd + 8) == 1 && MEM32(gd + 12) == 2);
    CHECK(getf(gd + 16) == hd.LPFDirectCoefficient);
    CHECK(getf(gd + 20) == hd.LPFReverbCoefficient);
    CHECK(getf(gd + 24) == 7.0f && hd.ReverbLevel == 7.0f);
    CHECK(getf(gd + 28) == hd.DopplerFactor);
    CHECK(getf(gd + 32) == hd.EmitterToListenerAngle);
    CHECK(getf(gd + 36) == hd.EmitterToListenerDistance);
    CHECK(getf(gd + 40) == hd.EmitterVelocityComponent);
    CHECK(getf(gd + 44) == hd.ListenerVelocityComponent);

    /* The reference itself is the expected scene: panned right, closer than
     * 40 units on the volume curve, approaching. */
    CHECK(hmatrix[1] > hmatrix[0]);
    CHECK(hmatrix[1] > 0.0f && hmatrix[1] <= 1.0f);
    CHECK(fabsf(hd.EmitterToListenerDistance - 10.0f) < 1e-4f);
    CHECK(hd.DopplerFactor > 1.0f);
    CHECK(MEM32(guard) == 0x600DF00Du);

    printf("X3DAudioCalculate: matrix L=%.6f R=%.6f doppler=%.6f distance=%.3f "
           "delay L=%.6f R=%.6f lpf=%.6f\n",
           getf(gmatrix), getf(gmatrix + 4), getf(gd + 28), getf(gd + 36), getf(gdelay),
           getf(gdelay + 4), getf(gd + 16));
    if (g_fail) {
        printf("test_win32_audio: FAIL\n");
        return 1;
    }
    printf("test_win32_audio: PASS\n");
    return 0;
}
