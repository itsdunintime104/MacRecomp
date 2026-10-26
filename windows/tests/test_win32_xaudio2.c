/* Win32 XAudio2 2.4 test: slices X1-X3 of docs/AUDIO_PLAN.md. Replays the
 * game's audio init and reverb submix set-up,
 * then plays a PCM buffer through a source voice with a guest callback
 * object, then each format the game sends through its two CreateSourceVoice
 * sites into the master and the reverb submix.
 * Callbacks must arrive with a TIB, x87 0x027F and MXCSR 0x1F80. Every method is called through a guest stack; call() checks the
 * stdcall pop and ebx/esi/edi/ebp.
 *
 * Links win32_xaudio2.c with win32_sync.c (the real w32_host_thread_attach),
 * guest_mem.c, guest_heap.c, dlmalloc.c, host_funcs.c, guest_call.c,
 * recomp_glue.c and -lFAudio. com_guest.h and the win32_core.c handle table
 * and last error are doubled below; win32_core.c and com_guest.c are not
 * linked. Run with SDL_AUDIO_DRIVER=dummy (no sound reaches the speakers). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "com_guest.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "quiet_abort.h"
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

#define GUEST_LO 0x00A00000u
#define DATA_AREA 0x00A00000u /* argument structs, audio data */
#define OBJ_AREA 0x00C00000u  /* COM objects, 8 bytes each */
#define VT_AREA 0x00C80000u   /* vtables */
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
static float gfloat(uint32_t a)
{
    float f;
    memcpy(&f, guest_ptr(a), 4);
    return f;
}

/* ---- com_guest.h double (main thread only) ----------------------------- */

#define MAX_OBJS 64
static struct {
    const com_interface_t *iface;
    void *self;
    int live;
} g_objs[MAX_OBJS];
static int g_n_objs, g_freed;
static struct { const com_interface_t *iface; uint32_t va; } g_vts[8];
static uint32_t g_vt_next = VT_AREA;

