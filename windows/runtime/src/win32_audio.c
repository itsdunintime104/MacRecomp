/* Win32 audio slice: the 2 X3DAudio1_6 imports on FAudio's F3DAudio
 * (notes/slice_owners.md, slice "audio"). Contract: runtime/include/win32_thunk.h
 * and docs/WIN32_THUNKS.md. The XAudio2 COM layer is planned in
 * docs/AUDIO_PLAN.md.
 *
 * Both imports are cdecl (the SDK's STDAPIVCALLTYPE), proven at the call
 * sites in HP: X3DAudioInitialize is followed by `add esp, 0xc`, and
 * X3DAudioCalculate leaves its 20 argument bytes on the stack
 * (the caller reads its DSP struct at [esp+0x6C] right after the call, then
 * at [esp+0x5C] after `add esp, 0x10`). So a thunk pops only the return
 * address.
 *
 * The X3DAUDIO structs hold pointers, so their 32-bit guest layout (offsets
 * below, from mingw-w64's x3daudio.h under clang -target i686-w64-windows-gnu)
 * differs from FAudio's 64-bit one. Each call copies the guest structs into
 * F3DAUDIO structs and copies the scalar outputs back. Arrays of plain floats
 * (channel azimuths, curve points, matrix coefficients, delay times) have the
 * same layout on both sides and are passed as guest_ptr() directly. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <F3DAudio.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "win32_thunk.h"

/* The guest owns the 20-byte X3DAUDIO_HANDLE and FAudio uses it in place:
 * F3DAudio stores only integers and floats in it, never a pointer. */
_Static_assert(sizeof(F3DAUDIO_HANDLE) == 20, "X3DAUDIO_HANDLE is 20 bytes");

/* Guest X3DAUDIO_DISTANCE_CURVE (8 bytes). */
#define G_CURVE_pPoints 0u
#define G_CURVE_PointCount 4u

/* Guest X3DAUDIO_LISTENER (52 bytes). */
#define G_LISTENER_OrientFront 0u
#define G_LISTENER_OrientTop 12u
#define G_LISTENER_Position 24u
#define G_LISTENER_Velocity 36u
#define G_LISTENER_pCone 48u

/* Guest X3DAUDIO_EMITTER (100 bytes). */
#define G_EMITTER_pCone 0u
#define G_EMITTER_OrientFront 4u
#define G_EMITTER_OrientTop 16u
#define G_EMITTER_Position 28u
#define G_EMITTER_Velocity 40u
#define G_EMITTER_InnerRadius 52u
#define G_EMITTER_InnerRadiusAngle 56u
#define G_EMITTER_ChannelCount 60u
#define G_EMITTER_ChannelRadius 64u
#define G_EMITTER_pChannelAzimuths 68u
#define G_EMITTER_pVolumeCurve 72u
#define G_EMITTER_pLFECurve 76u
#define G_EMITTER_pLPFDirectCurve 80u
#define G_EMITTER_pLPFReverbCurve 84u
#define G_EMITTER_pReverbCurve 88u
#define G_EMITTER_CurveDistanceScaler 92u
#define G_EMITTER_DopplerScaler 96u

/* Guest X3DAUDIO_DSP_SETTINGS (48 bytes): two pointers, then ten 4-byte
 * scalars in the same order as F3DAUDIO_DSP_SETTINGS. */
#define G_DSP_pMatrixCoefficients 0u
#define G_DSP_pDelayTimes 4u
#define G_DSP_SrcChannelCount 8u
#define G_DSP_SCALAR_BYTES 40u

/* X3DAUDIO_CONE, X3DAUDIO_VECTOR and X3DAUDIO_DISTANCE_CURVE_POINT are all
 * floats: one layout on both sides. */
_Static_assert(sizeof(F3DAUDIO_CONE) == 32, "guest X3DAUDIO_CONE is 32 bytes");
_Static_assert(sizeof(F3DAUDIO_VECTOR) == 12, "guest X3DAUDIO_VECTOR is 12 bytes");
_Static_assert(sizeof(F3DAUDIO_DISTANCE_CURVE_POINT) == 8, "guest curve point is 8 bytes");
_Static_assert(offsetof(F3DAUDIO_DSP_SETTINGS, ListenerVelocityComponent) -
                       offsetof(F3DAUDIO_DSP_SETTINGS, SrcChannelCount) + 4 ==
                   G_DSP_SCALAR_BYTES,
               "DSP scalars are contiguous");

static void *gptr_or_null(uint32_t a)
{
    return a != 0 ? guest_ptr(a) : NULL;
}

static float gfloat(uint32_t a)
{
    float f;
    memcpy(&f, guest_ptr(a), sizeof f);
    return f;
}

static void gvector(uint32_t a, F3DAUDIO_VECTOR *out)
{
    memcpy(out, guest_ptr(a), sizeof *out);
}

/* A guest curve pointer: NULL, or the host copy in *slot (its points stay in
 * guest memory). */
static F3DAUDIO_DISTANCE_CURVE *gcurve(uint32_t a, F3DAUDIO_DISTANCE_CURVE *slot)
{
    if (a == 0)
        return NULL;
    slot->pPoints = gptr_or_null(MEM32(a + G_CURVE_pPoints));
    slot->PointCount = MEM32(a + G_CURVE_PointCount);
    return slot;
}

