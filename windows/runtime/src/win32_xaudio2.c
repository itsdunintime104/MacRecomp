/* Win32 audio slice, XAudio2 2.4 COM layer on FAudio (docs/AUDIO_PLAN.md).
 * The game creates IXAudio2 and the AudioReverb effect through
 * ole32!CoCreateInstance; this file registers those 4 CLSIDs with the COM
 * slice and exports no imports. Contract: win32_thunk.h, com_guest.h.
 *
 * Every method is __stdcall with `this` at W32_ARG(0) and pops
 * 4 + 4 * (1 + n) bytes. Vtable order and argument counts are the plan's
 * section 2 (Wine's IXAudio27* interfaces, which 2.4 uses). Voices are not
 * COM objects: slot 0 of a voice is GetVoiceDetails.
 *
 * Structs with no pointers have the same bytes on both sides (FAudio.h is
 * pack 1, as the XAudio2 headers are) and go to FAudio as guest_ptr(). Structs
 * with pointers are copied field by field at the guest offsets of the plan's
 * section 3. Guest NULL maps to host NULL.
 *
 * FAudio mixes on SDL's audio thread and calls the voice and engine callbacks
 * there. Each shim gives that thread a guest stack and a TIB with
 * w32_host_thread_attach() before it calls guest code. */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <FAudio.h>
#include <FAudioFX.h>
#include <FAPO.h>

#include "com_guest.h"
#include "diag.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "win32_thunk.h"

/* Structs passed through as guest_ptr(): guest size (plan section 3) must
 * equal the host size. */
_Static_assert(sizeof(FAudioWaveFormatEx) == 18, "WAVEFORMATEX is 18 bytes");
_Static_assert(sizeof(FAudioDeviceDetails) == 1068, "XAUDIO2_DEVICE_DETAILS is 1068 bytes");
_Static_assert(sizeof(FAudioFilterParameters) == 12, "XAUDIO2_FILTER_PARAMETERS is 12 bytes");
_Static_assert(sizeof(FAudioPerformanceData) == 64, "XAUDIO2_PERFORMANCE_DATA is 64 bytes");
_Static_assert(sizeof(FAudioDebugConfiguration) == 24, "XAUDIO2_DEBUG_CONFIGURATION is 24 bytes");
_Static_assert(sizeof(FAudioFXReverbParameters) == 52, "XAUDIO2FX_REVERB_PARAMETERS is 52 bytes");

/* Guest XAUDIO27_VOICE_DETAILS (12 bytes; no ActiveFlags before 2.8). */
#define G_VD_CreationFlags 0u
#define G_VD_InputChannels 4u
#define G_VD_InputSampleRate 8u

/* Guest XAUDIO2_VOICE_SENDS (8) and XAUDIO2_SEND_DESCRIPTOR (8). */
#define G_SENDS_SendCount 0u
#define G_SENDS_pSends 4u
#define G_SEND_Flags 0u
#define G_SEND_pOutputVoice 4u
#define G_SEND_SIZE 8u

/* Guest XAUDIO2_EFFECT_CHAIN (8) and XAUDIO2_EFFECT_DESCRIPTOR (12). */
#define G_CHAIN_EffectCount 0u
#define G_CHAIN_pEffectDescriptors 4u
#define G_EFFECT_pEffect 0u
#define G_EFFECT_InitialState 4u
#define G_EFFECT_OutputChannels 8u
#define G_EFFECT_SIZE 12u

/* Guest XAUDIO2_BUFFER (36). */
#define G_BUF_Flags 0u
#define G_BUF_AudioBytes 4u
#define G_BUF_pAudioData 8u
#define G_BUF_PlayBegin 12u
#define G_BUF_PlayLength 16u
#define G_BUF_LoopBegin 20u
#define G_BUF_LoopLength 24u
#define G_BUF_LoopCount 28u
#define G_BUF_pContext 32u

/* Guest XAUDIO2_BUFFER_WMA (8). */
#define G_WMA_pDecodedPacketCumulativeBytes 0u
#define G_WMA_PacketCount 4u

/* Guest XAUDIO2_VOICE_STATE (16). */
#define G_VS_pCurrentBufferContext 0u
#define G_VS_BuffersQueued 4u
#define G_VS_SamplesPlayed 8u

/* Guest callback vtable slots (plan section 2). */
enum {
    VCB_PassStart, /* OnVoiceProcessingPassStart(BytesRequired) */
    VCB_PassEnd,
    VCB_StreamEnd,
    VCB_BufferStart, /* (pBufferContext) */
    VCB_BufferEnd,
    VCB_LoopEnd,
    VCB_VoiceError, /* (pBufferContext, Error) */
};
enum { ECB_PassStart, ECB_PassEnd, ECB_CriticalError };

#define XA2_ENGINE_CB_MAX 8u

