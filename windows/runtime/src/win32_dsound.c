/* Win32 audio slice: DirectSound 8 (DSOUND.dll!DirectSoundCreate8) on the
 * host device FAudio opens for the XAudio2 slice. Evidence for every method
 * the game calls (vtable offset, call site, flags, formats): notes/dsound.md.
 * Contract: win32_thunk.h, com_guest.h, docs/WIN32_THUNKS.md.
 *
 * Interfaces, in dsound.h method order: IDirectSound8 (12 methods),
 * IDirectSoundBuffer8 (24; the same vtable serves IDirectSoundBuffer, as in
 * dsound.dll), IDirectSound3DBuffer (21), IDirectSound3DListener (18). Every
 * method is __stdcall with `this` at W32_ARG(0) and pops 4 + 4 * (1 + n)
 * bytes. Methods the game never calls return DSERR_UNSUPPORTED (E_NOTIMPL)
 * and log their name once.
 *
 * Mixing: one FAudio source voice (float32 stereo at the device rate) whose
 * pass-start callback pulls ds_render(), on FAudio's mixer thread. ds_render
 * sums every playing secondary buffer: 8- or 16-bit PCM, mono or stereo,
 * linear resampling from the buffer frequency to the device rate, volume and
 * pan in hundredths of a decibel as dsound.h defines them, and a simple
 * software 3D model (inverse-distance rolloff, pan from the source azimuth;
 * no HRTF, cones or Doppler). One mutex per device guards all buffer state;
 * the guest writes audio data through Lock pointers without it, as on
 * Windows. A buffer's IDirectSoundBuffer8 and IDirectSound3DBuffer objects
 * (or the primary's listener) share one reference count. */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FAudio.h>

#include "com_guest.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "win32_dsound.h"
#include "win32_thunk.h"

/* ---- dsound.h constants -------------------------------------------------- */

#define DS_OK 0x00000000u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_NOAGGREGATION 0x80040110u
#define DSERR_INVALIDPARAM COM_E_INVALIDARG
#define DSERR_UNSUPPORTED COM_E_NOTIMPL
#define DSERR_OUTOFMEMORY COM_E_OUTOFMEMORY

#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBCAPS_CTRL3D 0x00000010u
#define DSBCAPS_CTRLFREQUENCY 0x00000020u
#define DSBCAPS_CTRLPAN 0x00000040u
#define DSBCAPS_CTRLVOLUME 0x00000080u

#define DSBPLAY_LOOPING 0x00000001u
#define DSBSTATUS_PLAYING 0x00000001u
#define DSBSTATUS_LOOPING 0x00000004u
#define DSBLOCK_FROMWRITECURSOR 0x00000001u
#define DSBLOCK_ENTIREBUFFER 0x00000002u

#define DSBVOLUME_MIN (-10000)
#define DSBVOLUME_MAX 0
#define DSBPAN_LEFT (-10000)
#define DSBPAN_RIGHT 10000
#define DSBFREQUENCY_ORIGINAL 0u
#define DSBFREQUENCY_MIN 100u
#define DSBFREQUENCY_MAX 200000u
#define DSBSIZE_MIN 4u
#define DSBSIZE_MAX 0x0FFFFFFFu

#define DSSCL_NORMAL 1u
#define DSSCL_WRITEPRIMARY 4u
#define DSSPEAKER_STEREO 4u
#define DSSPEAKER_GEOMETRY_WIDE 20u

#define DS3D_IMMEDIATE 0u
#define DS3D_DEFERRED 1u
#define DS3DMODE_NORMAL 0u
#define DS3DMODE_HEADRELATIVE 1u
#define DS3DMODE_DISABLE 2u
#define DS3D_MAXROLLOFFFACTOR 10.0f

#define WAVE_FORMAT_PCM 1u

/* Guest DSBUFFERDESC (36 bytes; DirectX 7 DSBUFFERDESC1 is the first 20). */
#define G_DSBD_dwSize 0u
#define G_DSBD_dwFlags 4u
#define G_DSBD_dwBufferBytes 8u
#define G_DSBD_lpwfxFormat 16u

/* Guest WAVEFORMATEX (18 bytes, pack 1 in mmreg.h but naturally aligned). */
#define G_WFX_wFormatTag 0u
#define G_WFX_nChannels 2u
#define G_WFX_nSamplesPerSec 4u
#define G_WFX_nBlockAlign 12u
#define G_WFX_wBitsPerSample 14u

/* Guest DSCAPS: 24 DWORDs. */
#define G_DSCAPS_SIZE 96u

static const com_guid_t IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t IID_IDirectSound = {
    0x279afa83, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t IID_IDirectSound8 = {
    0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66}};
