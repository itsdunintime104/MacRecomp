/* Win32 DirectSound 8 test (runtime/src/win32_dsound.c). Replays what
 * LEGOIndy.exe does (notes/dsound.md): DirectSoundCreate8, the device set-up
 * (cooperative level, caps, speaker config, the 3D primary
 * buffer, its format and its listener), the 4-second streaming
 * buffer (QueryInterface for IDirectSoundBuffer8, wrapping Lock,
 * looping Play), and the one-shot 3D samples
 * (DuplicateSoundBuffer, IDirectSound3DBuffer, deferred settings,
 * CommitDeferredSettings). The mixer is pulled offline and its output is
 * checked against the DirectSound definitions of volume, pan and frequency.
 * Last, one device on the host audio path (SDL_AUDIO_DRIVER=dummy) must
 * advance a play cursor by itself.
 *
 * Every method is called through its guest vtable and a guest stack; call()
 * checks the stdcall pop and ebx/esi/edi/ebp. Links win32_dsound.c with
 * com_guest.c, frame_trace.c, guest_mem.c, guest_heap.c, dlmalloc.c,
 * host_funcs.c, guest_call.c, recomp_glue.c and -lFAudio. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "com_guest.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "recomp_types.h"
#include "win32_dsound.h"
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

/* ---- Guest memory and calls -------------------------------------------- */

#define GUEST_LO 0x00A00000u
#define DATA_AREA 0x00A00000u
#define STACK_TOP 0x00FE0000u
#define GUEST_HI 0x01000000u
#define RET_ADDR 0xDEAD0001u