static const com_guid_t IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t IID_IXAudio2 = {
    0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
static const com_guid_t CLSID_XAudio2_24 = {
    0x03219e78, 0x5bc3, 0x44d1, {0xb9, 0x2e, 0xf6, 0x3d, 0x89, 0xcc, 0x65, 0x26}};
static const com_guid_t CLSID_XAudio2_Debug_24 = {
    0x4256535c, 0x1ea4, 0x4d4b, {0x8a, 0xd5, 0xf9, 0xdb, 0x76, 0x2e, 0xca, 0x9e}};
static const com_guid_t CLSID_AudioReverb_24 = {
    0x8bb7778b, 0x645b, 0x4475, {0x9a, 0x73, 0x1d, 0xe3, 0x17, 0x0b, 0xd3, 0xaf}};
static const com_guid_t CLSID_AudioReverb_Debug_24 = {
    0xda7738a2, 0xcd0c, 0x4367, {0x9a, 0xac, 0xd7, 0xea, 0xd7, 0xc6, 0x4f, 0x98}};

/* ---- Host objects -------------------------------------------------------- */

struct xa2_engine {
    FAudioEngineCallback cb; /* first member: the shims cast back */
    FAudio *fa;
    uint32_t refs;
    pthread_mutex_t cb_lock; /* guest_cbs: guest thread writes, mixer reads */
    uint32_t guest_cbs[XA2_ENGINE_CB_MAX];
    uint32_t n_cbs;
    FAudioVoice *master; /* trace only: channel count of the output */
};

struct xa2_voice {
    FAudioVoiceCallback cb; /* first member: the shims cast back */
    FAudioVoice *v;
    uint32_t guest_obj;
    uint32_t guest_cb; /* guest IXAudio2VoiceCallback, or 0 */
    uint32_t id;       /* trace only: index into g_vstat, 0 when tracing is off */
};

struct xa2_reverb {
    FAPO *fapo;
    uint32_t refs;
};

static const com_interface_t IXAudio2_iface, SourceVoice_iface, SubmixVoice_iface,
    MasteringVoice_iface, AudioReverb_iface;

static void *gptr_or_null(uint32_t a)
{
    return a != 0 ? guest_ptr(a) : NULL;
}

static float arg_float(unsigned i)
{
    uint32_t bits = W32_ARG(i);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static struct xa2_engine *engine_self(void)
{
    return com_guest_self(W32_ARG(0));
}

static struct xa2_voice *voice_self(void)
{
    return com_guest_self(W32_ARG(0));
}

/* The FAudio voice behind a guest voice object, or NULL for guest NULL. */
static FAudioVoice *voice_of(uint32_t guest_voice)
{
    return guest_voice != 0 ? ((struct xa2_voice *)com_guest_self(guest_voice))->v : NULL;
}


/* ---- HPRECOMP_TRACE_AUDIO=1: voice and output trace ---------------------- */
/* Lines on stderr start with "AUD <seconds>". Voice events (capped at
 * ATRACE_MAX_LINES): creation formats, the first submits per voice, Start,
 * Stop, volume and ratio changes, output matrices. Once per output second:
 * frames FAudio mixed for the host device, peak and RMS (dBFS) per channel,
 * samples over full scale, and the bytes each source voice submitted that
 * second (a music voice at 44100 Hz stereo 16-bit should submit ~176400).
 * HPRECOMP_AUDIO_WAV=<path> also writes that mixed output (float32 WAV). */
#define ATRACE_MAX_LINES 6000u
#define ATRACE_MAX_VOICES 1024u
#define ATRACE_MAX_CH 8u

struct atrace_voice {
    uint32_t rate, align, ch, tag;
    uint64_t submits, bytes, last_bytes;
    uint32_t vol_bits, ratio_bits;
    char kind; /* 's' source, 'x' submix, 'm' mastering */
};

static int g_atrace = -1;
static uint32_t g_atrace_lines, g_atrace_ids;
static struct atrace_voice g_vstat[ATRACE_MAX_VOICES];
static uint32_t g_starts, g_stops;
static struct timespec g_atrace_t0;

static int atrace_on(void)
{
    if (g_atrace < 0) {
        const char *s = getenv("HPRECOMP_TRACE_AUDIO");
        g_atrace = s != NULL && *s != '\0' && *s != '0';
        clock_gettime(CLOCK_MONOTONIC, &g_atrace_t0);
    }
    return g_atrace;
}

static double atrace_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - g_atrace_t0.tv_sec) + (t.tv_nsec - g_atrace_t0.tv_nsec) / 1e9;
}

static void atrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void atrace(const char *fmt, ...)
{
    if (!atrace_on() || __atomic_fetch_add(&g_atrace_lines, 1, __ATOMIC_RELAXED) >= ATRACE_MAX_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    flockfile(stderr);
    fprintf(stderr, "AUD %.3f ", atrace_now());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
}

/* A trace slot for a new voice, or 0 when tracing is off or the table is full. */
static uint32_t atrace_voice_new(char kind, uint32_t tag, uint32_t ch, uint32_t rate, uint32_t align)
{
    if (!atrace_on())
        return 0;
    uint32_t id = __atomic_add_fetch(&g_atrace_ids, 1, __ATOMIC_RELAXED);
    if (id >= ATRACE_MAX_VOICES)
        return 0;
    struct atrace_voice *v = &g_vstat[id];
    v->kind = kind;
    v->tag = tag;
    v->ch = ch;
    v->rate = rate;
    v->align = align;
    v->vol_bits = 0x3F800000u; /* 1.0 */
    v->ratio_bits = 0x3F800000u;
    return id;
}

static double bits_float(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/* Mixer-thread state of the per-second output statistics. */
static struct {
    uint32_t ch, rate, quantum;
    uint64_t frames, sec_frames;
    unsigned sec;
    double sumsq[ATRACE_MAX_CH];
    float peak[ATRACE_MAX_CH];
    uint32_t clip;
    FILE *wav;
    uint32_t wav_bytes;
} g_out;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* RIFF header for IEEE float32 data of g_out.wav_bytes bytes, at offset 0. */
static void wav_header(void)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + g_out.wav_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put32(h + 20, 3u | (g_out.ch << 16)); /* WAVE_FORMAT_IEEE_FLOAT, channels */
    put32(h + 24, g_out.rate);
    put32(h + 28, g_out.rate * g_out.ch * 4);
    put32(h + 32, (g_out.ch * 4) | (32u << 16));
    memcpy(h + 36, "data", 4);
    put32(h + 40, g_out.wav_bytes);
    fseek(g_out.wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_out.wav);
    fseek(g_out.wav, 0, SEEK_END);
}

static double dbfs(double x)
{
    return x > 0 ? 20.0 * log10(x) : -999.0;
}

static void atrace_second(void)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "out t=%u frames=%llu", g_out.sec,
                     (unsigned long long)g_out.sec_frames);
    for (uint32_t c = 0; c < g_out.ch && c < ATRACE_MAX_CH; c++)
        n += snprintf(buf + n, sizeof buf - n, " ch%u peak=%.1f rms=%.1f", c, dbfs(g_out.peak[c]),
                      dbfs(sqrt(g_out.sumsq[c] / (double)g_out.sec_frames)));
    snprintf(buf + n, sizeof buf - n, " clip=%u starts=%u stops=%u", g_out.clip,
             __atomic_load_n(&g_starts, __ATOMIC_RELAXED), __atomic_load_n(&g_stops, __ATOMIC_RELAXED));
    flockfile(stderr);
    fprintf(stderr, "AUD %.3f %s\n", atrace_now(), buf);
    uint32_t ids = __atomic_load_n(&g_atrace_ids, __ATOMIC_RELAXED);
    for (uint32_t i = 1; i <= ids && i < ATRACE_MAX_VOICES; i++) {
        struct atrace_voice *v = &g_vstat[i];
        uint64_t b = __atomic_load_n(&v->bytes, __ATOMIC_RELAXED);
        if (b != v->last_bytes)
            fprintf(stderr, "AUD %.3f   v%u rate=%u ch=%u +%llu B/s vol=%g\n", atrace_now(), i, v->rate,
                    v->ch, (unsigned long long)(b - v->last_bytes),
                    bits_float(v->vol_bits));
        v->last_bytes = b;
    }
    funlockfile(stderr);
    memset(g_out.sumsq, 0, sizeof g_out.sumsq);
    memset(g_out.peak, 0, sizeof g_out.peak);
    g_out.clip = 0;
    g_out.sec_frames = 0;
    g_out.sec++;
    if (g_out.wav != NULL) {
        wav_header();
        fflush(g_out.wav);
    }
}

/* FAudio engine procedure: mix as usual, then measure the float output that
 * goes to the platform device. */