/* HRESULT X3DAudioInitialize(UINT32 SpeakerChannelMask, float SpeedOfSound,
 *                            X3DAUDIO_HANDLE Instance)   cdecl
 * X3DAudio 1.6 declares it void; the game ignores eax (it clears eax next). */
static void imp_X3DAudioInitialize(void)
{
    uint32_t mask = W32_ARG(0);
    uint32_t speed_bits = W32_ARG(1);
    uint32_t instance = W32_ARG(2);
    float speed;
    memcpy(&speed, &speed_bits, sizeof speed);
    F3DAudioInitialize(mask, speed, guest_ptr(instance));
    g_eax = 0;
    g_esp += 4u;
}

/* void X3DAudioCalculate(const X3DAUDIO_HANDLE Instance,
 *                        const X3DAUDIO_LISTENER *pListener,
 *                        const X3DAUDIO_EMITTER *pEmitter, UINT32 Flags,
 *                        X3DAUDIO_DSP_SETTINGS *pDSPSettings)   cdecl */
static void imp_X3DAudioCalculate(void)
{
    uint32_t instance = W32_ARG(0), gl = W32_ARG(1), ge = W32_ARG(2);
    uint32_t flags = W32_ARG(3), gd = W32_ARG(4);

    F3DAUDIO_CONE listener_cone, emitter_cone;
    F3DAUDIO_LISTENER listener;
    gvector(gl + G_LISTENER_OrientFront, &listener.OrientFront);
    gvector(gl + G_LISTENER_OrientTop, &listener.OrientTop);
    gvector(gl + G_LISTENER_Position, &listener.Position);
    gvector(gl + G_LISTENER_Velocity, &listener.Velocity);
    listener.pCone = NULL;
    if (MEM32(gl + G_LISTENER_pCone) != 0) {
        memcpy(&listener_cone, guest_ptr(MEM32(gl + G_LISTENER_pCone)), sizeof listener_cone);
        listener.pCone = &listener_cone;
    }

    F3DAUDIO_DISTANCE_CURVE curves[5];
    F3DAUDIO_EMITTER emitter;
    emitter.pCone = NULL;
    if (MEM32(ge + G_EMITTER_pCone) != 0) {
        memcpy(&emitter_cone, guest_ptr(MEM32(ge + G_EMITTER_pCone)), sizeof emitter_cone);
        emitter.pCone = &emitter_cone;
    }
    gvector(ge + G_EMITTER_OrientFront, &emitter.OrientFront);
    gvector(ge + G_EMITTER_OrientTop, &emitter.OrientTop);
    gvector(ge + G_EMITTER_Position, &emitter.Position);
    gvector(ge + G_EMITTER_Velocity, &emitter.Velocity);
    emitter.InnerRadius = gfloat(ge + G_EMITTER_InnerRadius);
    emitter.InnerRadiusAngle = gfloat(ge + G_EMITTER_InnerRadiusAngle);
    emitter.ChannelCount = MEM32(ge + G_EMITTER_ChannelCount);
    emitter.ChannelRadius = gfloat(ge + G_EMITTER_ChannelRadius);
    emitter.pChannelAzimuths = gptr_or_null(MEM32(ge + G_EMITTER_pChannelAzimuths));
    emitter.pVolumeCurve = gcurve(MEM32(ge + G_EMITTER_pVolumeCurve), &curves[0]);
    emitter.pLFECurve = gcurve(MEM32(ge + G_EMITTER_pLFECurve), &curves[1]);
    emitter.pLPFDirectCurve = gcurve(MEM32(ge + G_EMITTER_pLPFDirectCurve), &curves[2]);
    emitter.pLPFReverbCurve = gcurve(MEM32(ge + G_EMITTER_pLPFReverbCurve), &curves[3]);
    emitter.pReverbCurve = gcurve(MEM32(ge + G_EMITTER_pReverbCurve), &curves[4]);
    emitter.CurveDistanceScaler = gfloat(ge + G_EMITTER_CurveDistanceScaler);
    emitter.DopplerScaler = gfloat(ge + G_EMITTER_DopplerScaler);

    /* Scalars in, so the ones FAudio does not compute for these flags come
     * back unchanged. */
    F3DAUDIO_DSP_SETTINGS dsp;
    dsp.pMatrixCoefficients = gptr_or_null(MEM32(gd + G_DSP_pMatrixCoefficients));
    dsp.pDelayTimes = gptr_or_null(MEM32(gd + G_DSP_pDelayTimes));
    memcpy(&dsp.SrcChannelCount, guest_ptr(gd + G_DSP_SrcChannelCount), G_DSP_SCALAR_BYTES);

    F3DAudioCalculate(guest_ptr(instance), &listener, &emitter, flags, &dsp);

    memcpy(guest_ptr(gd + G_DSP_SrcChannelCount), &dsp.SrcChannelCount, G_DSP_SCALAR_BYTES);
    g_esp += 4u;
}

const win32_export_t win32_exports_audio[] = {
    {"X3DAudio1_6.dll", "X3DAudioCalculate", 0, imp_X3DAudioCalculate},
    {"X3DAudio1_6.dll", "X3DAudioInitialize", 0, imp_X3DAudioInitialize},
    {0},
};