static uint32_t g_data_next = DATA_AREA;
static uint32_t guest_alloc(uint32_t size)
{
    uint32_t a = g_data_next;
    memset(guest_ptr(a), 0, size);
    g_data_next += (size + 15u) & ~15u;
    return a;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

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

/* Method idx of obj through its guest vtable; M(obj, idx, args...) passes
 * obj as `this`. */
static recomp_func_t method(uint32_t obj, unsigned idx)
{
    return guest_resolve(MEM32(MEM32(obj) + 4u * idx));
}
#define M(obj, idx, ...) CALL(method((obj), (idx)), (obj), ##__VA_ARGS__)

static uint32_t guid_at(const com_guid_t *g)
{
    uint32_t a = guest_alloc(16);
    MEM32(a) = g->d1;
    MEM16(a + 4) = g->d2;
    MEM16(a + 6) = g->d3;
    memcpy(guest_ptr(a + 8), g->d4, 8);
    return a;
}

/* ---- dsound.h ----------------------------------------------------------- */

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_NODRIVER 0x88780078u
#define DSERR_UNSUPPORTED 0x80004001u
#define E_NOINTERFACE 0x80004002u

#define DSSCL_PRIORITY 2u
#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBCAPS_LOCSOFTWARE 0x8u
#define DSBCAPS_CTRL3D 0x10u
#define DSBCAPS_CTRLFREQUENCY 0x20u
#define DSBCAPS_CTRLPAN 0x40u
#define DSBCAPS_CTRLVOLUME 0x80u
#define DSBCAPS_GETCURRENTPOSITION2 0x10000u
#define DSBPLAY_LOOPING 1u
#define DSBLOCK_ENTIREBUFFER 2u
#define DSBSTATUS_PLAYING 1u
#define DSBSTATUS_LOOPING 4u
#define DS3D_IMMEDIATE 0u
#define DS3D_DEFERRED 1u
#define DS3DMODE_NORMAL 0u
#define DS3DMODE_HEADRELATIVE 1u
#define DS3DMODE_DISABLE 2u

/* IDirectSound8 slots. */
enum { DS_QI, DS_AddRef, DS_Release, DS_CreateSoundBuffer, DS_GetCaps, DS_Duplicate,
       DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig };
/* IDirectSoundBuffer8 slots. */
enum { B_QI, B_AddRef, B_Release, B_GetCaps, B_GetCurrentPosition, B_GetFormat, B_GetVolume,
       B_GetPan, B_GetFrequency, B_GetStatus, B_Initialize, B_Lock, B_Play,
       B_SetCurrentPosition, B_SetFormat, B_SetVolume, B_SetPan, B_SetFrequency, B_Stop,
       B_Unlock, B_Restore };
/* IDirectSound3DBuffer slots used by the game. */
enum { B3_SetMaxDistance = 16, B3_SetMinDistance, B3_SetMode, B3_SetPosition };
/* IDirectSound3DListener slots used by the game. */
enum { L_SetOrientation = 13, L_SetPosition, L_SetRolloffFactor, L_SetVelocity,
       L_CommitDeferredSettings };

static const com_guid_t IID_IDirectSoundBuffer8 = {
    0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
static const com_guid_t IID_IDirectSound3DListener = {
    0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t IID_IDirectSound3DBuffer = {
    0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t IID_Other = {
    0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}};

#define RATE 48000u

static recomp_func_t create8(void)
{
    for (const win32_export_t *e = win32_exports_dsound; e->dll != NULL; e++)
        if (e->ordinal == 11 && strcmp(e->name, "DirectSoundCreate8") == 0)
            return e->fn;
    return NULL;
}

static uint32_t wfx_at(uint32_t ch, uint32_t rate, uint32_t bits)
{
    uint32_t w = guest_alloc(18);
    MEM16(w + 0) = 1; /* WAVE_FORMAT_PCM */
    MEM16(w + 2) = (uint16_t)ch;
    MEM32(w + 4) = rate;
    MEM32(w + 8) = rate * ch * bits / 8u;
    MEM16(w + 12) = (uint16_t)(ch * bits / 8u);
    MEM16(w + 14) = (uint16_t)bits;
    return w;
}

/* CreateSoundBuffer with a 36-byte DSBUFFERDESC; returns the HRESULT. */
static uint32_t create_buffer(uint32_t ds, uint32_t flags, uint32_t bytes, uint32_t wfx,
                              uint32_t *out)
{
    uint32_t d = guest_alloc(36), pp = guest_alloc(4);
    MEM32(d + 0) = 36;
    MEM32(d + 4) = flags;
    MEM32(d + 8) = bytes;
    MEM32(d + 16) = wfx;
    uint32_t hr = M(ds, DS_CreateSoundBuffer, d, pp, 0);
    *out = MEM32(pp);
    return hr;
}

static uint32_t query(uint32_t obj, const com_guid_t *iid, uint32_t *out)
{
    uint32_t pp = guest_alloc(4);
    uint32_t hr = M(obj, 0, guid_at(iid), pp);
    *out = MEM32(pp);
    return hr;
}

static uint32_t play_cursor(uint32_t b)
{
    uint32_t p = guest_alloc(8);
    CHECK(M(b, B_GetCurrentPosition, p, p + 4) == DS_OK);
    return MEM32(p);
}

static uint32_t status(uint32_t b)
{
    uint32_t p = guest_alloc(4);
    CHECK(M(b, B_GetStatus, p) == DS_OK);
    return MEM32(p);
}

/* Lock [offset, offset + bytes) and fill every 16-bit sample i of the
 * locked range (counted from offset, through the wrap) with f(i). Returns
 * the second region's size. */
static uint32_t write16(uint32_t b, uint32_t offset, uint32_t bytes, uint32_t flags,
                        int16_t (*f)(uint32_t))
{
    uint32_t p = guest_alloc(16);
    CHECK(M(b, B_Lock, offset, bytes, p, p + 4, p + 8, p + 12, flags) == DS_OK);
    uint32_t n1 = MEM32(p + 4), n2 = MEM32(p + 12), i = 0;
    for (uint32_t k = 0; k < n1 / 2u; k++)
        MEM16(MEM32(p) + 2u * k) = (uint16_t)f(i++);
    for (uint32_t k = 0; k < n2 / 2u; k++)
        MEM16(MEM32(p + 8) + 2u * k) = (uint16_t)f(i++);
    CHECK(M(b, B_Unlock, MEM32(p), n1, MEM32(p + 8), n2) == DS_OK);
    return n2;
}

static float g_out[2 * 8192];
static void pull(uint32_t ds, uint32_t frames)
{
    w32_dsound_render(ds, g_out, frames);
}

static int near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

/* ---- Waveforms ---------------------------------------------------------- */

/* Stereo ramp: sample i is frame i/2, channel i%2. */
static int16_t ramp(uint32_t i)
{
    return (int16_t)(((i / 2u) * 37u % 20000u) - 10000 + (i % 2u ? 500 : 0));
}
static float ramp_f(uint32_t frame, uint32_t ch)
{
    return (float)ramp(2u * frame + ch) / 32768.0f;
}
static int16_t marker(uint32_t i)
{
    return (int16_t)(20000 + (int)i);
}
static int16_t half(uint32_t i)
{
    (void)i;
    return 16384; /* 0.5 */
}
static int16_t mono_ramp(uint32_t i)
{
    return (int16_t)(i * 50u);
}

/* ---- Tests -------------------------------------------------------------- */

static uint32_t g_ds, g_primary, g_listener;

static void test_device(void)
{
    recomp_func_t create = create8();
    CHECK(create != NULL);
    uint32_t pp = guest_alloc(4);

    /* An unknown device GUID is DSERR_NODRIVER. */
    CHECK(CALL(create, guid_at(&IID_Other), pp, 0) == DSERR_NODRIVER && MEM32(pp) == 0);

    w32_dsound_set_offline_rate(RATE);
    CHECK(CALL(create, 0, pp, 0) == DS_OK); /* as the game calls it */
    g_ds = MEM32(pp);
    CHECK(g_ds != 0);

    CHECK(M(g_ds, DS_SetCooperativeLevel, 0x00010001u, DSSCL_PRIORITY) == DS_OK);
    uint32_t caps = guest_alloc(96);
    MEM32(caps) = 0x60;
    CHECK(M(g_ds, DS_GetCaps, caps) == DS_OK);
    CHECK(MEM32(caps + 8) == 100 && MEM32(caps + 12) == 200000);
    uint32_t spk = guest_alloc(4);
    CHECK(M(g_ds, DS_GetSpeakerConfig, spk) == DS_OK && (MEM32(spk) & 0xFFu) == 4u);

    /* The 3D primary buffer, 44.1 kHz stereo 16-bit, its listener. */
    CHECK(create_buffer(g_ds, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, 0, &g_primary) ==
          DS_OK);
    CHECK(g_primary != 0);
    CHECK(M(g_primary, B_SetFormat, wfx_at(2, 44100, 16)) == DS_OK);
    CHECK(query(g_primary, &IID_IDirectSound3DListener, &g_listener) == DS_OK);
    CHECK(g_listener != 0 && g_listener != g_primary);
    CHECK(M(g_listener, L_SetPosition, fbits(0), fbits(0), fbits(0), DS3D_DEFERRED) == DS_OK);
    CHECK(M(g_listener, L_SetOrientation, fbits(0), fbits(0), fbits(1), fbits(0), fbits(1),
            fbits(0), DS3D_DEFERRED) == DS_OK);
    CHECK(M(g_listener, L_SetRolloffFactor, fbits(0), DS3D_DEFERRED) == DS_OK);
    CHECK(M(g_listener, L_CommitDeferredSettings) == DS_OK);
    CHECK(M(g_listener, L_SetRolloffFactor, fbits(11.0f), DS3D_IMMEDIATE) == DSERR_INVALIDPARAM);

    /* A method the game never calls: E_NOTIMPL, still popping its args. */
    CHECK(M(g_listener, L_SetVelocity, fbits(0), fbits(0), fbits(0), 0) == DSERR_UNSUPPORTED);
}

/* The stream: 16-bit stereo, 4096 frames, IDirectSoundBuffer8, looping. */
static uint32_t g_stream;
#define SFRAMES 4096u

static void test_stream(void)
{
    uint32_t b1;
    CHECK(create_buffer(g_ds, DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2, SFRAMES * 4u,
                        wfx_at(2, RATE, 16), &b1) == DS_OK);
    CHECK(query(b1, &IID_IDirectSoundBuffer8, &g_stream) == DS_OK && g_stream != 0);
    uint32_t junk;
    CHECK(query(b1, &IID_IDirectSound3DBuffer, &junk) == E_NOINTERFACE && junk == 0);
    CHECK(M(b1, B_Release) == 1); /* the game drops the IDirectSoundBuffer */
    CHECK(M(g_stream, B_SetVolume, 0u) == DS_OK);

    CHECK(write16(g_stream, 0, 0, DSBLOCK_ENTIREBUFFER, ramp) == 0);
    CHECK(play_cursor(g_stream) == 0 && status(g_stream) == 0);

    /* Stopped: silence, no movement. */
    pull(g_ds, 64);
    CHECK(g_out[0] == 0.0f && g_out[127] == 0.0f && play_cursor(g_stream) == 0);

    CHECK(M(g_stream, B_Play, 0, 0, DSBPLAY_LOOPING) == DS_OK);
    CHECK(status(g_stream) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING));
    pull(g_ds, 1000);
    int ok = 1;
    for (uint32_t i = 0; i < 1000; i++)
        ok &= g_out[2 * i] == ramp_f(i, 0) && g_out[2 * i + 1] == ramp_f(i, 1);
    CHECK(ok);
    CHECK(play_cursor(g_stream) == 1000u * 4u);

    /* Past the end it loops back to frame 0. */
    pull(g_ds, 4000);
    CHECK(play_cursor(g_stream) == (5000u - SFRAMES) * 4u);
    CHECK(g_out[2 * 3096] == ramp_f(0, 0) && g_out[2 * 3097 + 1] == ramp_f(1, 1));

    /* A lock that wraps past the end: 100 frames before it, 100 after. */
    uint32_t n2 = write16(g_stream, (SFRAMES - 100u) * 4u, 200u * 4u, 0, marker);
    CHECK(n2 == 100u * 4u);
    CHECK(M(g_stream, B_SetCurrentPosition, (SFRAMES - 100u) * 4u) == DS_OK);
    pull(g_ds, 200);
    ok = 1;
    for (uint32_t i = 0; i < 400; i++)
        ok &= g_out[i] == (float)marker(i) / 32768.0f;
    CHECK(ok);
    CHECK(play_cursor(g_stream) == 100u * 4u);

    /* Volume in hundredths of a dB: -600 is 10^(-6/20). */
    CHECK(M(g_stream, B_SetVolume, (uint32_t)-600) == DS_OK);
    uint32_t v = guest_alloc(4);
    CHECK(M(g_stream, B_GetVolume, v) == DS_OK && (int32_t)MEM32(v) == -600);
    CHECK(M(g_stream, B_SetVolume, (uint32_t)-10001) == DSERR_INVALIDPARAM);
    pull(g_ds, 10);
    float g = powf(10.0f, -0.3f);
    CHECK(near(g_out[0], g * ramp_f(100, 0)) && near(g_out[19], g * ramp_f(109, 1)));
    CHECK(M(g_stream, B_SetVolume, 0u) == DS_OK);
    CHECK(M(g_stream, B_SetPan, 0u) == DSERR_CONTROLUNAVAIL);

    /* Stop holds the cursor and silences the buffer. */
    CHECK(M(g_stream, B_Stop) == DS_OK);
    uint32_t at = play_cursor(g_stream);
    pull(g_ds, 50);
    CHECK(g_out[0] == 0.0f && g_out[99] == 0.0f && play_cursor(g_stream) == at);
    CHECK(status(g_stream) == 0);
    CHECK(M(g_stream, B_Restore) == DS_OK);
}

static void test_pan(void)
{
    uint32_t b;
    CHECK(create_buffer(g_ds, DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME, 256u * 2u,
                        wfx_at(1, RATE, 16), &b) == DS_OK);
    write16(b, 0, 0, DSBLOCK_ENTIREBUFFER, half);
    CHECK(M(b, B_Play, 0, 0, DSBPLAY_LOOPING) == DS_OK);
    pull(g_ds, 4);
    CHECK(g_out[0] == 0.5f && g_out[1] == 0.5f); /* mono goes to both sides */
    /* Pan +1000 attenuates the left channel by 10 dB, -1000 the right. */
    float g = powf(10.0f, -0.5f);
    CHECK(M(b, B_SetPan, 1000u) == DS_OK);
    pull(g_ds, 4);
    CHECK(near(g_out[0], 0.5f * g) && g_out[1] == 0.5f);
    CHECK(M(b, B_SetPan, (uint32_t)-1000) == DS_OK);
    pull(g_ds, 4);
    CHECK(g_out[0] == 0.5f && near(g_out[1], 0.5f * g));
    CHECK(M(b, B_SetPan, 10001u) == DSERR_INVALIDPARAM);
    CHECK(M(b, B_Release) == 0);
    CHECK(com_guest_iface(b) == NULL);
}

static void test_frequency(void)
{
    uint32_t b;
    CHECK(create_buffer(g_ds, DSBCAPS_CTRLFREQUENCY, 1000u * 2u, wfx_at(1, 24000, 16), &b) ==
          DS_OK);
    write16(b, 0, 0, DSBLOCK_ENTIREBUFFER, mono_ramp);
    CHECK(M(b, B_Play, 0, 0, 0) == DS_OK);
    /* 24 kHz into 48 kHz: one source frame per two output frames, linearly
     * interpolated. */
    pull(g_ds, 200);
    int ok = 1;
    for (uint32_t k = 0; k < 100; k++) {
        float s0 = (float)mono_ramp(k) / 32768.0f, s1 = (float)mono_ramp(k + 1) / 32768.0f;
        ok &= near(g_out[4 * k], s0) && near(g_out[4 * k + 2], 0.5f * (s0 + s1)) &&
              g_out[4 * k] == g_out[4 * k + 1];
    }
    CHECK(ok);
    CHECK(play_cursor(b) == 100u * 2u);
    CHECK(M(b, B_SetFrequency, 48000u) == DS_OK);
    pull(g_ds, 100);
    CHECK(near(g_out[0], (float)mono_ramp(100) / 32768.0f) && play_cursor(b) == 200u * 2u);
    CHECK(M(b, B_SetFrequency, 96000u) == DS_OK);
    pull(g_ds, 100);
    CHECK(play_cursor(b) == 400u * 2u);
    CHECK(M(b, B_SetFrequency, 0u) == DS_OK); /* DSBFREQUENCY_ORIGINAL: 24 kHz again */
    pull(g_ds, 100);
    CHECK(play_cursor(b) == 450u * 2u);
    CHECK(M(b, B_SetFrequency, 50u) == DSERR_INVALIDPARAM);
    CHECK(M(b, B_Release) == 0);
}

/* The game's samples: one 3D, frequency and volume buffer per sound, a
 * duplicate per voice, 3D settings deferred until the per-frame commit. */
static void test_samples(void)
{
    const uint32_t flags =
        DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_LOCSOFTWARE;
    uint32_t junk;
    CHECK(create_buffer(g_ds, flags, 400u, wfx_at(2, RATE, 16), &junk) == DSERR_INVALIDPARAM);

    uint32_t s;
    CHECK(create_buffer(g_ds, flags, 500u * 2u, wfx_at(1, RATE, 16), &s) == DS_OK);
    write16(s, 0, 500u * 2u, 0, half);

    uint32_t pp = guest_alloc(4);
    CHECK(M(g_ds, DS_Duplicate, s, pp) == DS_OK); /* as the game calls it */
    uint32_t voice = MEM32(pp), v3d;
    CHECK(voice != 0 && voice != s);
    CHECK(query(voice, &IID_IDirectSound3DBuffer, &v3d) == DS_OK && v3d != 0);
    CHECK(M(v3d, B3_SetMaxDistance, fbits(1e9f), DS3D_DEFERRED) == DS_OK);
    CHECK(M(v3d, B3_SetMinDistance, fbits(1e9f), DS3D_DEFERRED) == DS_OK);
    CHECK(M(v3d, B3_SetMode, DS3DMODE_HEADRELATIVE, DS3D_DEFERRED) == DS_OK);
    CHECK(M(v3d, B3_SetPosition, fbits(1), fbits(0), fbits(0), DS3D_DEFERRED) == DS_OK);
    CHECK(M(voice, B_SetVolume, 0u) == DS_OK && M(voice, B_SetFrequency, RATE) == DS_OK);

    /* Not committed yet: the source is still at the listener, centred. */
    CHECK(M(voice, B_Play, 0, 0, 0) == DS_OK && status(voice) == DSBSTATUS_PLAYING);
    pull(g_ds, 10);
    CHECK(g_out[0] == 0.5f && g_out[1] == 0.5f);
    /* Committed: head-relative +x is full right. */
    CHECK(M(g_listener, L_CommitDeferredSettings) == DS_OK);
    pull(g_ds, 10);
    CHECK(g_out[0] < 1e-4f && g_out[1] == 0.5f);
    /* Immediate: -x is full left at once. */
    CHECK(M(v3d, B3_SetPosition, fbits(-1), fbits(0), fbits(0), DS3D_IMMEDIATE) == DS_OK);
    pull(g_ds, 10);
    CHECK(g_out[0] == 0.5f && g_out[1] < 1e-4f);
    CHECK(M(v3d, B3_SetMode, 7u, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM);

    /* One shot: the remaining 470 frames, then silence, stopped, cursor 0. */
    CHECK(play_cursor(voice) == 30u * 2u);
    pull(g_ds, 600);
    CHECK(g_out[2 * 469] == 0.5f && g_out[2 * 470] == 0.0f && g_out[2 * 599 + 1] == 0.0f);
    CHECK(status(voice) == 0 && play_cursor(voice) == 0);

    /* Distance, normal mode: rolloff 1, min 1, source 2 ahead: 6 dB down. */
    CHECK(M(g_listener, L_SetRolloffFactor, fbits(1), DS3D_IMMEDIATE) == DS_OK);
    CHECK(M(v3d, B3_SetMode, DS3DMODE_NORMAL, DS3D_IMMEDIATE) == DS_OK);
    CHECK(M(v3d, B3_SetMinDistance, fbits(1), DS3D_IMMEDIATE) == DS_OK);
    CHECK(M(v3d, B3_SetPosition, fbits(0), fbits(0), fbits(2), DS3D_IMMEDIATE) == DS_OK);
    CHECK(M(voice, B_Play, 0, 0, 0) == DS_OK);
    pull(g_ds, 4);
    CHECK(near(g_out[0], 0.25f) && near(g_out[1], 0.25f));
    /* Beyond the max distance there is no further attenuation. */
    CHECK(M(v3d, B3_SetMaxDistance, fbits(1.5f), DS3D_IMMEDIATE) == DS_OK);
    pull(g_ds, 4);
    CHECK(near(g_out[0], 0.5f / 1.5f) && near(g_out[1], 0.5f / 1.5f));
    /* Turn the listener to face +x: the source at +z is now on its left. */
    CHECK(M(g_listener, L_SetOrientation, fbits(1), fbits(0), fbits(0), fbits(0), fbits(1),
            fbits(0), DS3D_IMMEDIATE) == DS_OK);
    pull(g_ds, 4);
    CHECK(near(g_out[0], 0.5f / 1.5f) && g_out[1] < 1e-4f);
    /* DS3DMODE_DISABLE: no 3D processing. */
    CHECK(M(v3d, B3_SetMode, DS3DMODE_DISABLE, DS3D_IMMEDIATE) == DS_OK);
    pull(g_ds, 4);
    CHECK(g_out[0] == 0.5f && g_out[1] == 0.5f);
    CHECK(M(g_listener, L_SetOrientation, fbits(0), fbits(0), fbits(1), fbits(0), fbits(1),
            fbits(0), DS3D_IMMEDIATE) == DS_OK);
    CHECK(M(g_listener, L_SetRolloffFactor, fbits(0), DS3D_IMMEDIATE) == DS_OK);

    /* The game's voice teardown: 3D buffer, Stop, Release. */
    CHECK(M(v3d, B_Release) == 1);
    CHECK(M(voice, B_Stop) == DS_OK);
    CHECK(M(voice, B_Release) == 0);
    CHECK(com_guest_iface(voice) == NULL && com_guest_iface(v3d) == NULL);

    /* The original still owns the shared audio memory. */
    CHECK(M(s, B_Play, 0, 0, 0) == DS_OK);
    pull(g_ds, 4);
    CHECK(g_out[0] == 0.5f && g_out[1] == 0.5f);
    CHECK(M(s, B_Release) == 0);
}

static void test_teardown(void)
{
    /* The game's shutdown: listener, primary, device. The stream is still alive and
     * goes with the device. */
    CHECK(M(g_listener, B_Release) == 1);
    CHECK(M(g_primary, B_Release) == 0);
    CHECK(com_guest_iface(g_primary) == NULL && com_guest_iface(g_listener) == NULL);
    CHECK(com_guest_iface(g_stream) != NULL);
    CHECK(M(g_ds, DS_Release) == 0);
    CHECK(com_guest_iface(g_ds) == NULL && com_guest_iface(g_stream) == NULL);
}

/* The host path: FAudio's mixer thread advances a looping buffer. */
static void test_host_device(void)
{
    w32_dsound_set_offline_rate(0);
    uint32_t pp = guest_alloc(4);
    CHECK(CALL(create8(), 0, pp, 0) == DS_OK);
    uint32_t ds = MEM32(pp);
    if (ds == 0)
        return;
    uint32_t b;
    CHECK(create_buffer(ds, DSBCAPS_CTRLVOLUME, 44100u * 4u, wfx_at(2, 44100, 16), &b) == DS_OK);
    CHECK(M(b, B_Play, 0, 0, DSBPLAY_LOOPING) == DS_OK);
    uint32_t cur = 0;
    for (int i = 0; i < 200 && cur == 0; i++) {
        nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
        cur = play_cursor(b);
    }
    CHECK(cur != 0);
    CHECK(M(b, B_Stop) == DS_OK && M(b, B_Release) == 0);
    CHECK(M(ds, DS_Release) == 0);
}

int main(void)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(GUEST_LO), GUEST_HI - GUEST_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    test_device();
    test_stream();
    test_pan();
    test_frequency();
    test_samples();
    test_teardown();
    test_host_device();
    puts(g_fail ? "test_win32_dsound: FAIL" : "test_win32_dsound: PASS");
    return g_fail;
}