static void FAUDIOCALL atrace_engine_proc(FAudioEngineCallEXT mix, FAudio *fa, float *out, void *user)
{
    mix(fa, out);
    struct xa2_engine *e = user;
    if (g_out.quantum == 0) {
        if (e->master == NULL)
            return;
        FAudioVoiceDetails d;
        FAudioVoice_GetVoiceDetails(e->master, &d);
        FAudio_GetProcessingQuantum(fa, &g_out.quantum, &g_out.rate);
        g_out.ch = d.InputChannels;
        atrace("out device ch=%u rate=%u quantum=%u frames", g_out.ch, g_out.rate, g_out.quantum);
        const char *path = getenv("HPRECOMP_AUDIO_WAV");
        if (path != NULL && *path != '\0' && (g_out.wav = fopen(path, "w+b")) != NULL)
            wav_header();
    }
    uint32_t ch = g_out.ch;
    for (uint32_t i = 0; i < g_out.quantum; i++) {
        for (uint32_t c = 0; c < ch; c++) {
            float a = fabsf(out[i * ch + c]);
            if (c < ATRACE_MAX_CH) {
                g_out.sumsq[c] += (double)a * a;
                if (a > g_out.peak[c])
                    g_out.peak[c] = a;
            }
            if (a > 1.0f)
                g_out.clip++;
        }
    }
    if (g_out.wav != NULL) {
        fwrite(out, sizeof(float) * ch, g_out.quantum, g_out.wav);
        g_out.wav_bytes += (uint32_t)(sizeof(float) * ch * g_out.quantum);
    }
    g_out.frames += g_out.quantum;
    g_out.sec_frames += g_out.quantum;
    if (g_out.sec_frames >= g_out.rate)
        atrace_second();
}

/* ---- Struct marshalling -------------------------------------------------- */

/* Host copy of the guest XAUDIO2_VOICE_SENDS at g into *out, or NULL for
 * guest NULL. The caller frees out->pSends; FAudio copies the list. */
static const FAudioVoiceSends *sends_in(uint32_t g, FAudioVoiceSends *out)
{
    if (g == 0)
        return NULL;
    uint32_t n = MEM32(g + G_SENDS_SendCount), arr = MEM32(g + G_SENDS_pSends);
    out->SendCount = n;
    out->pSends = n != 0 ? calloc(n, sizeof *out->pSends) : NULL;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = arr + G_SEND_SIZE * i;
        out->pSends[i].Flags = MEM32(d + G_SEND_Flags);
        out->pSends[i].pOutputVoice = voice_of(MEM32(d + G_SEND_pOutputVoice));
    }
    return out;
}

/* Host copy of the guest XAUDIO2_EFFECT_CHAIN at g, as sends_in. Every
 * pEffect must be one of our AudioReverb objects: the game has no XAPO of
 * its own (plan section 1). */
static const FAudioEffectChain *chain_in(uint32_t g, FAudioEffectChain *out)
{
    if (g == 0)
        return NULL;
    uint32_t n = MEM32(g + G_CHAIN_EffectCount), arr = MEM32(g + G_CHAIN_pEffectDescriptors);
    out->EffectCount = n;
    out->pEffectDescriptors = n != 0 ? calloc(n, sizeof *out->pEffectDescriptors) : NULL;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = arr + G_EFFECT_SIZE * i, effect = MEM32(d + G_EFFECT_pEffect);
        if (com_guest_iface(effect) != &AudioReverb_iface) {
            diag_unsupported("xaudio2", MEM32(g_esp), "effect 0x%08X", effect);
            abort();
        }
        out->pEffectDescriptors[i].pEffect = ((struct xa2_reverb *)com_guest_self(effect))->fapo;
        out->pEffectDescriptors[i].InitialState = (int32_t)MEM32(d + G_EFFECT_InitialState);
        out->pEffectDescriptors[i].OutputChannels = MEM32(d + G_EFFECT_OutputChannels);
    }
    return out;
}

/* ---- Callback shims (FAudio's mixer thread) ------------------------------ */

static void guest_cb(uint32_t obj, unsigned slot, int n, uint32_t a1, uint32_t a2)
{
    uint32_t args[3] = {obj, a1, a2};
    w32_host_thread_attach();
    guest_call_cdecl(MEM32(MEM32(obj) + 4u * slot), n, args);
}

static uint32_t ctx_out(void *ctx)
{
    return (uint32_t)(uintptr_t)ctx;
}

static void FAUDIOCALL vcb_pass_start(FAudioVoiceCallback *cb, uint32_t bytes)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_PassStart, 2, bytes, 0);
}
static void FAUDIOCALL vcb_pass_end(FAudioVoiceCallback *cb)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_PassEnd, 1, 0, 0);
}
static void FAUDIOCALL vcb_stream_end(FAudioVoiceCallback *cb)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_StreamEnd, 1, 0, 0);
}
static void FAUDIOCALL vcb_buffer_start(FAudioVoiceCallback *cb, void *ctx)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_BufferStart, 2, ctx_out(ctx), 0);
}
static void FAUDIOCALL vcb_buffer_end(FAudioVoiceCallback *cb, void *ctx)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_BufferEnd, 2, ctx_out(ctx), 0);
}
static void FAUDIOCALL vcb_loop_end(FAudioVoiceCallback *cb, void *ctx)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_LoopEnd, 2, ctx_out(ctx), 0);
}
static void FAUDIOCALL vcb_voice_error(FAudioVoiceCallback *cb, void *ctx, uint32_t error)
{
    guest_cb(((struct xa2_voice *)cb)->guest_cb, VCB_VoiceError, 3, ctx_out(ctx), error);
}

/* Engine callbacks fan out to every registered guest callback. The list is
 * copied under the lock, so the guest may register from inside a callback. */
static void engine_fan_out(FAudioEngineCallback *cb, unsigned slot, int n, uint32_t a1)
{
    struct xa2_engine *e = (struct xa2_engine *)cb;
    uint32_t cbs[XA2_ENGINE_CB_MAX];
    pthread_mutex_lock(&e->cb_lock);
    uint32_t count = e->n_cbs;
    memcpy(cbs, e->guest_cbs, sizeof cbs);
    pthread_mutex_unlock(&e->cb_lock);
    for (uint32_t i = 0; i < count; i++)
        guest_cb(cbs[i], slot, n, a1, 0);
}
static void FAUDIOCALL ecb_critical_error(FAudioEngineCallback *cb, uint32_t error)
{
    engine_fan_out(cb, ECB_CriticalError, 2, error);
}
static void FAUDIOCALL ecb_pass_end(FAudioEngineCallback *cb)
{
    engine_fan_out(cb, ECB_PassEnd, 1, 0);
}
static void FAUDIOCALL ecb_pass_start(FAudioEngineCallback *cb)
{
    engine_fan_out(cb, ECB_PassStart, 1, 0);
}

/* ---- Voice creation ------------------------------------------------------ */

static struct xa2_voice *voice_new(uint32_t guest_callback)
{
    struct xa2_voice *xv = calloc(1, sizeof *xv);
    xv->guest_cb = guest_callback;
    xv->cb.OnBufferEnd = vcb_buffer_end;
    xv->cb.OnBufferStart = vcb_buffer_start;
    xv->cb.OnLoopEnd = vcb_loop_end;
    xv->cb.OnStreamEnd = vcb_stream_end;
    xv->cb.OnVoiceError = vcb_voice_error;
    xv->cb.OnVoiceProcessingPassEnd = vcb_pass_end;
    xv->cb.OnVoiceProcessingPassStart = vcb_pass_start;
    return xv;
}