static const com_guid_t IID_IDirectSoundBuffer = {
    0x279afa85, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t IID_IDirectSoundBuffer8 = {
    0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
static const com_guid_t IID_IDirectSound3DListener = {
    0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t IID_IDirectSound3DBuffer = {
    0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const com_guid_t DSDEVID_DefaultPlayback = {
    0xdef00000, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
static const com_guid_t DSDEVID_DefaultVoicePlayback = {
    0xdef00002, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};

/* ---- Host objects -------------------------------------------------------- */

#define DS_CHUNK_FRAMES 2048u /* largest FAudio request seen is one pass */
#define DS_CHUNKS 8u

struct ds_3d {
    float pos[3], vel[3];
    float min_dist, max_dist;
    uint32_t mode;
};

struct ds_listener {
    float pos[3], vel[3], front[3], top[3];
    float rolloff;
};

/* Audio memory in guest memory, shared by a buffer and its duplicates. */
struct ds_data {
    uint32_t guest, bytes, refs;
};

struct ds_device;

struct ds_buffer {
    struct ds_device *dev;
    struct ds_buffer *next;
    uint32_t refs;
    uint32_t guest, guest_3d; /* guest_3d: 3D buffer, or listener (primary) */
    uint32_t flags;
    struct ds_data *data; /* NULL for the primary buffer */
    uint32_t channels, bits, align, nframes, format_rate;
    uint32_t freq;
    int32_t volume, pan;
    int playing, looping;
    double pos; /* play position in frames */
    struct ds_3d cur, pend;
};

struct ds_voice_cb {
    FAudioVoiceCallback cb; /* first member: the shims cast back */
    struct ds_device *dev;
};

struct ds_device {
    pthread_mutex_t lock;
    uint32_t refs, guest, coop;
    uint32_t rate; /* output frames per second */
    struct ds_buffer *buffers; /* every live buffer, the primary included */
    struct ds_buffer *primary;
    struct ds_listener lis_cur, lis_pend;
    FAudio *fa;
    FAudioMasteringVoice *master;
    FAudioSourceVoice *src;
    struct ds_voice_cb vcb;
    float *chunks;
    uint32_t chunk_next, queued; /* queued: atomic, FAudio thread only */
};

static const com_interface_t DSound_iface, Buffer_iface, Buffer3D_iface, Listener_iface;
static uint32_t g_offline_rate;

static float arg_float(unsigned i)
{
    uint32_t bits = W32_ARG(i);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void log_once(int *done, const char *name)
{
    if (!__atomic_exchange_n(done, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "UNSUPPORTED dsound: %s\n", name);
}

/* A method the game does not call: E_NOTIMPL, popping this + nargs. */
#define DS_STUB(fn, label, nargs)              \
    static void fn(void)                       \
    {                                          \
        static int done;                       \
        log_once(&done, label);                \
        W32_RETURN(1 + (nargs), DSERR_UNSUPPORTED); \
    }

/* ---- Mixer ---------------------------------------------------------------- */

static float ds_db_gain(int32_t hundredths)
{
    return powf(10.0f, (float)hundredths / 2000.0f);
}

static float sample_at(const struct ds_buffer *b, const uint8_t *base, uint32_t frame,
                       uint32_t ch)
{
    const uint8_t *p = base + (size_t)frame * b->align + ch * (b->bits / 8u);
    if (b->bits == 8)
        return ((float)p[0] - 128.0f) / 128.0f;
    int16_t s;
    memcpy(&s, p, sizeof s);
    return (float)s / 32768.0f;
}

static void cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Left and right gains of b: volume, then 3D (CTRL3D, mode not DISABLE) or
 * SetPan. DirectSound pan attenuates the opposite channel by |pan|
 * hundredths of a decibel. */
static void buffer_gains(const struct ds_buffer *b, const struct ds_listener *l, float *gl,
                         float *gr)
{
    float g = ds_db_gain(b->volume);
    int32_t pan = b->pan;
    if ((b->flags & DSBCAPS_CTRL3D) && b->cur.mode != DS3DMODE_DISABLE) {
        float rel[3] = {b->cur.pos[0], b->cur.pos[1], b->cur.pos[2]};
        float lateral = rel[0];
        if (b->cur.mode == DS3DMODE_NORMAL) {
            float right[3];
            for (int i = 0; i < 3; i++)
                rel[i] -= l->pos[i];
            cross(l->top, l->front, right); /* left-handed: x right, y up, z front */
            float n = sqrtf(dot(right, right));
            lateral = n > 0.0f ? dot(rel, right) / n : 0.0f;
        }
        float d = sqrtf(dot(rel, rel));
        float dd = d < b->cur.max_dist ? d : b->cur.max_dist;
        if (dd > b->cur.min_dist)
            g *= b->cur.min_dist / (b->cur.min_dist + l->rolloff * (dd - b->cur.min_dist));
        pan = d > 0.0f ? (int32_t)lrintf(10000.0f * lateral / d) : 0;
    }
    *gl = pan > 0 ? g * ds_db_gain(-pan) : g;
    *gr = pan < 0 ? g * ds_db_gain(pan) : g;
}

/* Add `frames` frames of b to out and advance it. Caller holds dev->lock. */
static void mix_buffer(struct ds_buffer *b, const struct ds_listener *l, float *out,
                       uint32_t frames, uint32_t rate)
{
    const uint8_t *base = guest_ptr(b->data->guest);
    float gl, gr;
    buffer_gains(b, l, &gl, &gr);
    double step = (double)b->freq / (double)rate;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t f0 = (uint32_t)b->pos;
        float t = (float)(b->pos - (double)f0);
        uint32_t f1 = f0 + 1u < b->nframes ? f0 + 1u : (b->looping ? 0u : f0);
        float s[2];
        for (uint32_t c = 0; c < b->channels; c++) {
            float a = sample_at(b, base, f0, c);
            s[c] = a + (sample_at(b, base, f1, c) - a) * t;
        }
        if (b->channels == 1)
            s[1] = s[0];
        out[2 * i] += gl * s[0];
        out[2 * i + 1] += gr * s[1];
        b->pos += step;
        if (b->pos >= (double)b->nframes) {
            if (!b->looping) {
                b->playing = 0;
                b->pos = 0.0;
                return;
            }
            b->pos = fmod(b->pos, (double)b->nframes);
        }
    }
}

static void ds_render(struct ds_device *dev, float *out, uint32_t frames)
{
    memset(out, 0, sizeof(float) * 2u * frames);
    pthread_mutex_lock(&dev->lock);
    for (struct ds_buffer *b = dev->buffers; b != NULL; b = b->next)
        if (b->playing && b->data != NULL)
            mix_buffer(b, &dev->lis_cur, out, frames, dev->rate);
    pthread_mutex_unlock(&dev->lock);
    for (uint32_t i = 0; i < 2u * frames; i++)
        out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
}

void w32_dsound_set_offline_rate(uint32_t rate)
{
    g_offline_rate = rate;
}

void w32_dsound_render(uint32_t guest_ds, float *out, uint32_t frames)
{
    ds_render(com_guest_self(guest_ds), out, frames);
}

/* FAudio's mixer thread: fill what the pass needs, one chunk per submit. */
static void FAUDIOCALL on_pass_start(FAudioVoiceCallback *cb, uint32_t bytes)
{
    struct ds_device *dev = ((struct ds_voice_cb *)cb)->dev;
    uint32_t frames = bytes / 8u;
    while (frames > 0 && __atomic_load_n(&dev->queued, __ATOMIC_ACQUIRE) < DS_CHUNKS) {
        uint32_t n = frames < DS_CHUNK_FRAMES ? frames : DS_CHUNK_FRAMES;
        float *p = dev->chunks + (size_t)dev->chunk_next * DS_CHUNK_FRAMES * 2u;
        ds_render(dev, p, n);
        FAudioBuffer fb = {0};
        fb.AudioBytes = n * 8u;
        fb.pAudioData = (const uint8_t *)p;
        __atomic_add_fetch(&dev->queued, 1u, __ATOMIC_ACQ_REL);
        if (FAudioSourceVoice_SubmitSourceBuffer(dev->src, &fb, NULL) != 0) {
            __atomic_sub_fetch(&dev->queued, 1u, __ATOMIC_ACQ_REL);
            return;
        }
        dev->chunk_next = (dev->chunk_next + 1u) % DS_CHUNKS;
        frames -= n;
    }
}

static void FAUDIOCALL on_buffer_end(FAudioVoiceCallback *cb, void *ctx)
{
    (void)ctx;
    __atomic_sub_fetch(&((struct ds_voice_cb *)cb)->dev->queued, 1u, __ATOMIC_ACQ_REL);
}

/* Open the host device: engine, mastering voice at the device's own rate,
 * one float stereo source voice at that rate. 0 or an FAudio error. */
static uint32_t device_open(struct ds_device *dev)
{
    uint32_t hr = FAudioCreate(&dev->fa, 0, FAUDIO_DEFAULT_PROCESSOR);
    if (hr != 0)
        return hr;
    hr = FAudio_CreateMasteringVoice(dev->fa, &dev->master, 2, FAUDIO_DEFAULT_SAMPLERATE, 0, 0,
                                     NULL);
    if (hr != 0)
        return hr;
    FAudioVoiceDetails det;
    FAudioVoice_GetVoiceDetails(dev->master, &det);
    dev->rate = det.InputSampleRate;
    dev->chunks = calloc((size_t)DS_CHUNKS * DS_CHUNK_FRAMES * 2u, sizeof(float));
    if (dev->chunks == NULL)
        return DSERR_OUTOFMEMORY;
    FAudioWaveFormatEx fmt = {0};
    fmt.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = dev->rate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 8;
    fmt.nAvgBytesPerSec = dev->rate * 8u;
    dev->vcb.dev = dev;
    dev->vcb.cb.OnVoiceProcessingPassStart = on_pass_start;
    dev->vcb.cb.OnBufferEnd = on_buffer_end;
    hr = FAudio_CreateSourceVoice(dev->fa, &dev->src, &fmt, 0, 1.0f, &dev->vcb.cb, NULL, NULL);
    if (hr != 0)
        return hr;
    return FAudioSourceVoice_Start(dev->src, 0, 0);
}

/* Stop the mixer thread first, then free its state. */
static void device_close(struct ds_device *dev)
{
    if (dev->src != NULL)
        FAudioVoice_DestroyVoice(dev->src);
    if (dev->master != NULL)
        FAudioVoice_DestroyVoice(dev->master);
    if (dev->fa != NULL) {
        FAudio_StopEngine(dev->fa);
        FAudio_Release(dev->fa);
    }
    free(dev->chunks);
}

/* ---- Buffers: lifetime ----------------------------------------------------- */

static struct ds_device *dev_self(void)
{
    return com_guest_self(W32_ARG(0));
}

static struct ds_buffer *buf_self(void)
{
    return com_guest_self(W32_ARG(0));
}

static const struct ds_3d k_3d_default = {{0, 0, 0}, {0, 0, 0}, 1.0f, 1000000000.0f, 0};

/* Caller holds dev->lock. */
static struct ds_buffer *buffer_new(struct ds_device *dev, uint32_t flags)
{
    struct ds_buffer *b = calloc(1, sizeof *b);
    if (b == NULL)
        return NULL;
    b->dev = dev;
    b->refs = 1;
    b->flags = flags;
    b->cur = b->pend = k_3d_default;
    b->guest = com_guest_new(&Buffer_iface, b);
    b->next = dev->buffers;
    dev->buffers = b;
    return b;
}

/* Caller holds dev->lock. */
static void buffer_free(struct ds_buffer *b)
{
    struct ds_device *dev = b->dev;
    for (struct ds_buffer **pp = &dev->buffers; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    if (dev->primary == b)
        dev->primary = NULL;
    if (b->data != NULL && --b->data->refs == 0) {
        guest_free(b->data->guest);
        free(b->data);
    }
    com_guest_free(b->guest);
    if (b->guest_3d != 0)
        com_guest_free(b->guest_3d);
    free(b);
}

/* ---- IDirectSound8 ---------------------------------------------------------- */

static void DSound_QueryInterface(void)
{
    struct ds_device *dev = dev_self();
    uint32_t ppv = W32_ARG(2);
    if (ppv == 0) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    com_guid_t iid = com_guid_read(W32_ARG(1));
    if (!com_guid_eq(&iid, &IID_IUnknown) && !com_guid_eq(&iid, &IID_IDirectSound) &&
        !com_guid_eq(&iid, &IID_IDirectSound8)) {
        MEM32(ppv) = 0;
        W32_RETURN(3, COM_E_NOINTERFACE);
        return;
    }
    pthread_mutex_lock(&dev->lock);
    dev->refs++;
    pthread_mutex_unlock(&dev->lock);
    MEM32(ppv) = W32_ARG(0);
    W32_RETURN(3, DS_OK);
}

static void DSound_AddRef(void)
{
    struct ds_device *dev = dev_self();
    pthread_mutex_lock(&dev->lock);
    uint32_t n = ++dev->refs;
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(1, n);
}

/* At 0 the device and every buffer still alive go, as dsound.dll does. */
static void DSound_Release(void)
{
    struct ds_device *dev = dev_self();
    pthread_mutex_lock(&dev->lock);
    uint32_t n = --dev->refs;
    pthread_mutex_unlock(&dev->lock);
    if (n == 0) {
        device_close(dev);
        while (dev->buffers != NULL)
            buffer_free(dev->buffers);
        com_guest_free(dev->guest);
        pthread_mutex_destroy(&dev->lock);
        free(dev);
    }
    W32_RETURN(1, n);
}

/* Validate a guest WAVEFORMATEX: PCM, 1 or 2 channels, 8 or 16 bits. */
static uint32_t format_in(uint32_t wfx, struct ds_buffer *b)
{
    uint32_t tag = MEM16(wfx + G_WFX_wFormatTag), ch = MEM16(wfx + G_WFX_nChannels);
    uint32_t rate = MEM32(wfx + G_WFX_nSamplesPerSec), bits = MEM16(wfx + G_WFX_wBitsPerSample);
    uint32_t align = MEM16(wfx + G_WFX_nBlockAlign);
    if (tag != WAVE_FORMAT_PCM || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) ||
        align != ch * bits / 8u)
        return DSERR_BADFORMAT;
    if (rate < DSBFREQUENCY_MIN || rate > DSBFREQUENCY_MAX)
        return DSERR_INVALIDPARAM;
    b->channels = ch;
    b->bits = bits;
    b->align = align;
    b->format_rate = b->freq = rate;
    return DS_OK;
}

/* CreateSoundBuffer(LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN) */
static void DSound_CreateSoundBuffer(void)
{
    struct ds_device *dev = dev_self();
    uint32_t desc = W32_ARG(1), pp = W32_ARG(2);
    if (desc == 0 || pp == 0) {
        W32_RETURN(4, DSERR_INVALIDPARAM);
        return;
    }
    MEM32(pp) = 0;
    if (W32_ARG(3) != 0) {
        W32_RETURN(4, DSERR_NOAGGREGATION);
        return;
    }
    uint32_t size = MEM32(desc + G_DSBD_dwSize), flags = MEM32(desc + G_DSBD_dwFlags);
    uint32_t bytes = MEM32(desc + G_DSBD_dwBufferBytes), wfx = MEM32(desc + G_DSBD_lpwfxFormat);
    if (size != 20u && size != 36u) {
        W32_RETURN(4, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&dev->lock);
    uint32_t hr = DS_OK;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes != 0 || wfx != 0 || (flags & DSBCAPS_CTRLFREQUENCY)) {
            hr = DSERR_INVALIDPARAM;
        } else if (dev->primary != NULL) { /* one primary per device */
            dev->primary->refs++;
            MEM32(pp) = dev->primary->guest;
        } else {
            struct ds_buffer *b = buffer_new(dev, flags);
            if (b == NULL) {
                hr = DSERR_OUTOFMEMORY;
            } else {
                dev->primary = b;
                MEM32(pp) = b->guest;
            }
        }
    } else {
        struct ds_buffer tmp = {0};
        if (wfx == 0 || bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX)
            hr = DSERR_INVALIDPARAM;
        else
            hr = format_in(wfx, &tmp);
        /* DirectSound 8: 3D buffers are mono and have no pan control. */
        if (hr == DS_OK && (flags & DSBCAPS_CTRL3D) &&
            (tmp.channels != 1 || (flags & DSBCAPS_CTRLPAN)))
            hr = DSERR_INVALIDPARAM;
        uint32_t nframes = hr == DS_OK ? bytes / tmp.align : 0;
        struct ds_data *data = NULL;
        if (hr == DS_OK) {
            data = calloc(1, sizeof *data);
            uint32_t g = data != NULL ? guest_malloc(nframes * tmp.align) : 0;
            if (g == 0) {
                free(data);
                hr = DSERR_OUTOFMEMORY;
            } else {
                *data = (struct ds_data){g, nframes * tmp.align, 1};
                memset(guest_ptr(g), tmp.bits == 8 ? 0x80 : 0, data->bytes);
            }
        }
        struct ds_buffer *b = hr == DS_OK ? buffer_new(dev, flags) : NULL;
        if (hr == DS_OK && b == NULL) {
            guest_free(data->guest);
            free(data);
            hr = DSERR_OUTOFMEMORY;
        }
        if (b != NULL) {
            b->channels = tmp.channels;
            b->bits = tmp.bits;
            b->align = tmp.align;
            b->format_rate = b->freq = tmp.format_rate;
            b->nframes = nframes;
            b->data = data;
            MEM32(pp) = b->guest;
        }
    }
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(4, hr);
}

/* GetCaps(LPDSCAPS): a software device with no hardware buffers. */
static void DSound_GetCaps(void)
{
    uint32_t caps = W32_ARG(1);
    if (caps == 0 || MEM32(caps) != G_DSCAPS_SIZE) {
        W32_RETURN(2, DSERR_INVALIDPARAM);
        return;
    }
    memset(guest_ptr(caps + 4u), 0, G_DSCAPS_SIZE - 4u);
    MEM32(caps + 4u) = 0x00000F1Fu; /* PRIMARY/SECONDARY MONO STEREO 8BIT 16BIT, CONTINUOUSRATE */
    MEM32(caps + 8u) = DSBFREQUENCY_MIN;
    MEM32(caps + 12u) = DSBFREQUENCY_MAX;
    MEM32(caps + 16u) = 1; /* dwPrimaryBuffers */
    W32_RETURN(2, DS_OK);
}

/* DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER *dup):
 * the copy shares the audio memory; it starts stopped at 0 with the
 * original's frequency, volume and pan. */
static void DSound_DuplicateSoundBuffer(void)
{
    struct ds_device *dev = dev_self();
    uint32_t orig = W32_ARG(1), pp = W32_ARG(2);
    if (orig == 0 || pp == 0) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    MEM32(pp) = 0;
    struct ds_buffer *o = com_guest_self(orig);
    pthread_mutex_lock(&dev->lock);
    uint32_t hr = DS_OK;
    if (o->data == NULL) {
        hr = DSERR_INVALIDCALL;
    } else {
        struct ds_buffer *b = buffer_new(dev, o->flags);
        if (b == NULL) {
            hr = DSERR_OUTOFMEMORY;
        } else {
            b->data = o->data;
            b->data->refs++;
            b->channels = o->channels;
            b->bits = o->bits;
            b->align = o->align;
            b->nframes = o->nframes;
            b->format_rate = o->format_rate;
            b->freq = o->freq;
            b->volume = o->volume;
            b->pan = o->pan;
            MEM32(pp) = b->guest;
        }
    }
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(3, hr);
}

/* SetCooperativeLevel(HWND, DWORD) */
static void DSound_SetCooperativeLevel(void)
{
    struct ds_device *dev = dev_self();
    uint32_t level = W32_ARG(2);
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    dev->coop = level;
    W32_RETURN(3, DS_OK);
}

/* GetSpeakerConfig(LPDWORD) */
static void DSound_GetSpeakerConfig(void)
{
    uint32_t p = W32_ARG(1);
    if (p == 0) {
        W32_RETURN(2, DSERR_INVALIDPARAM);
        return;
    }
    MEM32(p) = DSSPEAKER_STEREO | (DSSPEAKER_GEOMETRY_WIDE << 16);
    W32_RETURN(2, DS_OK);
}

DS_STUB(DSound_Compact, "IDirectSound8::Compact", 0)
DS_STUB(DSound_SetSpeakerConfig, "IDirectSound8::SetSpeakerConfig", 1)
DS_STUB(DSound_Initialize, "IDirectSound8::Initialize", 1)
DS_STUB(DSound_VerifyCertification, "IDirectSound8::VerifyCertification", 1)

static const recomp_func_t DSound_methods[] = {
    DSound_QueryInterface, DSound_AddRef, DSound_Release, DSound_CreateSoundBuffer,
    DSound_GetCaps, DSound_DuplicateSoundBuffer, DSound_SetCooperativeLevel, DSound_Compact,
    DSound_GetSpeakerConfig, DSound_SetSpeakerConfig, DSound_Initialize,
    DSound_VerifyCertification,
};
static const com_interface_t DSound_iface = {"IDirectSound8", DSound_methods, 12};
_Static_assert(sizeof DSound_methods / sizeof DSound_methods[0] == 12, "IDirectSound8");

/* ---- IDirectSoundBuffer8, shared IUnknown of all three buffer objects ------ */

/* QueryInterface on the buffer, its 3D buffer or the primary's listener. */
static void Buffer_QueryInterface(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t ppv = W32_ARG(2);
    if (ppv == 0) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    com_guid_t iid = com_guid_read(W32_ARG(1));
    int primary = b->data == NULL, has3d = (b->flags & DSBCAPS_CTRL3D) != 0;
    const com_interface_t *side = NULL;
    uint32_t out = 0;
    if (com_guid_eq(&iid, &IID_IUnknown) || com_guid_eq(&iid, &IID_IDirectSoundBuffer) ||
        (!primary && com_guid_eq(&iid, &IID_IDirectSoundBuffer8)))
        out = b->guest;
    else if (has3d && !primary && com_guid_eq(&iid, &IID_IDirectSound3DBuffer))
        side = &Buffer3D_iface;
    else if (has3d && primary && com_guid_eq(&iid, &IID_IDirectSound3DListener))
        side = &Listener_iface;
    if (out == 0 && side == NULL) {
        MEM32(ppv) = 0;
        W32_RETURN(3, COM_E_NOINTERFACE);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    if (side != NULL) {
        if (b->guest_3d == 0)
            b->guest_3d = com_guest_new(side, b);
        out = b->guest_3d;
    }
    b->refs++;
    pthread_mutex_unlock(&b->dev->lock);
    MEM32(ppv) = out;
    W32_RETURN(3, DS_OK);
}

static void Buffer_AddRef(void)
{
    struct ds_buffer *b = buf_self();
    pthread_mutex_lock(&b->dev->lock);
    uint32_t n = ++b->refs;
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(1, n);
}

static void Buffer_Release(void)
{
    struct ds_buffer *b = buf_self();
    struct ds_device *dev = b->dev;
    pthread_mutex_lock(&dev->lock);
    uint32_t n = --b->refs;
    if (n == 0)
        buffer_free(b);
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(1, n);
}

/* Write cursor: 10 ms ahead of the play cursor while playing. Caller holds
 * the lock. */
static void cursors(const struct ds_buffer *b, uint32_t *play, uint32_t *write)
{
    *play = (uint32_t)b->pos * b->align;
    uint32_t lead = b->playing ? (b->freq / 100u) * b->align : 0;
    *write = (*play + lead) % (b->nframes * b->align);
}

/* GetCurrentPosition(LPDWORD pdwCurrentPlayCursor, LPDWORD pdwCurrentWriteCursor) */
static void Buffer_GetCurrentPosition(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t pplay = W32_ARG(1), pwrite = W32_ARG(2);
    if (b->data == NULL) {
        W32_RETURN(3, DSERR_INVALIDCALL);
        return;
    }
    uint32_t play, write;
    pthread_mutex_lock(&b->dev->lock);
    cursors(b, &play, &write);
    pthread_mutex_unlock(&b->dev->lock);
    if (pplay != 0)
        MEM32(pplay) = play;
    if (pwrite != 0)
        MEM32(pwrite) = write;
    W32_RETURN(3, DS_OK);
}

static void Buffer_GetVolume(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t p = W32_ARG(1);
    uint32_t hr = !(b->flags & DSBCAPS_CTRLVOLUME) ? DSERR_CONTROLUNAVAIL
                  : p == 0                         ? DSERR_INVALIDPARAM
                                                   : DS_OK;
    if (hr == DS_OK)
        MEM32(p) = (uint32_t)b->volume;
    W32_RETURN(2, hr);
}

static void Buffer_GetStatus(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t p = W32_ARG(1);
    if (p == 0) {
        W32_RETURN(2, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    MEM32(p) = b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u) : 0u;
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(2, DS_OK);
}

/* Lock(DWORD dwOffset, DWORD dwBytes, LPVOID *ppvAudioPtr1, LPDWORD
 *      pdwAudioBytes1, LPVOID *ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD
 *      dwFlags). The second region is the wrap to the start; without
 * ppvAudioPtr2 the lock stops at the end of the buffer. */
static void Buffer_Lock(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t offset = W32_ARG(1), bytes = W32_ARG(2), pp1 = W32_ARG(3), pn1 = W32_ARG(4);
    uint32_t pp2 = W32_ARG(5), pn2 = W32_ARG(6), flags = W32_ARG(7);
    if (b->data == NULL) {
        W32_RETURN(8, DSERR_INVALIDCALL);
        return;
    }
    uint32_t size = b->data->bytes;
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        uint32_t play;
        pthread_mutex_lock(&b->dev->lock);
        cursors(b, &play, &offset);
        pthread_mutex_unlock(&b->dev->lock);
    }
    if (flags & DSBLOCK_ENTIREBUFFER)
        bytes = size;
    if (pp1 == 0 || pn1 == 0 || offset >= size || bytes == 0 || bytes > size) {
        W32_RETURN(8, DSERR_INVALIDPARAM);
        return;
    }
    uint32_t n1 = bytes < size - offset ? bytes : size - offset;
    MEM32(pp1) = b->data->guest + offset;
    MEM32(pn1) = n1;
    if (pp2 != 0) {
        MEM32(pp2) = bytes > n1 ? b->data->guest : 0u;
        if (pn2 != 0)
            MEM32(pn2) = bytes - n1;
    }
    W32_RETURN(8, DS_OK);
}

/* Play(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) */
static void Buffer_Play(void)
{
    struct ds_buffer *b = buf_self();
    if (W32_ARG(1) != 0) {
        W32_RETURN(4, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    if (b->data != NULL) {
        b->playing = 1;
        b->looping = (W32_ARG(3) & DSBPLAY_LOOPING) != 0;
    }
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(4, DS_OK);
}

static void Buffer_SetCurrentPosition(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t pos = W32_ARG(1);
    if (b->data == NULL || pos >= b->data->bytes) {
        W32_RETURN(2, b->data == NULL ? DSERR_INVALIDCALL : DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    b->pos = (double)(pos / b->align);
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(2, DS_OK);
}

/* SetFormat(LPCWAVEFORMATEX): the primary buffer only. The mixer always
 * writes the device's own format, so the primary format is checked and
 * otherwise has no effect. */
static void Buffer_SetFormat(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t wfx = W32_ARG(1);
    struct ds_buffer tmp = {0};
    uint32_t hr = b->data != NULL ? DSERR_INVALIDCALL
                  : wfx == 0      ? DSERR_INVALIDPARAM
                                  : format_in(wfx, &tmp);
    W32_RETURN(2, hr);
}

static void Buffer_SetVolume(void)
{
    struct ds_buffer *b = buf_self();
    int32_t v = (int32_t)W32_ARG(1);
    uint32_t hr = !(b->flags & DSBCAPS_CTRLVOLUME)               ? DSERR_CONTROLUNAVAIL
                  : v < DSBVOLUME_MIN || v > DSBVOLUME_MAX ? DSERR_INVALIDPARAM
                                                           : DS_OK;
    if (hr == DS_OK) {
        pthread_mutex_lock(&b->dev->lock);
        b->volume = v;
        pthread_mutex_unlock(&b->dev->lock);
    }
    W32_RETURN(2, hr);
}

static void Buffer_SetPan(void)
{
    struct ds_buffer *b = buf_self();
    int32_t p = (int32_t)W32_ARG(1);
    uint32_t hr = !(b->flags & DSBCAPS_CTRLPAN)                 ? DSERR_CONTROLUNAVAIL
                  : p < DSBPAN_LEFT || p > DSBPAN_RIGHT ? DSERR_INVALIDPARAM
                                                        : DS_OK;
    if (hr == DS_OK) {
        pthread_mutex_lock(&b->dev->lock);
        b->pan = p;
        pthread_mutex_unlock(&b->dev->lock);
    }
    W32_RETURN(2, hr);
}

/* SetFrequency(DWORD): DSBFREQUENCY_ORIGINAL restores the format's rate. */
static void Buffer_SetFrequency(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t f = W32_ARG(1);
    uint32_t hr = !(b->flags & DSBCAPS_CTRLFREQUENCY) || b->data == NULL ? DSERR_CONTROLUNAVAIL
                  : f != DSBFREQUENCY_ORIGINAL &&
                          (f < DSBFREQUENCY_MIN || f > DSBFREQUENCY_MAX)
                      ? DSERR_INVALIDPARAM
                      : DS_OK;
    if (hr == DS_OK) {
        pthread_mutex_lock(&b->dev->lock);
        b->freq = f == DSBFREQUENCY_ORIGINAL ? b->format_rate : f;
        pthread_mutex_unlock(&b->dev->lock);
    }
    W32_RETURN(2, hr);
}

static void Buffer_Stop(void)
{
    struct ds_buffer *b = buf_self();
    pthread_mutex_lock(&b->dev->lock);
    b->playing = 0;
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(1, DS_OK);
}

/* Unlock(LPVOID, DWORD, LPVOID, DWORD): the data was written in place. */
static void Buffer_Unlock(void)
{
    struct ds_buffer *b = buf_self();
    W32_RETURN(5, b->data == NULL ? DSERR_INVALIDCALL : DS_OK);
}

/* Restore(): a software buffer is never lost. */
static void Buffer_Restore(void)
{
    W32_RETURN(1, DS_OK);
}

DS_STUB(Buffer_GetCaps, "IDirectSoundBuffer8::GetCaps", 1)
DS_STUB(Buffer_GetFormat, "IDirectSoundBuffer8::GetFormat", 3)
DS_STUB(Buffer_GetPan, "IDirectSoundBuffer8::GetPan", 1)
DS_STUB(Buffer_GetFrequency, "IDirectSoundBuffer8::GetFrequency", 1)
DS_STUB(Buffer_Initialize, "IDirectSoundBuffer8::Initialize", 2)
DS_STUB(Buffer_SetFX, "IDirectSoundBuffer8::SetFX", 3)
DS_STUB(Buffer_AcquireResources, "IDirectSoundBuffer8::AcquireResources", 3)
DS_STUB(Buffer_GetObjectInPath, "IDirectSoundBuffer8::GetObjectInPath", 4)

static const recomp_func_t Buffer_methods[] = {
    Buffer_QueryInterface, Buffer_AddRef, Buffer_Release, Buffer_GetCaps,
    Buffer_GetCurrentPosition, Buffer_GetFormat, Buffer_GetVolume, Buffer_GetPan,
    Buffer_GetFrequency, Buffer_GetStatus, Buffer_Initialize, Buffer_Lock, Buffer_Play,
    Buffer_SetCurrentPosition, Buffer_SetFormat, Buffer_SetVolume, Buffer_SetPan,
    Buffer_SetFrequency, Buffer_Stop, Buffer_Unlock, Buffer_Restore, Buffer_SetFX,
    Buffer_AcquireResources, Buffer_GetObjectInPath,
};
static const com_interface_t Buffer_iface = {"IDirectSoundBuffer8", Buffer_methods, 24};
_Static_assert(sizeof Buffer_methods / sizeof Buffer_methods[0] == 24, "IDirectSoundBuffer8");

/* ---- Deferred 3D settings -------------------------------------------------- */

/* dwApply: DS3D_IMMEDIATE sets the value now and in the pending set;
 * DS3D_DEFERRED only in the pending set, until CommitDeferredSettings. */
static int apply_ok(uint32_t apply)
{
    return apply == DS3D_IMMEDIATE || apply == DS3D_DEFERRED;
}

#define SET3D(b, apply, field_expr)            \
    do {                                       \
        struct ds_3d *s_ = &(b)->pend;         \
        field_expr;                            \
        if ((apply) == DS3D_IMMEDIATE) {       \
            s_ = &(b)->cur;                    \
            field_expr;                        \
        }                                      \
    } while (0)

#define SETLIS(dev, apply, field_expr)         \
    do {                                       \
        struct ds_listener *s_ = &(dev)->lis_pend; \
        field_expr;                            \
        if ((apply) == DS3D_IMMEDIATE) {       \
            s_ = &(dev)->lis_cur;              \
            field_expr;                        \
        }                                      \
    } while (0)

/* ---- IDirectSound3DBuffer --------------------------------------------------- */

/* SetMaxDistance(D3DVALUE, DWORD dwApply) */
static void B3D_SetMaxDistance(void)
{
    struct ds_buffer *b = buf_self();
    float d = arg_float(1);
    uint32_t apply = W32_ARG(2);
    if (!apply_ok(apply) || !(d > 0.0f)) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    SET3D(b, apply, s_->max_dist = d);
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(3, DS_OK);
}

/* SetMinDistance(D3DVALUE, DWORD dwApply) */
static void B3D_SetMinDistance(void)
{
    struct ds_buffer *b = buf_self();
    float d = arg_float(1);
    uint32_t apply = W32_ARG(2);
    if (!apply_ok(apply) || !(d > 0.0f)) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    SET3D(b, apply, s_->min_dist = d);
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(3, DS_OK);
}

/* SetMode(DWORD dwMode, DWORD dwApply) */
static void B3D_SetMode(void)
{
    struct ds_buffer *b = buf_self();
    uint32_t mode = W32_ARG(1), apply = W32_ARG(2);
    if (!apply_ok(apply) || mode > DS3DMODE_DISABLE) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    SET3D(b, apply, s_->mode = mode);
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(3, DS_OK);
}

/* SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) */
static void B3D_SetPosition(void)
{
    struct ds_buffer *b = buf_self();
    float x = arg_float(1), y = arg_float(2), z = arg_float(3);
    uint32_t apply = W32_ARG(4);
    if (!apply_ok(apply)) {
        W32_RETURN(5, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&b->dev->lock);
    SET3D(b, apply, (s_->pos[0] = x, s_->pos[1] = y, s_->pos[2] = z));
    pthread_mutex_unlock(&b->dev->lock);
    W32_RETURN(5, DS_OK);
}

DS_STUB(B3D_GetAllParameters, "IDirectSound3DBuffer::GetAllParameters", 1)
DS_STUB(B3D_GetConeAngles, "IDirectSound3DBuffer::GetConeAngles", 2)
DS_STUB(B3D_GetConeOrientation, "IDirectSound3DBuffer::GetConeOrientation", 1)
DS_STUB(B3D_GetConeOutsideVolume, "IDirectSound3DBuffer::GetConeOutsideVolume", 1)
DS_STUB(B3D_GetMaxDistance, "IDirectSound3DBuffer::GetMaxDistance", 1)
DS_STUB(B3D_GetMinDistance, "IDirectSound3DBuffer::GetMinDistance", 1)
DS_STUB(B3D_GetMode, "IDirectSound3DBuffer::GetMode", 1)
DS_STUB(B3D_GetPosition, "IDirectSound3DBuffer::GetPosition", 1)
DS_STUB(B3D_GetVelocity, "IDirectSound3DBuffer::GetVelocity", 1)
DS_STUB(B3D_SetAllParameters, "IDirectSound3DBuffer::SetAllParameters", 2)
DS_STUB(B3D_SetConeAngles, "IDirectSound3DBuffer::SetConeAngles", 3)
DS_STUB(B3D_SetConeOrientation, "IDirectSound3DBuffer::SetConeOrientation", 4)
DS_STUB(B3D_SetConeOutsideVolume, "IDirectSound3DBuffer::SetConeOutsideVolume", 2)
DS_STUB(B3D_SetVelocity, "IDirectSound3DBuffer::SetVelocity", 4)

static const recomp_func_t Buffer3D_methods[] = {
    Buffer_QueryInterface, Buffer_AddRef, Buffer_Release, B3D_GetAllParameters,
    B3D_GetConeAngles, B3D_GetConeOrientation, B3D_GetConeOutsideVolume, B3D_GetMaxDistance,
    B3D_GetMinDistance, B3D_GetMode, B3D_GetPosition, B3D_GetVelocity, B3D_SetAllParameters,
    B3D_SetConeAngles, B3D_SetConeOrientation, B3D_SetConeOutsideVolume, B3D_SetMaxDistance,
    B3D_SetMinDistance, B3D_SetMode, B3D_SetPosition, B3D_SetVelocity,
};
static const com_interface_t Buffer3D_iface = {"IDirectSound3DBuffer", Buffer3D_methods, 21};
_Static_assert(sizeof Buffer3D_methods / sizeof Buffer3D_methods[0] == 21, "IDirectSound3DBuffer");

/* ---- IDirectSound3DListener -------------------------------------------------- */

static struct ds_device *lis_dev(void)
{
    return buf_self()->dev;
}

/* SetOrientation(xFront, yFront, zFront, xTop, yTop, zTop, dwApply) */
static void Lis_SetOrientation(void)
{
    struct ds_device *dev = lis_dev();
    float v[6];
    for (unsigned i = 0; i < 6; i++)
        v[i] = arg_float(1 + i);
    uint32_t apply = W32_ARG(7);
    if (!apply_ok(apply)) {
        W32_RETURN(8, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&dev->lock);
    SETLIS(dev, apply, (memcpy(s_->front, v, 12), memcpy(s_->top, v + 3, 12)));
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(8, DS_OK);
}

/* SetPosition(x, y, z, dwApply) */
static void Lis_SetPosition(void)
{
    struct ds_device *dev = lis_dev();
    float x = arg_float(1), y = arg_float(2), z = arg_float(3);
    uint32_t apply = W32_ARG(4);
    if (!apply_ok(apply)) {
        W32_RETURN(5, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&dev->lock);
    SETLIS(dev, apply, (s_->pos[0] = x, s_->pos[1] = y, s_->pos[2] = z));
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(5, DS_OK);
}

/* SetRolloffFactor(D3DVALUE, DWORD dwApply): 0 to DS3D_MAXROLLOFFFACTOR. */
static void Lis_SetRolloffFactor(void)
{
    struct ds_device *dev = lis_dev();
    float r = arg_float(1);
    uint32_t apply = W32_ARG(2);
    if (!apply_ok(apply) || !(r >= 0.0f && r <= DS3D_MAXROLLOFFFACTOR)) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    pthread_mutex_lock(&dev->lock);
    SETLIS(dev, apply, s_->rolloff = r);
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(3, DS_OK);
}

/* CommitDeferredSettings(): the listener's and every 3D buffer's pending
 * settings take effect. */
static void Lis_CommitDeferredSettings(void)
{
    struct ds_device *dev = lis_dev();
    pthread_mutex_lock(&dev->lock);
    dev->lis_cur = dev->lis_pend;
    for (struct ds_buffer *b = dev->buffers; b != NULL; b = b->next)
        b->cur = b->pend;
    pthread_mutex_unlock(&dev->lock);
    W32_RETURN(1, DS_OK);
}

DS_STUB(Lis_GetAllParameters, "IDirectSound3DListener::GetAllParameters", 1)
DS_STUB(Lis_GetDistanceFactor, "IDirectSound3DListener::GetDistanceFactor", 1)
DS_STUB(Lis_GetDopplerFactor, "IDirectSound3DListener::GetDopplerFactor", 1)
DS_STUB(Lis_GetOrientation, "IDirectSound3DListener::GetOrientation", 2)
DS_STUB(Lis_GetPosition, "IDirectSound3DListener::GetPosition", 1)
DS_STUB(Lis_GetRolloffFactor, "IDirectSound3DListener::GetRolloffFactor", 1)
DS_STUB(Lis_GetVelocity, "IDirectSound3DListener::GetVelocity", 1)
DS_STUB(Lis_SetAllParameters, "IDirectSound3DListener::SetAllParameters", 2)
DS_STUB(Lis_SetDistanceFactor, "IDirectSound3DListener::SetDistanceFactor", 2)
DS_STUB(Lis_SetDopplerFactor, "IDirectSound3DListener::SetDopplerFactor", 2)
DS_STUB(Lis_SetVelocity, "IDirectSound3DListener::SetVelocity", 4)

static const recomp_func_t Listener_methods[] = {
    Buffer_QueryInterface, Buffer_AddRef, Buffer_Release, Lis_GetAllParameters,
    Lis_GetDistanceFactor, Lis_GetDopplerFactor, Lis_GetOrientation, Lis_GetPosition,
    Lis_GetRolloffFactor, Lis_GetVelocity, Lis_SetAllParameters, Lis_SetDistanceFactor,
    Lis_SetDopplerFactor, Lis_SetOrientation, Lis_SetPosition, Lis_SetRolloffFactor,
    Lis_SetVelocity, Lis_CommitDeferredSettings,
};
static const com_interface_t Listener_iface = {"IDirectSound3DListener", Listener_methods, 18};
_Static_assert(sizeof Listener_methods / sizeof Listener_methods[0] == 18,
               "IDirectSound3DListener");

/* ---- DirectSoundCreate8 ------------------------------------------------------ */

/* HRESULT DirectSoundCreate8(LPCGUID lpcGuidDevice, LPDIRECTSOUND8 *ppDS8,
 *                            LPUNKNOWN pUnkOuter)   stdcall, pops 12 */
static void imp_DirectSoundCreate8(void)
{
    uint32_t gdev = W32_ARG(0), pp = W32_ARG(1);
    if (pp == 0) {
        W32_RETURN(3, DSERR_INVALIDPARAM);
        return;
    }
    MEM32(pp) = 0;
    if (W32_ARG(2) != 0) {
        W32_RETURN(3, DSERR_NOAGGREGATION);
        return;
    }
    if (gdev != 0) {
        com_guid_t g = com_guid_read(gdev);
        if (!com_guid_eq(&g, &DSDEVID_DefaultPlayback) &&
            !com_guid_eq(&g, &DSDEVID_DefaultVoicePlayback)) {
            W32_RETURN(3, DSERR_NODRIVER);
            return;
        }
    }
    struct ds_device *dev = calloc(1, sizeof *dev);
    if (dev == NULL) {
        W32_RETURN(3, DSERR_OUTOFMEMORY);
        return;
    }
    pthread_mutex_init(&dev->lock, NULL);
    static const struct ds_listener lis = {
        {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.0f};
    dev->lis_cur = dev->lis_pend = lis;
    dev->refs = 1;
    if (g_offline_rate != 0) {
        dev->rate = g_offline_rate;
    } else {
        uint32_t hr = device_open(dev);
        if (hr != 0) {
            fprintf(stderr, "DirectSoundCreate8: no host audio device (FAudio 0x%08X)\n", hr);
            device_close(dev);
            pthread_mutex_destroy(&dev->lock);
            free(dev);
            W32_RETURN(3, DSERR_NODRIVER);
            return;
        }
    }
    dev->guest = com_guest_new(&DSound_iface, dev);
    MEM32(pp) = dev->guest;
    W32_RETURN(3, DS_OK);
}

/* LEGOIndy.exe imports it as DSOUND.dll ordinal 11; the name serves the
 * resolver's ordinal-to-name map. */
const win32_export_t win32_exports_dsound[] = {
    {"DSOUND.dll", "DirectSoundCreate8", 11, imp_DirectSoundCreate8},
    {0},
};