uint32_t com_guest_new(const com_interface_t *iface, void *host_self)
{
    uint32_t vt = 0;
    for (int i = 0; i < 8 && !vt; i++) {
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
/* As com_guest.c: the object must be a live one at its own address. */
static int obj_index(uint32_t guest_this)
{
    if (guest_this < OBJ_AREA || guest_this >= OBJ_AREA + 8u * MAX_OBJS)
        return -1;
    uint32_t i = MEM32(guest_this + 4);
    if (i >= (uint32_t)g_n_objs || !g_objs[i].live || OBJ_AREA + 8u * i != guest_this)
        return -1;
    return (int)i;
}
void *com_guest_self(uint32_t guest_this)
{
    int i = obj_index(guest_this);
    if (i < 0) {
        fprintf(stderr, "UNSUPPORTED com: bad object 0x%08X\n", guest_this);
        abort();
    }
    return g_objs[i].self;
}
const com_interface_t *com_guest_iface(uint32_t guest_this)
{
    int i = obj_index(guest_this);
    return i < 0 ? NULL : g_objs[i].iface;
}
void com_guest_free(uint32_t guest_this)
{
    int i = obj_index(guest_this);
    CHECK(i >= 0);
    if (i < 0)
        return;
    g_objs[i].live = 0;
    MEM32(guest_this) = 0;
    MEM32(guest_this + 4) = 0;
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

static struct {
    com_guid_t clsid;
    com_factory_t factory;
} g_classes[8];
static int g_n_classes;
void com_register_class(const com_guid_t *clsid, const char *name, com_factory_t factory)
{
    (void)name;
    g_classes[g_n_classes].clsid = *clsid;
    g_classes[g_n_classes].factory = factory;
    g_n_classes++;
}
static com_factory_t class_factory(const com_guid_t *clsid)
{
    for (int i = 0; i < g_n_classes; i++)
        if (com_guid_eq(&g_classes[i].clsid, clsid))
            return g_classes[i].factory;
    return NULL;
}

/* ---- win32_core.c doubles for win32_sync.c ----------------------------- */

/* win32_sync.c needs the handle table and last error; this test makes no
 * handles. */
static __thread uint32_t t_err;
uint32_t w32_last_error(void) { return t_err; }
void w32_set_last_error(uint32_t code) { t_err = code; }
uint32_t w32_handle_new(struct w32_object *obj)
{
    (void)obj;
    return 0;
}
struct w32_object *w32_handle_get(uint32_t h, enum w32_kind kind)
{
    (void)h, (void)kind;
    return NULL;
}
uint32_t w32_handle_close(uint32_t h)
{
    (void)h;
    return W32_FALSE;
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

/* COM method idx of obj, through its guest vtable. */
static recomp_func_t method(uint32_t obj, unsigned idx)
{
    return guest_resolve(MEM32(MEM32(obj) + 4u * idx));
}

static uint32_t guid_at(const com_guid_t *g)
{
    uint32_t a = guest_alloc(16);
    MEM32(a) = g->d1;
    MEM16(a + 4) = g->d2;
    MEM16(a + 6) = g->d3;
    memcpy(guest_ptr(a + 8), g->d4, 8);
    return a;
}

static const com_guid_t IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t IID_IXAudio2 = {
    0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
static const com_guid_t IID_Other = {
    0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}};
static const com_guid_t CLSID_XAudio2 = {
    0x03219e78, 0x5bc3, 0x44d1, {0xb9, 0x2e, 0xf6, 0x3d, 0x89, 0xcc, 0x65, 0x26}};
static const com_guid_t CLSID_XAudio2_Debug = {
    0x4256535c, 0x1ea4, 0x4d4b, {0x8a, 0xd5, 0xf9, 0xdb, 0x76, 0x2e, 0xca, 0x9e}};
static const com_guid_t CLSID_AudioReverb = {
    0x8bb7778b, 0x645b, 0x4475, {0x9a, 0x73, 0x1d, 0xe3, 0x17, 0x0b, 0xd3, 0xaf}};
static const com_guid_t CLSID_AudioReverb_Debug = {
    0xda7738a2, 0xcd0c, 0x4367, {0x9a, 0xac, 0xd7, 0xea, 0xd7, 0xc6, 0x4f, 0x98}};

/* IXAudio2 slots. */
enum {
    XA_QueryInterface, XA_AddRef, XA_Release, XA_GetDeviceCount, XA_GetDeviceDetails,
    XA_Initialize, XA_RegisterForCallbacks, XA_UnregisterForCallbacks, XA_CreateSourceVoice,
    XA_CreateSubmixVoice, XA_CreateMasteringVoice, XA_StartEngine, XA_StopEngine,
    XA_CommitChanges, XA_GetPerformanceData, XA_SetDebugConfiguration,
};
/* IXAudio2Voice slots, then IXAudio2SourceVoice. */
enum {
    V_GetVoiceDetails, V_SetOutputVoices, V_SetEffectChain, V_EnableEffect, V_DisableEffect,
    V_GetEffectState, V_SetEffectParameters, V_GetEffectParameters, V_SetFilterParameters,
    V_GetFilterParameters, V_SetOutputFilterParameters, V_GetOutputFilterParameters,
    V_SetVolume, V_GetVolume, V_SetChannelVolumes, V_GetChannelVolumes, V_SetOutputMatrix,
    V_GetOutputMatrix, V_DestroyVoice,
    S_Start, S_Stop, S_SubmitSourceBuffer, S_FlushSourceBuffers, S_Discontinuity, S_ExitLoop,
    S_GetState, S_SetFrequencyRatio, S_GetFrequencyRatio, S_SetSourceSampleRate,
};

/* ---- Guest callback objects -------------------------------------------- */

/* Each slot is a host thunk that records the call and pops like the stdcall
 * guest method. Voice callbacks 2-6 are logged; the per-pass ones counted. */
static pthread_t g_main;
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned slot; uint32_t self, a1, a2; } g_ev[256];
static int g_nev;
static int g_vpass[2], g_epass[3];
static int g_cb_on_main, g_cb_unattached, g_cb_after_destroy, g_cb_bad_this, g_cb_bad_fpu;
static int g_destroyed;
static uint32_t g_vcb_obj, g_ecb_obj;

static void record(int engine, unsigned slot, unsigned nargs)
{
    uint32_t self = W32_ARG(0);
    uint32_t a1 = nargs > 1 ? W32_ARG(1) : 0, a2 = nargs > 2 ? W32_ARG(2) : 0;
    pthread_mutex_lock(&g_ev_lock);
    if (pthread_equal(pthread_self(), g_main))
        g_cb_on_main++;
    if (g_fs_base == 0)
        g_cb_unattached++;
    /* A Windows thread starts at x87 0x027F and MXCSR 0x1F80. */
    if (g_fp_control_word != 0x027F || g_mxcsr != 0x1F80)
        g_cb_bad_fpu++;
    if (self != (engine ? g_ecb_obj : g_vcb_obj))
        g_cb_bad_this++;
    if (engine) {
        g_epass[slot]++;
    } else {
        if (__atomic_load_n(&g_destroyed, __ATOMIC_ACQUIRE))
            g_cb_after_destroy++;
        if (slot < 2) {
            g_vpass[slot]++;
        } else if (g_nev < 256) {
            g_ev[g_nev].slot = slot;
            g_ev[g_nev].self = self;
            g_ev[g_nev].a1 = a1;
            g_ev[g_nev].a2 = a2;
            g_nev++;
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
    W32_RETURN(nargs, 0);
}
static void vcb0(void) { record(0, 0, 2); }
static void vcb1(void) { record(0, 1, 1); }
static void vcb2(void) { record(0, 2, 1); }
static void vcb3(void) { record(0, 3, 2); }
static void vcb4(void) { record(0, 4, 2); }
static void vcb5(void) { record(0, 5, 2); }
static void vcb6(void) { record(0, 6, 3); }
static void ecb0(void) { record(1, 0, 1); }
static void ecb1(void) { record(1, 1, 1); }
static void ecb2(void) { record(1, 2, 2); }

static uint32_t guest_object(const recomp_func_t *fns, unsigned n)
{
    uint32_t vt = guest_alloc(4u * n), obj = guest_alloc(8);
    for (unsigned i = 0; i < n; i++)
        MEM32(vt + 4u * i) = host_func_va(fns[i]);
    MEM32(obj) = vt;
    return obj;
}

/* Index of the first event (slot, a1) at or after index from, or -1. a1 is
 * not compared for OnStreamEnd, which has no argument, or when it is ANY. */
#define ANY 0xFFFFFFFFu
static int find_ev(unsigned slot, uint32_t a1, int from)
{
    int r = -1;
    pthread_mutex_lock(&g_ev_lock);
    for (int i = from; i < g_nev && r < 0; i++)
        if (g_ev[i].slot == slot && (slot == 2 || a1 == ANY || g_ev[i].a1 == a1))
            r = i;
    pthread_mutex_unlock(&g_ev_lock);
    return r;
}
static int counter(const int *c)
{
    pthread_mutex_lock(&g_ev_lock);
    int v = *c;
    pthread_mutex_unlock(&g_ev_lock);
    return v;
}
/* Poll cond every ms for up to 2 s; FAudio calls back on its own thread. */
#define WAIT_FOR(cond)                                     \
    do {                                                   \
        for (int w_ = 0; w_ < 2000 && !(cond); w_++)       \
            usleep(1000);                                  \
    } while (0)

/* ---- X1: engine, mastering voice ------------------------------------ */

static uint32_t g_xa, g_master, g_rate, g_master_ch;

static void test_engine(void)
{
    /* Factory for both XAudio2 CLSIDs; the game asks for IID_IXAudio2. */
    com_factory_t f = class_factory(&CLSID_XAudio2), fd = class_factory(&CLSID_XAudio2_Debug);
    CHECK(f != NULL && fd != NULL);
    uint32_t dbg = 0;
    CHECK(fd(&IID_IXAudio2, &dbg) == COM_S_OK && dbg != 0);
    CHECK(CALL(method(dbg, XA_Release), dbg) == 0);
    CHECK(com_guest_iface(dbg) == NULL);
    uint32_t none = 1;
    CHECK(f(&IID_Other, &none) == COM_E_NOINTERFACE && none == 0);
    CHECK(f(&IID_IXAudio2, &g_xa) == COM_S_OK && g_xa != 0);
    uint32_t xa = g_xa;

    CHECK(CALL(method(xa, XA_Initialize), xa, 0, 0xFFFFFFFFu) == 0);

    /* CreateMasteringVoice(pp, 0, 0, 0, DeviceIndex 0, NULL) */
    uint32_t pp = guest_alloc(4);
    MEM32(pp) = 0xDEADBEEFu;
    CHECK(CALL(method(xa, XA_CreateMasteringVoice), xa, pp, 0, 0, 0, 0, 0) == 0);
    g_master = MEM32(pp);
    CHECK(com_guest_iface(g_master) != NULL);
    CHECK(strcmp(com_guest_iface(g_master)->name, "IXAudio2MasteringVoice") == 0);
    CHECK(com_guest_iface(g_master)->n_methods == 19);

    /* GetDeviceCount, then GetDeviceDetails(0): dwChannelMask at +0x418. */
    uint32_t cnt = guest_alloc(4);
    CHECK(CALL(method(xa, XA_GetDeviceCount), xa, cnt) == 0);
    CHECK(MEM32(cnt) >= 1);
    uint32_t det = guest_alloc(1068);
    CHECK(CALL(method(xa, XA_GetDeviceDetails), xa, 0, det) == 0);
    CHECK(MEM32(det + 0x418) != 0);
    CHECK(MEM16(det + 1028 + 2) >= 1); /* OutputFormat.nChannels */

    /* master->GetVoiceDetails: InputSampleRate > 0 at +8, nothing past +12. */
    uint32_t vd = guest_alloc(32);
    memset(guest_ptr(vd), 0xAB, 32);
    CHECK(CALL(method(g_master, V_GetVoiceDetails), g_master, vd) == 0);
    g_master_ch = MEM32(vd + 4);
    g_rate = MEM32(vd + 8);
    CHECK(g_master_ch >= 1 && g_rate > 0);
    for (uint32_t i = 12; i < 32; i++)
        CHECK(MEM8(vd + i) == 0xAB);

    /* QueryInterface: IUnknown gives the same object, another IID fails. */
    uint32_t ppv = guest_alloc(4);
    CHECK(CALL(method(xa, XA_QueryInterface), xa, guid_at(&IID_IUnknown), ppv) == COM_S_OK);
    CHECK(MEM32(ppv) == xa);
    CHECK(CALL(method(xa, XA_QueryInterface), xa, guid_at(&IID_IXAudio2), ppv) == COM_S_OK);
    CHECK(MEM32(ppv) == xa);
    CHECK(CALL(method(xa, XA_QueryInterface), xa, guid_at(&IID_Other), ppv) ==
          COM_E_NOINTERFACE);
    CHECK(MEM32(ppv) == 0);
    CHECK(CALL(method(xa, XA_AddRef), xa) == 4);
    CHECK(CALL(method(xa, XA_Release), xa) == 3);
    CHECK(CALL(method(xa, XA_Release), xa) == 2);
    CHECK(CALL(method(xa, XA_Release), xa) == 1);

    /* Engine callbacks arrive on the mixer thread, after the attach. */
    static const recomp_func_t ecb[] = {ecb0, ecb1, ecb2};
    g_ecb_obj = guest_object(ecb, 3);
    CHECK(CALL(method(xa, XA_RegisterForCallbacks), xa, g_ecb_obj) == 0);
    WAIT_FOR(counter(&g_epass[0]) > 2 && counter(&g_epass[1]) > 2);
    CHECK(counter(&g_epass[0]) > 2 && counter(&g_epass[1]) > 2);
    CHECK(CALL(method(xa, XA_UnregisterForCallbacks), xa, g_ecb_obj) == 0);

    /* The other engine methods. */
    CHECK(CALL(method(xa, XA_StopEngine), xa) == 0);
    CHECK(CALL(method(xa, XA_StartEngine), xa) == 0);
    CHECK(CALL(method(xa, XA_CommitChanges), xa, 1) == 0);
    uint32_t perf = guest_alloc(64);
    memset(guest_ptr(perf), 0xAB, 64);
    CHECK(CALL(method(xa, XA_GetPerformanceData), xa, perf) == 0);
    int changed = 0;
    for (uint32_t i = 0; i < 64; i++)
        changed |= MEM8(perf + i) != 0xAB;
    CHECK(changed);
    uint32_t dcfg = guest_alloc(24);
    CHECK(CALL(method(xa, XA_SetDebugConfiguration), xa, dcfg, 0) == 0);
}

/* ---- X2: submix, reverb, voice methods ------------------------------- */

static uint32_t g_submix;

static void test_submix(void)
{
    uint32_t xa = g_xa, pp = guest_alloc(4);
    /* CreateSubmixVoice(pp, 1, rate, 0, 1, NULL, NULL) */
    CHECK(CALL(method(xa, XA_CreateSubmixVoice), xa, pp, 1, g_rate, 0, 1, 0, 0) == 0);
    uint32_t sm = g_submix = MEM32(pp);
    CHECK(com_guest_iface(sm) != NULL && com_guest_iface(sm)->n_methods == 19);

    /* AudioReverb through both factories with IID_IUnknown. */
    com_factory_t rf = class_factory(&CLSID_AudioReverb);
    com_factory_t rfd = class_factory(&CLSID_AudioReverb_Debug);
    CHECK(rf != NULL && rfd != NULL);
    uint32_t rv = 0, rvd = 0, none = 1;
    CHECK(rf(&IID_IXAudio2, &none) == COM_E_NOINTERFACE && none == 0);
    CHECK(rfd(&IID_IUnknown, &rvd) == COM_S_OK && rvd != 0);
    CHECK(CALL(method(rvd, 2), rvd) == 0);
    CHECK(rf(&IID_IUnknown, &rv) == COM_S_OK && rv != 0);
    CHECK(com_guest_iface(rv)->n_methods == 3);
    uint32_t ppv = guest_alloc(4);
    CHECK(CALL(method(rv, 0), rv, guid_at(&IID_IUnknown), ppv) == COM_S_OK && MEM32(ppv) == rv);
    CHECK(CALL(method(rv, 0), rv, guid_at(&IID_Other), ppv) == COM_E_NOINTERFACE &&
          MEM32(ppv) == 0);
    CHECK(CALL(method(rv, 1), rv) == 3);
    CHECK(CALL(method(rv, 2), rv) == 2);
    CHECK(CALL(method(rv, 2), rv) == 1);

    /* SetEffectChain({1, {reverb, TRUE, 1}}) */
    uint32_t desc = guest_alloc(12), chain = guest_alloc(8);
    MEM32(desc + 0) = rv;
    MEM32(desc + 4) = 1;
    MEM32(desc + 8) = 1;
    MEM32(chain + 0) = 1;
    MEM32(chain + 4) = desc;
    CHECK(CALL(method(sm, V_SetEffectChain), sm, chain) == 0);
    /* The game may release its reference; the voice keeps the effect. */
    CHECK(CALL(method(rv, 2), rv) == 0);
    CHECK(com_guest_iface(rv) == NULL);

    /* SetVolume / GetVolume */
    uint32_t vol = guest_alloc(4);
    CHECK(CALL(method(sm, V_SetVolume), sm, fbits(0.25f), 0) == 0);
    CHECK(CALL(method(sm, V_GetVolume), sm, vol) == 0);
    CHECK(gfloat(vol) == 0.25f);
    CHECK(CALL(method(sm, V_SetVolume), sm, fbits(1.0f), 0) == 0);
    CHECK(CALL(method(sm, V_GetVolume), sm, vol) == 0);
    CHECK(gfloat(vol) == 1.0f);

    /* SetEffectParameters(0, 52-byte reverb params): FAudio applies them on
     * the next mixing pass, so GetEffectParameters is polled. */
    uint32_t prm = guest_alloc(52), got = guest_alloc(52);
    float wet = 42.0f, room[8] = {5000.0f, 0.0f, 0.0f, -10.0f, -10.0f, 1.49f, 100.0f, 100.0f};
    memcpy(guest_ptr(prm), &wet, 4);
    MEM32(prm + 4) = 5;
    static const uint8_t bytes[12] = {5, 5, 6, 6, 27, 27, 8, 8, 8, 4, 0, 4};
    memcpy(guest_ptr(prm + 8), bytes, 12);
    memcpy(guest_ptr(prm + 20), room, 32);
    CHECK(CALL(method(sm, V_SetEffectParameters), sm, 0, prm, 52, 0) == 0);
    WAIT_FOR((CALL(method(sm, V_GetEffectParameters), sm, 0, got, 52) == 0 &&
              memcmp(guest_ptr(got), guest_ptr(prm), 52) == 0));
    CHECK(memcmp(guest_ptr(got), guest_ptr(prm), 52) == 0);
    CHECK(CALL(method(sm, V_SetEffectParameters), sm, 0, prm, 48, 0) == COM_E_INVALIDARG);
    CHECK(CALL(method(sm, V_GetEffectParameters), sm, 0, got, 56) == COM_E_INVALIDARG);

    /* Enable / Disable / GetEffectState */
    uint32_t on = guest_alloc(4);
    CHECK(CALL(method(sm, V_DisableEffect), sm, 0, 0) == 0);
    CHECK(CALL(method(sm, V_GetEffectState), sm, 0, on) == 0);
    CHECK(MEM32(on) == 0);
    CHECK(CALL(method(sm, V_EnableEffect), sm, 0, 0) == 0);
    CHECK(CALL(method(sm, V_GetEffectState), sm, 0, on) == 0);
    CHECK(MEM32(on) == 1);

    /* SetOutputVoices({1, {0, master}}) */
    uint32_t send = guest_alloc(8), sends = guest_alloc(8);
    MEM32(send + 0) = 0;
    MEM32(send + 4) = g_master;
    MEM32(sends + 0) = 1;
    MEM32(sends + 4) = send;
    CHECK(CALL(method(sm, V_SetOutputVoices), sm, sends) == 0);

    /* Channel volumes (1 input channel) and the output matrix to master. */
    uint32_t cv = guest_alloc(4), cvg = guest_alloc(4);
    memcpy(guest_ptr(cv), &(float){0.5f}, 4);
    CHECK(CALL(method(sm, V_SetChannelVolumes), sm, 1, cv, 0) == 0);
    CHECK(CALL(method(sm, V_GetChannelVolumes), sm, 1, cvg) == 0);
    CHECK(gfloat(cvg) == 0.5f);
    uint32_t mx = guest_alloc(4u * g_master_ch), mxg = guest_alloc(4u * g_master_ch);
    for (uint32_t i = 0; i < g_master_ch; i++)
        memcpy(guest_ptr(mx + 4u * i), &(float){0.125f * (float)(i + 1)}, 4);
    CHECK(CALL(method(sm, V_SetOutputMatrix), sm, g_master, 1, g_master_ch, mx, 0) == 0);
    CHECK(CALL(method(sm, V_GetOutputMatrix), sm, g_master, 1, g_master_ch, mxg) == 0);
    CHECK(memcmp(guest_ptr(mx), guest_ptr(mxg), 4u * g_master_ch) == 0);

    /* An effect that is not our AudioReverb aborts with UNSUPPORTED. */
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        quiet_abort_child();
        dup2(fds[1], 2);
        MEM32(desc + 0) = DATA_AREA + 0x100u;
        CALL(method(sm, V_SetEffectChain), sm, chain);
        _exit(0);
    }
    close(fds[1]);
    char out[512];
    ssize_t n = read(fds[0], out, sizeof out - 1);
    out[n > 0 ? n : 0] = 0;
    close(fds[0]);
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(CHILD_ABORTED(st));
    char want[64];
    snprintf(want, sizeof want, "UNSUPPORTED xaudio2: effect 0x%08X", DATA_AREA + 0x100u);
    CHECK(strstr(out, want) != NULL);
}

/* ---- X3: source voice and callbacks ------------------------------------ */

#define FRAMES 4410u
#define CTX1 0x12345678u
#define CTX2 0x0BADF00Du
#define CTX3 0x33333333u

static uint32_t buffer_at(uint32_t data, uint32_t flags, uint32_t loops, uint32_t ctx)
{
    uint32_t b = guest_alloc(36);
    MEM32(b + 0) = flags;
    MEM32(b + 4) = 2u * FRAMES;
    MEM32(b + 8) = data;
    MEM32(b + 28) = loops;
    MEM32(b + 32) = ctx;
    return b;
}

static void test_source(void)
{
    uint32_t xa = g_xa;
    /* 16-bit mono PCM, 44100 Hz, 4410 frames of a quiet sine. */
    uint32_t data = guest_alloc(2u * FRAMES);
    for (uint32_t i = 0; i < FRAMES; i++)
        MEM16(data + 2u * i) = (uint16_t)(int16_t)(1000.0 * sin(i * 0.0627));
    uint32_t fmt = guest_alloc(18);
    MEM16(fmt + 0) = 1;
    MEM16(fmt + 2) = 1;
    MEM32(fmt + 4) = 44100;
    MEM32(fmt + 8) = 88200;
    MEM16(fmt + 12) = 2;
    MEM16(fmt + 14) = 16;
    MEM16(fmt + 16) = 0;

    static const recomp_func_t vcb[] = {vcb0, vcb1, vcb2, vcb3, vcb4, vcb5, vcb6};
    g_vcb_obj = guest_object(vcb, 7);

    /* CreateSourceVoice(pp, fmt, USEFILTER, 2.0f, cb, {1, {USEFILTER, master}}, NULL) */
    uint32_t send = guest_alloc(8), sends = guest_alloc(8), pp = guest_alloc(4);
    MEM32(send + 0) = 0x80;
    MEM32(send + 4) = g_master;
    MEM32(sends + 0) = 1;
    MEM32(sends + 4) = send;
    CHECK(CALL(method(xa, XA_CreateSourceVoice), xa, pp, fmt, 0x8, fbits(2.0f), g_vcb_obj,
               sends, 0) == 0);
    uint32_t sv = MEM32(pp);
    CHECK(com_guest_iface(sv) != NULL && com_guest_iface(sv)->n_methods == 29);

    /* FAudio copied the format: overwrite the guest copy, the voice is fine. */
    memset(guest_ptr(fmt), 0xFF, 18);
    uint32_t vd = guest_alloc(12);
    CHECK(CALL(method(sv, V_GetVoiceDetails), sv, vd) == 0);
    CHECK(MEM32(vd + 0) == 0x8 && MEM32(vd + 4) == 1 && MEM32(vd + 8) == 44100);

    /* Filter on the voice and on its send. */
    uint32_t fp = guest_alloc(12), fpg = guest_alloc(12);
    MEM32(fp + 0) = 0; /* LowPassFilter */
    memcpy(guest_ptr(fp + 4), &(float){0.5f}, 4);
    memcpy(guest_ptr(fp + 8), &(float){1.0f}, 4);
    CHECK(CALL(method(sv, V_SetFilterParameters), sv, fp, 0) == 0);
    CHECK(CALL(method(sv, V_GetFilterParameters), sv, fpg) == 0);
    CHECK(memcmp(guest_ptr(fp), guest_ptr(fpg), 12) == 0);
    memcpy(guest_ptr(fp + 4), &(float){0.75f}, 4);
    CHECK(CALL(method(sv, V_SetOutputFilterParameters), sv, g_master, fp, 0) == 0);
    CHECK(CALL(method(sv, V_GetOutputFilterParameters), sv, g_master, fpg) == 0);
    CHECK(memcmp(guest_ptr(fp), guest_ptr(fpg), 12) == 0);
    memcpy(guest_ptr(fp + 4), &(float){1.0f}, 4);
    CHECK(CALL(method(sv, V_SetFilterParameters), sv, fp, 0) == 0);
    CHECK(CALL(method(sv, V_SetOutputFilterParameters), sv, g_master, fp, 0) == 0);

    /* Buffer 1: END_OF_STREAM, pContext 0x12345678. */
    uint32_t b1 = buffer_at(data, 0x40, 0, CTX1);
    CHECK(CALL(method(sv, S_SubmitSourceBuffer), sv, b1, 0) == 0);
    uint32_t vs = guest_alloc(16);
    CHECK(CALL(method(sv, S_GetState), sv, vs) == 0);
    CHECK(MEM32(vs + 0) == CTX1 && MEM32(vs + 4) == 1);

    CHECK(CALL(method(sv, S_Start), sv, 0, 0) == 0);
    WAIT_FOR(find_ev(2, 0, 0) >= 0);
    int bs = find_ev(3, CTX1, 0), be = find_ev(4, CTX1, 0), se = find_ev(2, 0, 0);
    CHECK(bs >= 0 && be > bs && se > be);
    CHECK(counter(&g_vpass[0]) > 0 && counter(&g_vpass[1]) > 0);
    CHECK(counter(&g_cb_on_main) == 0);
    CHECK(counter(&g_cb_unattached) == 0);
    CHECK(counter(&g_cb_bad_fpu) == 0);
    CHECK(counter(&g_cb_bad_this) == 0);

    /* As in XAudio2 before 2.8, SamplesPlayed restarts at 0 when an
     * END_OF_STREAM buffer ends (FAudio_internal.c sets totalSamples = 0). */
    uint64_t played;
    CHECK(CALL(method(sv, S_GetState), sv, vs) == 0);
    memcpy(&played, guest_ptr(vs + 8), 8);
    CHECK(played == 0);
    CHECK(MEM32(vs + 4) == 0 && MEM32(vs + 0) == 0);

    /* Buffer 3 has no END_OF_STREAM: after it, SamplesPlayed counts it. */
    int mark = g_nev;
    uint32_t b3 = buffer_at(data, 0, 0, CTX3);
    CHECK(CALL(method(sv, S_SubmitSourceBuffer), sv, b3, 0) == 0);
    WAIT_FOR(find_ev(4, CTX3, mark) >= 0);
    CHECK(find_ev(3, CTX3, mark) >= 0 && find_ev(4, CTX3, mark) >= 0);
    CHECK(CALL(method(sv, S_GetState), sv, vs) == 0);
    memcpy(&played, guest_ptr(vs + 8), 8);
    CHECK(played >= FRAMES);
    CHECK(MEM32(vs + 4) == 0 && MEM32(vs + 0) == 0);

    /* Buffer 2 loops once: OnLoopEnd, OnBufferEnd and OnStreamEnd with its context. */
    mark = g_nev;
    uint32_t b2 = buffer_at(data, 0x40, 1, CTX2);
    CHECK(CALL(method(sv, S_SubmitSourceBuffer), sv, b2, 0) == 0);
    WAIT_FOR(find_ev(2, 0, mark) >= 0);
    int le = find_ev(5, CTX2, mark);
    be = find_ev(4, CTX2, mark);
    se = find_ev(2, 0, mark);
    CHECK(le >= 0 && be > le && se > be);
    CHECK(find_ev(6, ANY, 0) < 0 && counter(&g_cb_on_main) == 0 && counter(&g_cb_unattached) == 0);

    /* Frequency ratio. */
    uint32_t fr = guest_alloc(4);
    CHECK(CALL(method(sv, S_SetFrequencyRatio), sv, fbits(0.5f), 0) == 0);
    CHECK(CALL(method(sv, S_GetFrequencyRatio), sv, fr) == 0);
    CHECK(gfloat(fr) == 0.5f);

    /* The remaining source methods, on an empty queue. */
    CHECK(CALL(method(sv, S_ExitLoop), sv, 0) == 0);
    CHECK(CALL(method(sv, S_Discontinuity), sv) == 0);
    CHECK(CALL(method(sv, S_FlushSourceBuffers), sv) == 0);
    CHECK(CALL(method(sv, S_Stop), sv, 0, 0) == 0);
    CHECK(CALL(method(sv, S_SetSourceSampleRate), sv, 22050) == 0);
    CHECK(CALL(method(sv, V_GetVoiceDetails), sv, vd) == 0);
    CHECK(MEM32(vd + 8) == 22050);

    /* DestroyVoice while the voice runs: no callback after it returns. */
    CHECK(CALL(method(sv, S_Start), sv, 0, 0) == 0);
    int before = counter(&g_vpass[0]);
    WAIT_FOR(counter(&g_vpass[0]) > before + 2);
    CHECK(counter(&g_vpass[0]) > before + 2);
    int freed = g_freed;
    CHECK(CALL(method(sv, V_DestroyVoice), sv) == 0);
    __atomic_store_n(&g_destroyed, 1, __ATOMIC_RELEASE);
    CHECK(g_freed == freed + 1 && com_guest_iface(sv) == NULL);
    usleep(200000);
    CHECK(counter(&g_cb_after_destroy) == 0);
}

/* ---- X4: the game's source voices ------------------------------- */

/* KSDATAFORMAT_SUBTYPE_PCM as the game stores it. */
static const uint8_t SUBTYPE_PCM[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

/* WAVEFORMATEX as the WAV and OGG FormatData build it, or with tag 0xFFFE
 * the WAVEFORMATEXTENSIBLE of NuFmvChromaXAudio2Voice (notes/assets.md 3). */
static uint32_t game_format(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits)
{
    uint32_t f = guest_alloc(40);
    uint16_t align = (uint16_t)(ch * bits / 8);
    MEM16(f + 0) = tag;
    MEM16(f + 2) = ch;
    MEM32(f + 4) = rate;
    MEM32(f + 8) = rate * align;
    MEM16(f + 12) = align;
    MEM16(f + 14) = bits;
    MEM16(f + 16) = 0;
    if (tag == 0xFFFE) {
        MEM16(f + 16) = 22;
        MEM16(f + 18) = bits;              /* wValidBitsPerSample */
        MEM32(f + 20) = ch == 1 ? 1u : 3u; /* dwChannelMask, as the game's jump table sets it */
        memcpy(guest_ptr(f + 24), SUBTYPE_PCM, 16);
    }
    return f;
}

/* Create a voice as the game does, play one END_OF_STREAM buffer of 50 ms
 * through it while the reverb update runs, and destroy it. sends is the
 * game's {master, submix} list (one site) or 0 (the other, which then
 * sets the output matrix with pDestinationVoice NULL). */
static void play_game_voice(uint32_t fmt, uint32_t sends, uint32_t ctx)
{
    uint32_t xa = g_xa, pp = guest_alloc(4);
    uint32_t ch = MEM16(fmt + 2), rate = MEM32(fmt + 4), align = MEM16(fmt + 12);
    uint32_t frames = rate / 20, bytes = frames * align;
    uint32_t data = guest_alloc(bytes);
    for (uint32_t i = 0; i < bytes; i++)
        MEM8(data + i) = (uint8_t)(i * 7u);
    /* CreateSourceVoice(pp, fmt, 0, 2.0f (a game constant), cb, sends, NULL) */
    CHECK(CALL(method(xa, XA_CreateSourceVoice), xa, pp, fmt, 0, fbits(2.0f), g_vcb_obj, sends,
               0) == 0);
    uint32_t sv = MEM32(pp);
    CHECK(com_guest_iface(sv) != NULL && com_guest_iface(sv)->n_methods == 29);
    uint32_t vd = guest_alloc(12);
    CHECK(CALL(method(sv, V_GetVoiceDetails), sv, vd) == 0);
    CHECK(MEM32(vd + 4) == ch && MEM32(vd + 8) == rate);
    if (sends == 0) {
        uint32_t mx = guest_alloc(4u * ch * g_master_ch), mxg = guest_alloc(4u * ch * g_master_ch);
        for (uint32_t i = 0; i < ch * g_master_ch; i++)
            memcpy(guest_ptr(mx + 4u * i), &(float){i % (ch + 1) == 0 ? 1.0f : 0.0f}, 4);
        CHECK(CALL(method(sv, V_SetOutputMatrix), sv, 0, ch, g_master_ch, mx, 0) == 0);
        /* NULL means the only send, the master. FAudio (as Wine) reads no
         * matrix for NULL in GetOutputMatrix, so read it back by voice. */
        CHECK(CALL(method(sv, V_GetOutputMatrix), sv, g_master, ch, g_master_ch, mxg) == 0);
        CHECK(memcmp(guest_ptr(mx), guest_ptr(mxg), 4u * ch * g_master_ch) == 0);
    }
    uint32_t b = guest_alloc(36);
    memset(guest_ptr(b), 0, 36);
    MEM32(b + 0) = 0x40; /* END_OF_STREAM */
    MEM32(b + 4) = bytes;
    MEM32(b + 8) = data;
    MEM32(b + 32) = ctx;
    int mark = g_nev;
    /* SubmitSourceBuffer(p, NULL): the WAV and OGG slot 5 return 0. */
    CHECK(CALL(method(sv, S_SubmitSourceBuffer), sv, b, 0) == 0);
    CHECK(CALL(method(sv, S_Start), sv, 0, 0) == 0);
    /* The game's reverb update while the voice plays into the submix. */
    uint32_t prm = guest_alloc(52);
    CHECK(CALL(method(g_submix, V_GetEffectParameters), g_submix, 0, prm, 52) == 0);
    for (int i = 0; i < 20 && find_ev(2, 0, mark) < 0; i++) {
        CHECK(CALL(method(g_submix, V_SetVolume), g_submix, fbits(1.0f), 0) == 0);
        CHECK(CALL(method(g_submix, V_SetEffectParameters), g_submix, 0, prm, 0x34, 0) == 0);
        usleep(5000);
    }
    WAIT_FOR(find_ev(2, 0, mark) >= 0);
    int bs = find_ev(3, ctx, mark), be = find_ev(4, ctx, mark), se = find_ev(2, 0, mark);
    CHECK(bs >= 0 && be > bs && se > be);
    uint32_t vs = guest_alloc(16);
    CHECK(CALL(method(sv, S_GetState), sv, vs) == 0);
    CHECK(MEM32(vs + 4) == 0);
    CHECK(CALL(method(sv, V_DestroyVoice), sv) == 0);
    CHECK(com_guest_iface(sv) == NULL);
}

static void test_game_sources(void)
{
    /* The source sends at [audio+0x3924]: {2, {{0, master}, {0, submix}}}. */
    uint32_t send = guest_alloc(16), sends = guest_alloc(8);
    MEM32(send + 0) = 0;
    MEM32(send + 4) = g_master;
    MEM32(send + 8) = 0;
    MEM32(send + 12) = g_submix;
    MEM32(sends + 0) = 2;
    MEM32(sends + 4) = send;
    play_game_voice(game_format(1, 1, 44100, 16), sends, 0x4C410001u); /* WAV effects */
    play_game_voice(game_format(1, 1, 22050, 16), sends, 0x4C410002u);
    play_game_voice(game_format(1, 1, 22050, 8), sends, 0x4C410003u);  /* the 8-bit file */
    play_game_voice(game_format(1, 2, 44100, 16), sends, 0x4C410004u); /* Ogg music */
    play_game_voice(game_format(0xFFFE, 2, 44100, 16), 0, 0x4C410005u); /* movie audio */
    play_game_voice(game_format(0xFFFE, 1, 22050, 16), 0, 0x4C410006u);
    CHECK(find_ev(6, ANY, 0) < 0); /* no OnVoiceError */
    CHECK(counter(&g_cb_on_main) == 0 && counter(&g_cb_unattached) == 0);
    CHECK(counter(&g_cb_bad_fpu) == 0 && counter(&g_cb_bad_this) == 0);
}

int main(void)
{
    g_main = pthread_self();
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(GUEST_LO), GUEST_HI - GUEST_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    CHECK(g_n_classes == 4);
    test_engine();
    test_submix();
    test_source();
    __atomic_store_n(&g_destroyed, 0, __ATOMIC_RELEASE); /* that voice is gone */
    test_game_sources();

    /* Tear down as the game would: voices, then the engine to 0. */
    int freed = g_freed;
    CHECK(CALL(method(g_submix, V_DestroyVoice), g_submix) == 0);
    CHECK(CALL(method(g_master, V_DestroyVoice), g_master) == 0);
    CHECK(CALL(method(g_xa, XA_Release), g_xa) == 0);
    CHECK(g_freed == freed + 3 && com_guest_iface(g_xa) == NULL);

    puts(g_fail ? "test_win32_xaudio2: FAIL" : "test_win32_xaudio2: PASS");
    return g_fail;
}