/* After FAudio created (hr == 0) or failed to create xv->v: write the guest
 * voice object, or 0, to the guest pointer pp. */
static uint32_t voice_finish(uint32_t hr, struct xa2_voice *xv, const com_interface_t *iface,
                             uint32_t pp)
{
    uint32_t obj = 0;
    if (hr == 0) {
        obj = com_guest_new(iface, xv);
        xv->guest_obj = obj;
    } else {
        free(xv);
    }
    if (pp != 0)
        MEM32(pp) = obj;
    return hr;
}

/* ---- IXAudio2 (16 methods) ----------------------------------------------- */

/* QueryInterface(REFIID riid, void **ppv): IXAudio2 or IUnknown. */
static void IXAudio2_QueryInterface(void)
{
    uint32_t self = W32_ARG(0), ppv = W32_ARG(2);
    com_guid_t iid = com_guid_read(W32_ARG(1));
    uint32_t hr = COM_E_NOINTERFACE;
    if (com_guid_eq(&iid, &IID_IXAudio2) || com_guid_eq(&iid, &IID_IUnknown)) {
        __atomic_add_fetch(&engine_self()->refs, 1, __ATOMIC_RELAXED);
        hr = COM_S_OK;
    } else {
        char s[39];
        com_guid_str(&iid, s);
        diag_unsupported("xaudio2", MEM32(g_esp), "IXAudio2::QueryInterface %s", s);
        self = 0;
    }
    if (ppv != 0)
        MEM32(ppv) = self;
    W32_RETURN(3, hr);
}

static void IXAudio2_AddRef(void)
{
    W32_RETURN(1, __atomic_add_fetch(&engine_self()->refs, 1, __ATOMIC_RELAXED));
}

/* Release at 0 releases FAudio, which stops the engine and its thread. */
static void IXAudio2_Release(void)
{
    uint32_t self = W32_ARG(0);
    struct xa2_engine *e = engine_self();
    uint32_t refs = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);
    if (refs == 0) {
        FAudio_Release(e->fa);
        com_guest_free(self);
        pthread_mutex_destroy(&e->cb_lock);
        free(e);
    }
    W32_RETURN(1, refs);
}

/* GetDeviceCount(UINT32 *pCount) */
static void IXAudio2_GetDeviceCount(void)
{
    W32_RETURN(2, FAudio_GetDeviceCount(engine_self()->fa, gptr_or_null(W32_ARG(1))));
}

/* GetDeviceDetails(UINT32 Index, XAUDIO2_DEVICE_DETAILS *pDeviceDetails) */
static void IXAudio2_GetDeviceDetails(void)
{
    W32_RETURN(3, FAudio_GetDeviceDetails(engine_self()->fa, W32_ARG(1), gptr_or_null(W32_ARG(2))));
}

/* Initialize(UINT32 Flags, XAUDIO2_PROCESSOR Processor). FAudio has no
 * processor affinity; Wine passes FAUDIO_DEFAULT_PROCESSOR the same way. */
static void IXAudio2_Initialize(void)
{
    uint32_t hr = FAudio_Initialize(engine_self()->fa, W32_ARG(1), FAUDIO_DEFAULT_PROCESSOR);
    atrace("initialize flags=%x hr=%08x", W32_ARG(1), hr);
    W32_RETURN(3, hr);
}

/* RegisterForCallbacks(IXAudio2EngineCallback *pCallback). FAudio gets one
 * host callback per engine, registered with the first guest callback. */
static void IXAudio2_RegisterForCallbacks(void)
{
    struct xa2_engine *e = engine_self();
    uint32_t cb = W32_ARG(1), hr = COM_S_OK;
    int first = 0;
    pthread_mutex_lock(&e->cb_lock);
    uint32_t i = 0;
    while (i < e->n_cbs && e->guest_cbs[i] != cb)
        i++;
    if (i == e->n_cbs) {
        if (e->n_cbs == XA2_ENGINE_CB_MAX) {
            hr = COM_E_OUTOFMEMORY;
        } else {
            first = e->n_cbs == 0;
            e->guest_cbs[e->n_cbs++] = cb;
        }
    }
    pthread_mutex_unlock(&e->cb_lock);
    if (hr != COM_S_OK)
        diag_unsupported("xaudio2", MEM32(g_esp), "IXAudio2::RegisterForCallbacks: more than %u",
                         XA2_ENGINE_CB_MAX);
    if (first)
        hr = FAudio_RegisterForCallbacks(e->fa, &e->cb);
    W32_RETURN(2, hr);
}

/* UnregisterForCallbacks(IXAudio2EngineCallback *pCallback) (void) */
static void IXAudio2_UnregisterForCallbacks(void)
{
    struct xa2_engine *e = engine_self();
    uint32_t cb = W32_ARG(1);
    int last = 0;
    pthread_mutex_lock(&e->cb_lock);
    for (uint32_t i = 0; i < e->n_cbs; i++) {
        if (e->guest_cbs[i] == cb) {
            memmove(&e->guest_cbs[i], &e->guest_cbs[i + 1], 4u * (e->n_cbs - i - 1));
            last = --e->n_cbs == 0;
            break;
        }
    }
    pthread_mutex_unlock(&e->cb_lock);
    if (last)
        FAudio_UnregisterForCallbacks(e->fa, &e->cb);
    W32_RETURN(2, 0);
}

/* CreateSourceVoice(IXAudio2SourceVoice **pp, const WAVEFORMATEX *fmt,
 *     UINT32 Flags, float MaxFrequencyRatio, IXAudio2VoiceCallback *cb,
 *     const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain).
 * FAudio copies the format (FAudio.c FAudio_CreateSourceVoice), so the guest
 * WAVEFORMATEX is passed in place. */
static void IXAudio2_CreateSourceVoice(void)
{
    struct xa2_engine *e = engine_self();
    uint32_t pp = W32_ARG(1), cb = W32_ARG(5);
    FAudioVoiceSends sends;
    FAudioEffectChain chain;
    const FAudioVoiceSends *ps = sends_in(W32_ARG(6), &sends);
    const FAudioEffectChain *pc = chain_in(W32_ARG(7), &chain);
    struct xa2_voice *xv = voice_new(cb);
    uint32_t hr = FAudio_CreateSourceVoice(e->fa, &xv->v, gptr_or_null(W32_ARG(2)), W32_ARG(3),
                                           arg_float(4), cb != 0 ? &xv->cb : NULL, ps, pc);
    if (atrace_on()) {
        const FAudioWaveFormatEx *f = gptr_or_null(W32_ARG(2));
        if (f != NULL) {
            if (hr == 0)
                xv->id = atrace_voice_new('s', f->wFormatTag, f->nChannels, f->nSamplesPerSec,
                                          f->nBlockAlign);
            atrace("src v%u hr=%08x tag=%04x ch=%u rate=%u bits=%u align=%u flags=%x maxratio=%g "
                   "cb=%08x sends=%u chain=%u", xv->id, hr, f->wFormatTag, f->nChannels,
                   f->nSamplesPerSec, f->wBitsPerSample, f->nBlockAlign, W32_ARG(3),
                   (double)arg_float(4), cb, ps != NULL ? ps->SendCount : 0,
                   pc != NULL ? pc->EffectCount : 0);
        }
    }
    if (ps != NULL)
        free(sends.pSends);
    if (pc != NULL)
        free(chain.pEffectDescriptors);
    W32_RETURN(8, voice_finish(hr, xv, &SourceVoice_iface, pp));
}

/* CreateSubmixVoice(IXAudio2SubmixVoice **pp, UINT32 InputChannels,
 *     UINT32 InputSampleRate, UINT32 Flags, UINT32 ProcessingStage,
 *     const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain) */
static void IXAudio2_CreateSubmixVoice(void)
{
    struct xa2_engine *e = engine_self();
    uint32_t pp = W32_ARG(1);
    FAudioVoiceSends sends;
    FAudioEffectChain chain;
    const FAudioVoiceSends *ps = sends_in(W32_ARG(6), &sends);
    const FAudioEffectChain *pc = chain_in(W32_ARG(7), &chain);
    struct xa2_voice *xv = voice_new(0);
    uint32_t hr = FAudio_CreateSubmixVoice(e->fa, &xv->v, W32_ARG(2), W32_ARG(3), W32_ARG(4),
                                           W32_ARG(5), ps, pc);
    if (hr == 0)
        xv->id = atrace_voice_new('x', 0, W32_ARG(2), W32_ARG(3), 0);
    atrace("submix v%u hr=%08x ch=%u rate=%u flags=%x stage=%u sends=%u chain=%u", xv->id, hr,
           W32_ARG(2), W32_ARG(3), W32_ARG(4), W32_ARG(5), ps != NULL ? ps->SendCount : 0,
           pc != NULL ? pc->EffectCount : 0);
    if (ps != NULL)
        free(sends.pSends);
    if (pc != NULL)
        free(chain.pEffectDescriptors);
    W32_RETURN(8, voice_finish(hr, xv, &SubmixVoice_iface, pp));
}

/* CreateMasteringVoice(IXAudio2MasteringVoice **pp, UINT32 InputChannels,
 *     UINT32 InputSampleRate, UINT32 Flags, UINT32 DeviceIndex,
 *     const XAUDIO2_EFFECT_CHAIN *chain) */
static void IXAudio2_CreateMasteringVoice(void)
{
    struct xa2_engine *e = engine_self();
    uint32_t pp = W32_ARG(1);
    FAudioEffectChain chain;
    const FAudioEffectChain *pc = chain_in(W32_ARG(6), &chain);
    struct xa2_voice *xv = voice_new(0);
    uint32_t hr = FAudio_CreateMasteringVoice(e->fa, &xv->v, W32_ARG(2), W32_ARG(3), W32_ARG(4),
                                              W32_ARG(5), pc);
    if (hr == 0) {
        e->master = xv->v;
        if (atrace_on()) {
            FAudioVoiceDetails d;
            FAudioVoice_GetVoiceDetails(xv->v, &d);
            xv->id = atrace_voice_new('m', 0, d.InputChannels, d.InputSampleRate, 0);
            atrace("master v%u asked ch=%u rate=%u flags=%x dev=%u -> ch=%u rate=%u", xv->id,
                   W32_ARG(2), W32_ARG(3), W32_ARG(4), W32_ARG(5), d.InputChannels,
                   d.InputSampleRate);
        }
    } else {
        atrace("master hr=%08x", hr);
    }
    if (pc != NULL)
        free(chain.pEffectDescriptors);
    W32_RETURN(7, voice_finish(hr, xv, &MasteringVoice_iface, pp));
}

static void IXAudio2_StartEngine(void)
{
    uint32_t hr = FAudio_StartEngine(engine_self()->fa);
    atrace("start engine hr=%08x", hr);
    W32_RETURN(1, hr);
}

/* StopEngine() (void) */
static void IXAudio2_StopEngine(void)
{
    FAudio_StopEngine(engine_self()->fa);
    atrace("stop engine");
    W32_RETURN(1, 0);
}

/* CommitChanges(UINT32 OperationSet) */
static void IXAudio2_CommitChanges(void)
{
    W32_RETURN(2, FAudio_CommitOperationSet(engine_self()->fa, W32_ARG(1)));
}

/* GetPerformanceData(XAUDIO2_PERFORMANCE_DATA *p) (void) */
static void IXAudio2_GetPerformanceData(void)
{
    FAudio_GetPerformanceData(engine_self()->fa, gptr_or_null(W32_ARG(1)));
    W32_RETURN(2, 0);
}

/* SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION *p, void *reserved)
 * (void) */
static void IXAudio2_SetDebugConfiguration(void)
{
    FAudio_SetDebugConfiguration(engine_self()->fa, gptr_or_null(W32_ARG(1)), NULL);
    W32_RETURN(3, 0);
}

static const recomp_func_t IXAudio2_methods[] = {
    IXAudio2_QueryInterface,
    IXAudio2_AddRef,
    IXAudio2_Release,
    IXAudio2_GetDeviceCount,
    IXAudio2_GetDeviceDetails,
    IXAudio2_Initialize,
    IXAudio2_RegisterForCallbacks,
    IXAudio2_UnregisterForCallbacks,
    IXAudio2_CreateSourceVoice,
    IXAudio2_CreateSubmixVoice,
    IXAudio2_CreateMasteringVoice,
    IXAudio2_StartEngine,
    IXAudio2_StopEngine,
    IXAudio2_CommitChanges,
    IXAudio2_GetPerformanceData,
    IXAudio2_SetDebugConfiguration,
};
static const com_interface_t IXAudio2_iface = {
    "IXAudio2", IXAudio2_methods, sizeof IXAudio2_methods / sizeof IXAudio2_methods[0]};

/* ---- IXAudio2Voice (19 methods, shared by the 3 voice tables) ------------ */

/* GetVoiceDetails(XAUDIO27_VOICE_DETAILS *p) (void) */
static void Voice_GetVoiceDetails(void)
{
    uint32_t p = W32_ARG(1);
    FAudioVoiceDetails d;
    FAudioVoice_GetVoiceDetails(voice_self()->v, &d);
    MEM32(p + G_VD_CreationFlags) = d.CreationFlags;
    MEM32(p + G_VD_InputChannels) = d.InputChannels;
    MEM32(p + G_VD_InputSampleRate) = d.InputSampleRate;
    W32_RETURN(2, 0);
}

/* SetOutputVoices(const XAUDIO2_VOICE_SENDS *p) */
static void Voice_SetOutputVoices(void)
{
    FAudioVoiceSends sends;
    const FAudioVoiceSends *ps = sends_in(W32_ARG(1), &sends);
    uint32_t hr = FAudioVoice_SetOutputVoices(voice_self()->v, ps);
    if (ps != NULL)
        free(sends.pSends);
    W32_RETURN(2, hr);
}

/* SetEffectChain(const XAUDIO2_EFFECT_CHAIN *p) */
static void Voice_SetEffectChain(void)
{
    FAudioEffectChain chain;
    const FAudioEffectChain *pc = chain_in(W32_ARG(1), &chain);
    uint32_t hr = FAudioVoice_SetEffectChain(voice_self()->v, pc);
    if (pc != NULL)
        free(chain.pEffectDescriptors);
    W32_RETURN(2, hr);
}

/* EnableEffect(UINT32 idx, UINT32 OperationSet) */
static void Voice_EnableEffect(void)
{
    W32_RETURN(3, FAudioVoice_EnableEffect(voice_self()->v, W32_ARG(1), W32_ARG(2)));
}

/* DisableEffect(UINT32 idx, UINT32 OperationSet) */
static void Voice_DisableEffect(void)
{
    W32_RETURN(3, FAudioVoice_DisableEffect(voice_self()->v, W32_ARG(1), W32_ARG(2)));
}

/* GetEffectState(UINT32 idx, BOOL *pEnabled) (void) */
static void Voice_GetEffectState(void)
{
    FAudioVoice_GetEffectState(voice_self()->v, W32_ARG(1), gptr_or_null(W32_ARG(2)));
    W32_RETURN(3, 0);
}

/* The only effect is AudioReverb 2.4, whose parameters are the 52-byte
 * XAUDIO2FX_REVERB_PARAMETERS. FAudio copies ParametersByteSize bytes either
 * way, so another size would read or write past the guest's block. */
static int reverb_size_ok(const char *method, uint32_t size)
{
    if (size == sizeof(FAudioFXReverbParameters))
        return 1;
    diag_unsupported("xaudio2", MEM32(g_esp), "IXAudio2Voice::%s size %u", method, size);
    return 0;
}

/* SetEffectParameters(UINT32 idx, const void *p, UINT32 size, UINT32 OperationSet) */
static void Voice_SetEffectParameters(void)
{
    uint32_t size = W32_ARG(3);
    if (!reverb_size_ok("SetEffectParameters", size)) {
        W32_RETURN(5, COM_E_INVALIDARG);
        return;
    }
    W32_RETURN(5, FAudioVoice_SetEffectParameters(voice_self()->v, W32_ARG(1),
                                                  gptr_or_null(W32_ARG(2)), size, W32_ARG(4)));
}

/* GetEffectParameters(UINT32 idx, void *p, UINT32 size) */
static void Voice_GetEffectParameters(void)
{
    uint32_t size = W32_ARG(3);
    if (!reverb_size_ok("GetEffectParameters", size)) {
        W32_RETURN(4, COM_E_INVALIDARG);
        return;
    }
    W32_RETURN(4, FAudioVoice_GetEffectParameters(voice_self()->v, W32_ARG(1),
                                                  gptr_or_null(W32_ARG(2)), size));
}

/* SetFilterParameters(const XAUDIO2_FILTER_PARAMETERS *p, UINT32 OperationSet) */
static void Voice_SetFilterParameters(void)
{
    W32_RETURN(3, FAudioVoice_SetFilterParameters(voice_self()->v, gptr_or_null(W32_ARG(1)),
                                                  W32_ARG(2)));
}

/* GetFilterParameters(XAUDIO2_FILTER_PARAMETERS *p) (void) */
static void Voice_GetFilterParameters(void)
{
    FAudioVoice_GetFilterParameters(voice_self()->v, gptr_or_null(W32_ARG(1)));
    W32_RETURN(2, 0);
}

/* SetOutputFilterParameters(IXAudio2Voice *dst, const XAUDIO2_FILTER_PARAMETERS *p,
 *                           UINT32 OperationSet) */
static void Voice_SetOutputFilterParameters(void)
{
    W32_RETURN(4, FAudioVoice_SetOutputFilterParameters(voice_self()->v, voice_of(W32_ARG(1)),
                                                        gptr_or_null(W32_ARG(2)), W32_ARG(3)));
}

/* GetOutputFilterParameters(IXAudio2Voice *dst, XAUDIO2_FILTER_PARAMETERS *p) (void) */
static void Voice_GetOutputFilterParameters(void)
{
    FAudioVoice_GetOutputFilterParameters(voice_self()->v, voice_of(W32_ARG(1)),
                                          gptr_or_null(W32_ARG(2)));
    W32_RETURN(3, 0);
}

/* SetVolume(float v, UINT32 OperationSet) */
static void Voice_SetVolume(void)
{
    struct xa2_voice *xv = voice_self();
    if (xv->id != 0 && g_vstat[xv->id].vol_bits != W32_ARG(1)) {
        g_vstat[xv->id].vol_bits = W32_ARG(1);
        atrace("vol v%u %g", xv->id, (double)arg_float(1));
    }
    W32_RETURN(3, FAudioVoice_SetVolume(xv->v, arg_float(1), W32_ARG(2)));
}

/* GetVolume(float *p) (void) */
static void Voice_GetVolume(void)
{
    FAudioVoice_GetVolume(voice_self()->v, gptr_or_null(W32_ARG(1)));
    W32_RETURN(2, 0);
}

/* SetChannelVolumes(UINT32 ch, const float *p, UINT32 OperationSet) */
static void Voice_SetChannelVolumes(void)
{
    W32_RETURN(4, FAudioVoice_SetChannelVolumes(voice_self()->v, W32_ARG(1),
                                                gptr_or_null(W32_ARG(2)), W32_ARG(3)));
}

/* GetChannelVolumes(UINT32 ch, float *p) (void) */
static void Voice_GetChannelVolumes(void)
{
    FAudioVoice_GetChannelVolumes(voice_self()->v, W32_ARG(1), gptr_or_null(W32_ARG(2)));
    W32_RETURN(3, 0);
}

/* SetOutputMatrix(IXAudio2Voice *dst, UINT32 src_ch, UINT32 dst_ch,
 *                 const float *p, UINT32 OperationSet) */
static void Voice_SetOutputMatrix(void)
{
    struct xa2_voice *xv = voice_self();
    const float *m = gptr_or_null(W32_ARG(4));
    if (xv->id != 0 && m != NULL && W32_ARG(2) * W32_ARG(3) >= 4)
        atrace("matrix v%u -> %08x %ux%u [%g %g %g %g]", xv->id, W32_ARG(1), W32_ARG(2), W32_ARG(3),
               (double)m[0], (double)m[1], (double)m[2], (double)m[3]);
    else if (xv->id != 0 && m != NULL)
        atrace("matrix v%u -> %08x %ux%u [%g ...]", xv->id, W32_ARG(1), W32_ARG(2), W32_ARG(3),
               (double)m[0]);
    W32_RETURN(6, FAudioVoice_SetOutputMatrix(xv->v, voice_of(W32_ARG(1)), W32_ARG(2), W32_ARG(3),
                                              m, W32_ARG(5)));
}

/* GetOutputMatrix(IXAudio2Voice *dst, UINT32 src_ch, UINT32 dst_ch, float *p) (void) */
static void Voice_GetOutputMatrix(void)
{
    FAudioVoice_GetOutputMatrix(voice_self()->v, voice_of(W32_ARG(1)), W32_ARG(2), W32_ARG(3),
                                gptr_or_null(W32_ARG(4)));
    W32_RETURN(5, 0);
}

/* DestroyVoice() (void). FAudio waits for a mixing pass on this voice to end,
 * so no callback runs after it returns. */
static void Voice_DestroyVoice(void)
{
    uint32_t self = W32_ARG(0);
    struct xa2_voice *xv = voice_self();
    if (xv->id != 0)
        atrace("destroy v%u submits=%llu bytes=%llu", xv->id,
               (unsigned long long)g_vstat[xv->id].submits, (unsigned long long)g_vstat[xv->id].bytes);
    FAudioVoice_DestroyVoice(xv->v);
    com_guest_free(self);
    free(xv);
    W32_RETURN(1, 0);
}

#define VOICE_METHODS                                                                     \
    Voice_GetVoiceDetails, Voice_SetOutputVoices, Voice_SetEffectChain, Voice_EnableEffect, \
        Voice_DisableEffect, Voice_GetEffectState, Voice_SetEffectParameters,              \
        Voice_GetEffectParameters, Voice_SetFilterParameters, Voice_GetFilterParameters,   \
        Voice_SetOutputFilterParameters, Voice_GetOutputFilterParameters, Voice_SetVolume, \
        Voice_GetVolume, Voice_SetChannelVolumes, Voice_GetChannelVolumes,                 \
        Voice_SetOutputMatrix, Voice_GetOutputMatrix, Voice_DestroyVoice

/* ---- IXAudio2SourceVoice (slots 19-28) ----------------------------------- */

/* Start(UINT32 Flags, UINT32 OperationSet) */
static void SourceVoice_Start(void)
{
    struct xa2_voice *xv = voice_self();
    uint32_t hr = FAudioSourceVoice_Start(xv->v, W32_ARG(1), W32_ARG(2));
    if (xv->id != 0) {
        __atomic_add_fetch(&g_starts, 1, __ATOMIC_RELAXED);
        atrace("start v%u flags=%x op=%u hr=%08x", xv->id, W32_ARG(1), W32_ARG(2), hr);
    }
    W32_RETURN(3, hr);
}

/* Stop(UINT32 Flags, UINT32 OperationSet) */
static void SourceVoice_Stop(void)
{
    struct xa2_voice *xv = voice_self();
    uint32_t hr = FAudioSourceVoice_Stop(xv->v, W32_ARG(1), W32_ARG(2));
    if (xv->id != 0) {
        __atomic_add_fetch(&g_stops, 1, __ATOMIC_RELAXED);
        atrace("stop v%u flags=%x op=%u hr=%08x", xv->id, W32_ARG(1), W32_ARG(2), hr);
    }
    W32_RETURN(3, hr);
}

/* SubmitSourceBuffer(const XAUDIO2_BUFFER *p, const XAUDIO2_BUFFER_WMA *wma).
 * FAudio copies the FAudioBuffer; the audio data stays in guest memory until
 * OnBufferEnd, as XAudio2 requires of the caller (plan section 5). */
static void SourceVoice_SubmitSourceBuffer(void)
{
    uint32_t g = W32_ARG(1), gw = W32_ARG(2);
    FAudioBuffer b = {
        .Flags = MEM32(g + G_BUF_Flags),
        .AudioBytes = MEM32(g + G_BUF_AudioBytes),
        .pAudioData = gptr_or_null(MEM32(g + G_BUF_pAudioData)),
        .PlayBegin = MEM32(g + G_BUF_PlayBegin),
        .PlayLength = MEM32(g + G_BUF_PlayLength),
        .LoopBegin = MEM32(g + G_BUF_LoopBegin),
        .LoopLength = MEM32(g + G_BUF_LoopLength),
        .LoopCount = MEM32(g + G_BUF_LoopCount),
        .pContext = (void *)(uintptr_t)MEM32(g + G_BUF_pContext),
    };
    FAudioBufferWMA w, *pw = NULL;
    if (gw != 0) {
        w.pDecodedPacketCumulativeBytes =
            gptr_or_null(MEM32(gw + G_WMA_pDecodedPacketCumulativeBytes));
        w.PacketCount = MEM32(gw + G_WMA_PacketCount);
        pw = &w;
    }
    struct xa2_voice *xv = voice_self();
    uint32_t hr = FAudioSourceVoice_SubmitSourceBuffer(xv->v, &b, pw);
    if (xv->id != 0) {
        struct atrace_voice *v = &g_vstat[xv->id];
        uint64_t k = __atomic_add_fetch(&v->submits, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&v->bytes, b.AudioBytes, __ATOMIC_RELAXED);
        if (k <= 3 || hr != 0)
            atrace("submit v%u #%llu bytes=%u flags=%x play=%u+%u loop=%u+%u x%u hr=%08x", xv->id,
                   (unsigned long long)k, b.AudioBytes, b.Flags, b.PlayBegin, b.PlayLength,
                   b.LoopBegin, b.LoopLength, b.LoopCount, hr);
    }
    W32_RETURN(3, hr);
}

static void SourceVoice_FlushSourceBuffers(void)
{
    W32_RETURN(1, FAudioSourceVoice_FlushSourceBuffers(voice_self()->v));
}

static void SourceVoice_Discontinuity(void)
{
    W32_RETURN(1, FAudioSourceVoice_Discontinuity(voice_self()->v));
}

/* ExitLoop(UINT32 OperationSet) */
static void SourceVoice_ExitLoop(void)
{
    W32_RETURN(2, FAudioSourceVoice_ExitLoop(voice_self()->v, W32_ARG(1)));
}

/* GetState(XAUDIO2_VOICE_STATE *p) (void; 2.4 has no Flags argument) */
static void SourceVoice_GetState(void)
{
    uint32_t p = W32_ARG(1);
    FAudioVoiceState st;
    FAudioSourceVoice_GetState(voice_self()->v, &st, 0);
    MEM32(p + G_VS_pCurrentBufferContext) = ctx_out(st.pCurrentBufferContext);
    MEM32(p + G_VS_BuffersQueued) = st.BuffersQueued;
    memcpy(guest_ptr(p + G_VS_SamplesPlayed), &st.SamplesPlayed, 8);
    W32_RETURN(2, 0);
}

/* SetFrequencyRatio(float r, UINT32 OperationSet) */
static void SourceVoice_SetFrequencyRatio(void)
{
    struct xa2_voice *xv = voice_self();
    if (xv->id != 0 && g_vstat[xv->id].ratio_bits != W32_ARG(1)) {
        g_vstat[xv->id].ratio_bits = W32_ARG(1);
        atrace("ratio v%u %g", xv->id, (double)arg_float(1));
    }
    W32_RETURN(3, FAudioSourceVoice_SetFrequencyRatio(xv->v, arg_float(1), W32_ARG(2)));
}

/* GetFrequencyRatio(float *p) (void) */
static void SourceVoice_GetFrequencyRatio(void)
{
    FAudioSourceVoice_GetFrequencyRatio(voice_self()->v, gptr_or_null(W32_ARG(1)));
    W32_RETURN(2, 0);
}

/* SetSourceSampleRate(UINT32 rate) */
static void SourceVoice_SetSourceSampleRate(void)
{
    struct xa2_voice *xv = voice_self();
    atrace("srcrate v%u %u", xv->id, W32_ARG(1));
    W32_RETURN(2, FAudioSourceVoice_SetSourceSampleRate(xv->v, W32_ARG(1)));
}

static const recomp_func_t SourceVoice_methods[] = {
    VOICE_METHODS,
    SourceVoice_Start,
    SourceVoice_Stop,
    SourceVoice_SubmitSourceBuffer,
    SourceVoice_FlushSourceBuffers,
    SourceVoice_Discontinuity,
    SourceVoice_ExitLoop,
    SourceVoice_GetState,
    SourceVoice_SetFrequencyRatio,
    SourceVoice_GetFrequencyRatio,
    SourceVoice_SetSourceSampleRate,
};
static const com_interface_t SourceVoice_iface = {
    "IXAudio2SourceVoice", SourceVoice_methods,
    sizeof SourceVoice_methods / sizeof SourceVoice_methods[0]};

static const recomp_func_t SubmixVoice_methods[] = {VOICE_METHODS};
static const com_interface_t SubmixVoice_iface = {
    "IXAudio2SubmixVoice", SubmixVoice_methods,
    sizeof SubmixVoice_methods / sizeof SubmixVoice_methods[0]};

static const recomp_func_t MasteringVoice_methods[] = {VOICE_METHODS};
static const com_interface_t MasteringVoice_iface = {
    "IXAudio2MasteringVoice", MasteringVoice_methods,
    sizeof MasteringVoice_methods / sizeof MasteringVoice_methods[0]};

_Static_assert(sizeof SourceVoice_methods / sizeof SourceVoice_methods[0] == 29,
               "IXAudio2SourceVoice has 29 methods");
_Static_assert(sizeof SubmixVoice_methods / sizeof SubmixVoice_methods[0] == 19,
               "IXAudio2Voice has 19 methods");
_Static_assert(sizeof IXAudio2_methods / sizeof IXAudio2_methods[0] == 16,
               "IXAudio2 has 16 methods");

/* ---- AudioReverb (IUnknown) ---------------------------------------------- */

static void AudioReverb_QueryInterface(void)
{
    uint32_t self = W32_ARG(0), ppv = W32_ARG(2);
    com_guid_t iid = com_guid_read(W32_ARG(1));
    uint32_t hr = COM_E_NOINTERFACE;
    if (com_guid_eq(&iid, &IID_IUnknown)) {
        __atomic_add_fetch(&((struct xa2_reverb *)com_guest_self(self))->refs, 1, __ATOMIC_RELAXED);
        hr = COM_S_OK;
    } else {
        char s[39];
        com_guid_str(&iid, s);
        diag_unsupported("xaudio2", MEM32(g_esp), "AudioReverb::QueryInterface %s", s);
        self = 0;
    }
    if (ppv != 0)
        MEM32(ppv) = self;
    W32_RETURN(3, hr);
}

static void AudioReverb_AddRef(void)
{
    struct xa2_reverb *r = com_guest_self(W32_ARG(0));
    W32_RETURN(1, __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED));
}

/* Release at 0 drops our FAPO reference; a voice whose chain holds the
 * effect keeps its own (FAudio AddRefs every effect in a chain). */
static void AudioReverb_Release(void)
{
    uint32_t self = W32_ARG(0);
    struct xa2_reverb *r = com_guest_self(self);
    uint32_t refs = __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
    if (refs == 0) {
        r->fapo->Release(r->fapo);
        com_guest_free(self);
        free(r);
    }
    W32_RETURN(1, refs);
}

static const recomp_func_t AudioReverb_methods[] = {
    AudioReverb_QueryInterface,
    AudioReverb_AddRef,
    AudioReverb_Release,
};
static const com_interface_t AudioReverb_iface = {
    "AudioReverb", AudioReverb_methods, sizeof AudioReverb_methods / sizeof AudioReverb_methods[0]};

/* ---- Class factories ----------------------------------------------------- */

/* CLSID_XAudio2 and CLSID_XAudio2_Debug (2.4). FAudio applies 2.4 behaviour
 * when constructed with version 4, as Wine's xaudio2_4 does. */
static uint32_t xaudio2_factory(const com_guid_t *iid, uint32_t *out)
{
    *out = 0;
    if (!com_guid_eq(iid, &IID_IXAudio2) && !com_guid_eq(iid, &IID_IUnknown))
        return COM_E_NOINTERFACE;
    struct xa2_engine *e = calloc(1, sizeof *e);
    uint32_t hr = FAudioCOMConstructEXT(&e->fa, 4);
    if (hr != 0) {
        free(e);
        return hr;
    }
    e->refs = 1;
    pthread_mutex_init(&e->cb_lock, NULL);
    e->cb.OnCriticalError = ecb_critical_error;
    e->cb.OnProcessingPassEnd = ecb_pass_end;
    e->cb.OnProcessingPassStart = ecb_pass_start;
    if (atrace_on())
        FAudio_SetEngineProcedureEXT(e->fa, atrace_engine_proc, e);
    *out = com_guest_new(&IXAudio2_iface, e);
    return COM_S_OK;
}

/* CLSID_AudioReverb and CLSID_AudioReverb_Debug (2.4): FAudio's pre-2.9
 * reverb, as Wine uses below 2.9. */
static uint32_t reverb_factory(const com_guid_t *iid, uint32_t *out)
{
    *out = 0;
    if (!com_guid_eq(iid, &IID_IUnknown))
        return COM_E_NOINTERFACE;
    struct xa2_reverb *r = calloc(1, sizeof *r);
    uint32_t hr = FAudioCreateReverb(&r->fapo, 0);
    if (hr != 0) {
        free(r);
        return hr;
    }
    r->refs = 1;
    *out = com_guest_new(&AudioReverb_iface, r);
    return COM_S_OK;
}

__attribute__((constructor)) static void xaudio2_register(void)
{
    com_register_class(&CLSID_XAudio2_24, "XAudio2 2.4", xaudio2_factory);
    com_register_class(&CLSID_XAudio2_Debug_24, "XAudio2 2.4 debug", xaudio2_factory);
    com_register_class(&CLSID_AudioReverb_24, "AudioReverb 2.4", reverb_factory);
    com_register_class(&CLSID_AudioReverb_Debug_24, "AudioReverb 2.4 debug", reverb_factory);
}
