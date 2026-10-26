/* D3DX shader slice test: D3DXCompileShader and D3DXGetShaderConstantTable
 * (d3dx_shader.c) on the game's own shaders, through the guest-facing thunks
 * and COM objects. Links d3dx_shader.c, vkd3d-shader and the engine's
 * guest_mem.c, guest_heap.c, dlmalloc.c, host_funcs.c, guest_call.c and
 * recomp_glue.c. The com_guest.h object functions, the ID3DXInclude and the
 * IDirect3DDevice9 are doubles here. Shaders come from
 * tools/pe/extract_shaders.py output in $HP_SHADER_DIR (default
 * orig.nosync/shaders). The shaders are game data and stay out of git: when
 * that directory is absent, the checks that need them are skipped and the
 * test exits 77 (SKIP) after the other checks pass. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "com_guest.h"
#include "win32_thunk.h"
#include "d3dx_guest.h"

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

/* ---- com_guest.h doubles ----------------------------------------------- */

#define MAX_OBJ 4096u
static void *g_obj_self[MAX_OBJ];
static uint32_t g_obj_guest[MAX_OBJ];
static int g_live_objects;
static struct {
    const com_interface_t *iface;
    uint32_t vtbl;
} g_vtbls[8];

uint32_t com_guest_new(const com_interface_t *iface, void *host_self)
{
    uint32_t vtbl = 0;
    for (unsigned i = 0; i < 8 && vtbl == 0; i++) {
        if (g_vtbls[i].iface == NULL) {
            g_vtbls[i].iface = iface;
            g_vtbls[i].vtbl = guest_malloc(4u * iface->n_methods);
            for (uint32_t k = 0; k < iface->n_methods; k++)
                MEM32(g_vtbls[i].vtbl + 4u * k) = host_func_va(iface->methods[k]);
        }
        if (g_vtbls[i].iface == iface)
            vtbl = g_vtbls[i].vtbl;
    }
    uint32_t idx = 1;
    while (idx < MAX_OBJ && g_obj_guest[idx] != 0)
        idx++;
    if (vtbl == 0 || idx == MAX_OBJ)
        abort();
    uint32_t obj = guest_malloc(8);
    MEM32(obj) = vtbl;
    MEM32(obj + 4u) = idx;
    g_obj_self[idx] = host_self;
    g_obj_guest[idx] = obj;
    g_live_objects++;
    return obj;
}

void *com_guest_self(uint32_t guest_this)
{
    uint32_t idx = MEM32(guest_this + 4u);
    if (idx == 0 || idx >= MAX_OBJ || g_obj_guest[idx] != guest_this) {
        fprintf(stderr, "UNSUPPORTED com: bad object 0x%08X\n", guest_this);
        abort();
    }
    return g_obj_self[idx];
}

void com_guest_free(uint32_t guest_this)
{
    uint32_t idx = MEM32(guest_this + 4u);
    CHECK(idx < MAX_OBJ && g_obj_guest[idx] == guest_this);
    g_obj_guest[idx] = 0;
    g_live_objects--;
    guest_free(guest_this);
}

com_guid_t com_guid_read(uint32_t a)
{
    com_guid_t g;
    g.d1 = MEM32(a);
    g.d2 = (uint16_t)MEM16(a + 4u);
    g.d3 = (uint16_t)MEM16(a + 6u);
    for (int i = 0; i < 8; i++)
        g.d4[i] = MEM8(a + 8u + (uint32_t)i);
    return g;
}

int com_guid_eq(const com_guid_t *a, const com_guid_t *b)
{
    return a->d1 == b->d1 && a->d2 == b->d2 && a->d3 == b->d3 && memcmp(a->d4, b->d4, 8) == 0;
}

/* ---- Guest stack and calls --------------------------------------------- */

#define STACK_LO 0x00F00000u
#define STACK_TOP 0x00FF0000u
#define RET_ADDR 0xDEAD0001u

/* Push args right to left and the return address, call fn, check that it
 * popped 4 + 4*nargs bytes and kept ebx/esi/edi/ebp. Returns g_eax. */
static uint32_t call_thunk(recomp_func_t fn, unsigned nargs, const uint32_t *args)
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

/* Call vtable slot of obj with n further arguments. */
static uint32_t call_method(uint32_t obj, uint32_t slot, unsigned n, const uint32_t *more)
{
    uint32_t args[8] = {obj};
    memcpy(args + 1, more, 4u * n);
    recomp_func_t fn = guest_resolve(MEM32(MEM32(obj) + 4u * slot));
    CHECK(fn != NULL);
    return fn != NULL ? call_thunk(fn, n + 1u, args) : 0u;
}
#define M(obj, slot, ...) \
    call_method((obj), (slot), sizeof((uint32_t[]){__VA_ARGS__}) / 4u, (uint32_t[]){__VA_ARGS__})
#define M0(obj, slot) call_method((obj), (slot), 0, NULL)

/* ID3DXBuffer / ID3DXConstantTable vtable slots. */
enum { RELEASE = 2, GET_BUFFER_POINTER = 3, GET_BUFFER_SIZE = 4, GET_DESC = 5,
       GET_CONSTANT_DESC = 6, GET_CONSTANT = 8, GET_CONSTANT_BY_NAME = 9,
       GET_CONSTANT_ELEMENT = 10, SET_FLOAT_ARRAY = 18 };

static uint32_t guest_bytes(const void *p, uint32_t n)
{
    uint32_t a = guest_malloc(n + 1u);
    memcpy(guest_ptr(a), p, n);
    MEM8(a + n) = 0;
    return a;
}
static uint32_t guest_str(const char *s) { return guest_bytes(s, (uint32_t)strlen(s)); }
static const char *host_str(uint32_t a) { return (const char *)guest_ptr(a); }

static char *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = (uint32_t)fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static const char *g_dir;
static char *read_shader(const char *rel, uint32_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, rel);
    char *b = read_file(path, len);
    if (b == NULL)
        fprintf(stderr, "cannot read %s (run tools/pe/extract_shaders.py)\n", path);
    return b;
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
    static char buf[65536];
    fflush(stderr);
    dup2(g_saved_fd, 2);
    close(g_saved_fd);
    rewind(g_cap);
    size_t n = fread(buf, 1, sizeof buf - 1, g_cap);
    buf[n] = '\0';
    fclose(g_cap);
    return buf;
}
static int count_str(const char *text, const char *s)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    return n;
}

/* ---- ID3DXInclude double: records Open and Close ------------------------ */

static const struct {
    const char *name, *file;
} g_include_files[] = {
    {"DeferredFilterShadows.hlsl", "hlsl/deferredFilterShadows.hlsl"},
    {"DeferredFilterCommon.hlsl", "hlsl/deferredFilterCommon.hlsl"},
    {"VertexShaderSemantics.fx", NULL},
};
/* VertexShaderSemantics.fx is not in the exe; these are the names the
 * lighting files read from it (notes/vkd3d_test.md, B3). */
static const char g_semantics_stub[] =
    "uniform float4 viewProj;\nuniform float4 vs_inverse_projection_xy;\n"
    "uniform float4 vs_projection_params;\nuniform float4 vs_viewportScaleBias;\n"
    "uniform float4 world;\n";

struct inc_event {
    int open;
    uint32_t type, parent, data;
    char name[64];
};
static struct inc_event g_inc[16];
static int g_n_inc;

/* HRESULT Open(this, type, name, parent, ppData, pBytes), stdcall. */
static void inc_open(void)
{
    uint32_t type = W32_ARG(1), name = W32_ARG(2), parent = W32_ARG(3);
    uint32_t pp_data = W32_ARG(4), p_bytes = W32_ARG(5), data = 0, n = 0;
    for (size_t i = 0; i < sizeof g_include_files / sizeof g_include_files[0]; i++) {
        if (strcasecmp(host_str(name), g_include_files[i].name) != 0)
            continue;
        char *body = g_include_files[i].file != NULL ? read_shader(g_include_files[i].file, &n)
                                                     : strdup(g_semantics_stub);
        if (g_include_files[i].file == NULL)
            n = (uint32_t)strlen(body);
        data = guest_bytes(body, n);
        free(body);
    }
    if (g_n_inc < 16) {
        struct inc_event *e = &g_inc[g_n_inc++];
        *e = (struct inc_event){1, type, parent, data, ""};
        snprintf(e->name, sizeof e->name, "%s", host_str(name));
    }
    MEM32(pp_data) = data;
    MEM32(p_bytes) = n;
    W32_RETURN(6, data != 0 ? 0u : COM_E_FAIL);
}

/* HRESULT Close(this, pData), stdcall. */
static void inc_close(void)
{
    uint32_t data = W32_ARG(1);
    if (g_n_inc < 16)
        g_inc[g_n_inc++] = (struct inc_event){0, 0, 0, data, ""};
    guest_free(data);
    W32_RETURN(2, 0);
}

static uint32_t make_include(void)
{
    uint32_t vtbl = guest_malloc(8), obj = guest_malloc(4);
    MEM32(vtbl) = host_func_va(inc_open);
    MEM32(vtbl + 4u) = host_func_va(inc_close);
    MEM32(obj) = vtbl;
    return obj;
}

/* ---- IDirect3DDevice9 double: records SetPixelShaderConstantF ------------ */

#define DEVICE_METHODS 119u
struct dev_call {
    uint32_t start, count;
    float v[8];
};
static struct dev_call g_dev[16];
static int g_n_dev, g_dev_other;

static void dev_set_ps_f(void)
{
    uint32_t start = W32_ARG(1), data = W32_ARG(2), count = W32_ARG(3);
    if (g_n_dev < 16) {
        struct dev_call *c = &g_dev[g_n_dev++];
        c->start = start;
        c->count = count;
        for (uint32_t i = 0; i < 4u * count && i < 8; i++)
            memcpy(&c->v[i], guest_ptr(data + 4u * i), 4);
    }
    W32_RETURN(4, 0);
}
static void dev_other(void)
{
    g_dev_other++;
    W32_RETURN(4, 0);
}

static uint32_t make_device(void)
{
    uint32_t vtbl = guest_malloc(4u * DEVICE_METHODS), obj = guest_malloc(4);
    for (uint32_t k = 0; k < DEVICE_METHODS; k++)
        MEM32(vtbl + 4u * k) = host_func_va(dev_other);
    MEM32(vtbl + 0x1B4u) = host_func_va(dev_set_ps_f);
    MEM32(obj) = vtbl;
    return obj;
}

/* ---- Helpers on the guest-facing API ------------------------------------- */

/* D3DXCompileShader through the thunk. out[0..2] = shader, errors, table. */
/* The d3d slice table the resolver searches: the d3dx9_41 entry, which
 * d3dx9_35 imports reach through the resolver's alias. */
const win32_export_t win32_exports_d3d[] = {
    {"d3dx9_41.dll", "D3DXCompileShader", 0, thunk_D3DXCompileShader}, {0}};

static uint32_t compile_with(recomp_func_t fn, const char *src, uint32_t len, uint32_t defines,
                             uint32_t include, const char *entry, const char *profile, uint32_t flags,
                             uint32_t out[3])
{
    uint32_t outs = guest_malloc(12);
    MEM32(outs) = MEM32(outs + 4u) = MEM32(outs + 8u) = 0xBADBADu;
    uint32_t gsrc = guest_bytes(src, len);
    const uint32_t args[10] = {gsrc, len, defines, include, guest_str(entry), guest_str(profile),
                               flags, outs, outs + 4u, outs + 8u};
    uint32_t hr = call_thunk(fn, 10, args);
    for (int i = 0; i < 3; i++)
        out[i] = MEM32(outs + 4u * (uint32_t)i);
    return hr;
}

static uint32_t compile(const char *src, uint32_t len, uint32_t defines, uint32_t include,
                        const char *entry, const char *profile, uint32_t flags, uint32_t out[3])
{
    return compile_with(thunk_D3DXCompileShader, src, len, defines, include, entry, profile, flags,
                        out);
}

static uint32_t get_table(const void *blob, uint32_t len, uint32_t *table)
{
    uint32_t out = guest_malloc(4), code = guest_bytes(blob, len);
    const uint32_t args[2] = {code, out};
    uint32_t hr = call_thunk(thunk_D3DXGetShaderConstantTable, 2, args);
    *table = MEM32(out);
    return hr;
}

struct desc {
    const char *name;
    uint32_t set, index, count, cls, type, rows, cols, elements, members, bytes, defval;
};
static int get_desc(uint32_t table, uint32_t h, struct desc *d)
{
    uint32_t buf = guest_malloc(48), cnt = guest_malloc(4);
    uint32_t hr = M(table, GET_CONSTANT_DESC, h, buf, cnt);
    if (hr == 0) {
        d->name = host_str(MEM32(buf));
        uint32_t *f = &d->set;
        for (uint32_t i = 0; i < 11; i++)
            f[i] = MEM32(buf + 4u + 4u * i);
        CHECK(MEM32(cnt) == 1);
    }
    return hr == 0;
}

static uint32_t n_constants(uint32_t table)
{
    uint32_t desc = guest_malloc(12);
    CHECK(M(table, GET_DESC, desc) == 0);
    return MEM32(desc + 8u);
}

/* ---- Tests --------------------------------------------------------------- */

/* DeferredLightPS through the thunk; its table must match the CTAB of the
 * exe's own compile of the same file. */
static void test_deferred_light(void)
{
    uint32_t len, blob_len;
    char *src = read_shader("hlsl/deferredFilterLighting.hlsl", &len);
    char *blob = read_shader("blobs/ps_3_0_89fc98.bin", &blob_len);
    if (src == NULL || blob == NULL) {
        CHECK(!"shader files");
        return;
    }
    /* One guest define, to exercise the D3DXMACRO walk. */
    uint32_t defs = guest_malloc(16);
    MEM32(defs) = guest_str("D3DX_TEST_DEFINE"), MEM32(defs + 4u) = guest_str("1");
    MEM32(defs + 8u) = 0, MEM32(defs + 12u) = 0;
    uint32_t inc = make_include(), out[3];
    g_n_inc = 0;
    capture_begin();
    uint32_t hr = compile(src, len, defs, inc, "DeferredLightPS", "ps_3_0", 0x200u, out);
    char *log = capture_end();
    CHECK(hr == 0);
    CHECK(count_str(log, "UNSUPPORTED d3dx9: D3DXCompileShader flag 0x200\n") == 1);
    CHECK(out[0] != 0 && out[2] != 0);
    if (hr != 0 || out[0] == 0 || out[2] == 0) {
        fprintf(stderr, "%s", log);
        if (out[1] != 0)
            M0(out[1], RELEASE);
        return;
    }

    uint32_t code = M0(out[0], GET_BUFFER_POINTER), size = M0(out[0], GET_BUFFER_SIZE);
    CHECK(size > 8 && size % 4 == 0 && MEM32(code) == 0xFFFF0300u &&
          MEM32(code + size - 4u) == 0x0000FFFFu);
    printf("DeferredLightPS: ps_3_0 byte code %u bytes, error buffer %s\n", size,
           out[1] != 0 ? "holds warnings" : "none");

    /* The include callback: three nested opens, each closed once afterwards. */
    printf("include events:");
    for (int i = 0; i < g_n_inc; i++)
        printf(" %s(%s)", g_inc[i].open ? "Open" : "Close", g_inc[i].open ? g_inc[i].name : "");
    printf("\n");
    int opens = 0;
    for (int i = 0; i < g_n_inc; i++) {
        if (!g_inc[i].open)
            continue;
        CHECK(g_inc[i].type == 0); /* D3DXINC_LOCAL for "..." */
        CHECK(opens < 3 && strcmp(g_inc[i].name, g_include_files[opens].name) == 0);
        /* The main file's parent is NULL; a nested one's is the includer's data. */
        CHECK(g_inc[i].parent == (opens == 0 ? 0u : g_inc[i - 1].data));
        int closes = 0;
        for (int j = 0; j < g_n_inc; j++)
            if (!g_inc[j].open && g_inc[j].data == g_inc[i].data) {
                closes++;
                CHECK(j > i);
            }
        CHECK(closes == 1);
        opens++;
    }
    CHECK(opens == 3 && g_n_inc == 6);

    /* The same flag again prints nothing. */
    capture_begin();
    uint32_t out2[3];
    compile("float4 main() : COLOR { return 0; }", 35, 0, 0, "main", "ps_2_0", 0x200u, out2);
    CHECK(count_str(capture_end(), "UNSUPPORTED") == 0);
    CHECK(out2[0] != 0 && M0(out2[0], RELEASE) == 0);
    CHECK(out2[2] != 0 && M0(out2[2], RELEASE) == 0);
    if (out2[1] != 0)
        CHECK(M0(out2[1], RELEASE) == 0);

    uint32_t shipped;
    CHECK(get_table(blob, blob_len, &shipped) == 0 && shipped != 0);
    uint32_t n = n_constants(shipped);
    CHECK(n == 3);
    printf("constant     shipped blob     vkd3d compile\n");
    for (uint32_t i = 0; i < n; i++) {
        struct desc a = {0}, b = {0};
        uint32_t ha = M(shipped, GET_CONSTANT, 0, i);
        CHECK(get_desc(shipped, ha, &a));
        uint32_t name = guest_str(a.name);
        uint32_t hb = M(out[2], GET_CONSTANT_BY_NAME, 0, name);
        CHECK(hb != 0 && get_desc(out[2], hb, &b));
        static const char sets[] = "bics";
        printf("%-16s %c%-3u x%u          %c%-3u x%u\n", a.name, sets[a.set & 3], a.index, a.count,
               sets[b.set & 3], b.index, b.count);
        CHECK(b.name != NULL && strcmp(a.name, b.name) == 0 && a.set == b.set &&
              a.index == b.index && a.count == b.count);
    }
    struct desc d;
    uint32_t h = M(out[2], GET_CONSTANT_BY_NAME, 0, guest_str("g_Light"));
    CHECK(get_desc(out[2], h, &d) && d.set == 2 && d.index == 180 && d.count == 3);
    h = M(out[2], GET_CONSTANT_BY_NAME, 0, guest_str("g_AlbedoTex"));
    CHECK(get_desc(out[2], h, &d) && d.set == 3 && d.index == 6 && d.count == 1);
    h = M(out[2], GET_CONSTANT_BY_NAME, 0, guest_str("g_SceneDepthTex"));
    CHECK(get_desc(out[2], h, &d) && d.set == 3 && d.index == 0 && d.count == 1);

    CHECK(M0(shipped, RELEASE) == 0);
    CHECK(M0(out[0], RELEASE) == 0);
    CHECK(M0(out[2], RELEASE) == 0);
    if (out[1] != 0)
        CHECK(M0(out[1], RELEASE) == 0);
    free(src);
    free(blob);
}

/* Register limits per shader model (D3D9 caps maxima). */
static uint32_t reg_limit(uint32_t version, uint32_t set)
{
    int vs = (version >> 16) == 0xFFFEu;
    uint32_t major = (version >> 8) & 0xFFu, minor = version & 0xFFu;
    switch (set) {
    case 0: /* bool */
    case 1: /* int4 */
        return 16;
    case 2: /* float4 */
        return vs ? 256u : major == 1 ? 8u : major == 2 ? 32u : 224u;
    case 3: /* sampler */
        return vs ? 4u : major == 1 ? (minor >= 4 ? 6u : 4u) : 16u;
    default:
        return 0;
    }
}

static void test_shipped_blobs(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/blobs", g_dir);
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    if (dir == NULL)
        return;
    int blobs = 0, ok = 0, constants = 0, in_range = 0;
    for (struct dirent *e; (e = readdir(dir)) != NULL;) {
        if (strstr(e->d_name, ".bin") == NULL)
            continue;
        char rel[512];
        uint32_t len, table;
        snprintf(rel, sizeof rel, "blobs/%s", e->d_name);
        char *blob = read_shader(rel, &len);
        blobs++;
        uint32_t version;
        memcpy(&version, blob, 4);
        if (get_table(blob, len, &table) == 0 && table != 0) {
            ok++;
            uint32_t n = n_constants(table);
            int good = 1;
            for (uint32_t i = 0; i < n; i++) {
                struct desc d;
                constants++;
                if (get_desc(table, M(table, GET_CONSTANT, 0, i), &d) && d.name[0] != '\0' &&
                    d.index + d.count <= reg_limit(version, d.set))
                    in_range++;
                else
                    good = 0, fprintf(stderr, "%s: constant %u out of range\n", e->d_name, i);
            }
            CHECK(good);
            CHECK(M0(table, RELEASE) == 0);
        } else {
            fprintf(stderr, "%s: D3DXGetShaderConstantTable failed\n", e->d_name);
        }
        free(blob);
    }
    closedir(dir);
    printf("shipped blobs: %d read, %d tables, %d constants, %d inside register limits\n", blobs,
           ok, constants, in_range);
    CHECK(blobs == 451 && ok == 451 && in_range == constants && constants > 0);
}

static void test_errors(void)
{
    static const char bad[] = "float4 main() : COLOR { return 1 }";
    uint32_t out[3];
    capture_begin();
    uint32_t hr = compile(bad, sizeof bad - 1, 0, 0, "main", "ps_2_0", 0, out);
    capture_end();
    CHECK(hr == D3DXERR_INVALIDDATA && hr == 0x88760B59u);
    CHECK(out[0] == 0 && out[2] == 0 && out[1] != 0);
    if (out[1] != 0) {
        const char *text = host_str(M0(out[1], GET_BUFFER_POINTER));
        uint32_t n = M0(out[1], GET_BUFFER_SIZE);
        printf("syntax error: hr=0x%08X, error buffer %u bytes: %.*s", hr, n, (int)n, text);
        CHECK(strstr(text, "E5000") != NULL && strstr(text, "syntax error") != NULL);
        CHECK(M0(out[1], RELEASE) == 0);
    }

    /* half globals compile (as float), as in D3DX. */
    static const char halfs[] = "half4 g_H4; half g_H;\n"
                                "float4 main() : COLOR { return g_H4 * g_H; }";
    hr = compile(halfs, sizeof halfs - 1, 0, 0, "main", "ps_2_0", 0, out);
    CHECK(hr == 0 && out[0] != 0 && out[2] != 0);
    for (int i = 0; i < 3; i++)
        if (out[i] != 0)
            CHECK(M0(out[i], RELEASE) == 0);

    /* From memory with no ID3DXInclude, an #include fails. */
    static const char inc[] = "#include \"fog.hlsl\"\nfloat4 main() : COLOR { return 0; }";
    capture_begin();
    hr = compile(inc, sizeof inc - 1, 0, 0, "main", "ps_2_0", 0, out);
    capture_end();
    CHECK(hr == D3DXERR_INVALIDDATA && out[0] == 0);
    if (out[1] != 0)
        CHECK(M0(out[1], RELEASE) == 0);

    /* No byte code: D3DERR_INVALIDCALL; not byte code: S_OK and no table. */
    uint32_t pp = guest_malloc(4);
    const uint32_t none[2] = {0, pp}, text[2] = {guest_str("text"), pp};
    CHECK(call_thunk(thunk_D3DXGetShaderConstantTable, 2, none) == D3DERR_INVALIDCALL);
    CHECK(call_thunk(thunk_D3DXGetShaderConstantTable, 2, text) == 0 && MEM32(pp) == 0);
}

/* A child process, as a game is, binds D3DXCompileShader from dll and
 * compiles a vs_3_0 shader that assigns to a global; then binds from also
 * (NULL: none). D3DX before SDK version 37 lets a shader write a global, so
 * it compiles from d3dx9_35 (LEGO Indiana Jones); from d3dx9_41 (LEGO Harry
 * Potter) globals are const: E5003. The first d3dx9 DLL bound decides.
 * D3DXSHADER_AVOID_FLOW_CONTROL (0x200, Indy) is a hint, not UNSUPPORTED. */
static void sdk_version_case(const char *dll, const char *also, int compiles)
{
    static const char src[] = "float4 g_Pos;\n"
                              "float4 main(float4 p : POSITION) : POSITION\n"
                              "{ g_Pos = g_Pos * p; return g_Pos; }";
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        recomp_func_t fn = guest_resolve(win32_resolve_import(dll, "D3DXCompileShader", 0, 0));
        CHECK(fn != NULL);
        if (also != NULL)
            CHECK(win32_resolve_import(also, "D3DXCompileShader", 0, 0) != 0);
        uint32_t out[3];
        capture_begin();
        uint32_t hr = fn != NULL ? compile_with(fn, src, sizeof src - 1, 0, 0, "main", "vs_3_0", 0x200u, out)
                                 : 0xFFFFFFFFu;
        CHECK(strstr(capture_end(), "UNSUPPORTED") == NULL);
        printf("%s%s%s: hr=0x%08X\n", dll, also != NULL ? " then " : "", also != NULL ? also : "", hr);
        if (compiles) {
            CHECK(hr == 0 && out[0] != 0 && out[2] != 0);
        } else {
            CHECK(hr == D3DXERR_INVALIDDATA && out[0] == 0 && out[1] != 0);
            if (out[1] != 0)
                CHECK(strstr(host_str(M0(out[1], GET_BUFFER_POINTER)),
                             "E5003: Statement modifies a const expression.") != NULL);
        }
        for (int i = 0; i < 3; i++)
            if (out[i] != 0 && out[i] != 0xBADBADu)
                CHECK(M0(out[i], RELEASE) == 0);
        fflush(stdout);
        _exit(g_fail);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_sdk_version(void)
{
    /* 35 first: its compile is in the shader cache when the 41 case runs. */
    sdk_version_case("D3DX9_35.DLL", NULL, 1);
    sdk_version_case("d3dx9_41.dll", NULL, 0);
    sdk_version_case("d3dx9_35.dll", "d3dx9_41.dll", 1);
}

/* SetFloatArray on a float4[2] at c5 reaches the device, one register each. */
static void test_set_float_array(void)
{
    static const char src[] =
        "uniform float4 g_V[2] : register(c5);\n"
        "float4 main() : COLOR { return g_V[0] + g_V[1]; }\n";
    uint32_t out[3];
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "ps_3_0", 0, out) == 0 && out[2] != 0);
    if (out[2] == 0)
        return;
    uint32_t table = out[2], dev = make_device();
    const double want[8] = {1.0, 2.0, 3.0, 4.0, -0.5, 0.25, 8.0, 16.0};
    float vals[8];
    for (int i = 0; i < 8; i++)
        vals[i] = (float)want[i];
    uint32_t data = guest_bytes(vals, sizeof vals);

    /* The handle as a guest char* name, then as a table handle. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t h = pass == 0 ? guest_str("g_V") : M(table, GET_CONSTANT_BY_NAME, 0, guest_str("g_V"));
        g_n_dev = g_dev_other = 0;
        CHECK(M(table, SET_FLOAT_ARRAY, dev, h, data, 8) == 0);
        CHECK(g_n_dev == 2 && g_dev_other == 0);
        for (int i = 0; i < g_n_dev && i < 2; i++) {
            CHECK(g_dev[i].start == 5u + (uint32_t)i && g_dev[i].count == 1);
            for (int k = 0; k < 4; k++)
                CHECK((double)g_dev[i].v[k] == want[4 * i + k]);
        }
        if (pass == 0)
            printf("SetFloatArray(g_V, 8 floats): SetPixelShaderConstantF(%u, .., %u) and (%u, .., %u)\n",
                   g_dev[0].start, g_dev[0].count, g_dev[1].start, g_dev[1].count);
    }
    struct desc d;
    uint32_t h = M(table, GET_CONSTANT_BY_NAME, 0, guest_str("g_V"));
    CHECK(get_desc(table, M(table, GET_CONSTANT_ELEMENT, h, 1), &d) && d.index == 6 && d.count == 1);
    CHECK(get_desc(table, M(table, GET_CONSTANT_BY_NAME, 0, guest_str("g_V[1]")), &d) &&
          d.index == 6);
    CHECK(M0(out[0], RELEASE) == 0 && M0(table, RELEASE) == 0);
}

/* The disk cache: a second identical compile is a hit with the same bytes;
 * HPRECOMP_SHADER_CACHE=0 compiles again; a different flag is a miss. */
static void test_cache(void)
{
    static const char src[] =
        "uniform float4 g_C : register(c2);\n"
        "float4 main() : COLOR { return g_C * 2; }\n";
    uint32_t a[3], b[3], c[3];
    uint32_t hits = d3dx_shader_cache_hits();
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "ps_3_0", 0x200, a) == 0 && a[0] != 0 && a[2] != 0);
    CHECK(d3dx_shader_cache_hits() == hits);
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "ps_3_0", 0x200, b) == 0 && b[0] != 0 && b[2] != 0);
    CHECK(d3dx_shader_cache_hits() == hits + 1u);
    setenv("HPRECOMP_SHADER_CACHE", "0", 1);
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "ps_3_0", 0x200, c) == 0 && c[0] != 0);
    CHECK(d3dx_shader_cache_hits() == hits + 1u);
    unsetenv("HPRECOMP_SHADER_CACHE");
    if (a[0] != 0 && b[0] != 0 && c[0] != 0) {
        uint32_t na = M0(a[0], GET_BUFFER_SIZE), nb = M0(b[0], GET_BUFFER_SIZE), nc = M0(c[0], GET_BUFFER_SIZE);
        CHECK(na == nb && na == nc);
        CHECK(na == nb && memcmp(guest_ptr(M0(a[0], GET_BUFFER_POINTER)), guest_ptr(M0(b[0], GET_BUFFER_POINTER)), na) == 0);
        CHECK(na == nc && memcmp(guest_ptr(M0(a[0], GET_BUFFER_POINTER)), guest_ptr(M0(c[0], GET_BUFFER_POINTER)), na) == 0);
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != 0)
            M0(a[i], RELEASE);
        if (b[i] != 0)
            M0(b[i], RELEASE);
        if (c[i] != 0)
            M0(c[i], RELEASE);
    }
    uint32_t d[3];
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "ps_3_0", 0, d) == 0 && d[0] != 0);
    CHECK(d3dx_shader_cache_hits() == hits + 1u);
    for (int i = 0; i < 3; i++)
        if (d[i] != 0)
            M0(d[i], RELEASE);
}

/* mul(float4, float4x4) compiles to one dp4 per output component, as fxc
 * does (tools/vkd3d/patches/0001). A mul/add chain rounds differently, so
 * shaders that recompute one position disagree (notes/render_artifacts.md). */
static int count_opcode(const uint8_t *code, uint32_t size, uint32_t op)
{
    int n = 0;
    for (uint32_t i = 4; i + 4 <= size;) {
        uint32_t tok;
        memcpy(&tok, code + i, 4);
        if (tok == 0x0000FFFFu)
            break;
        uint32_t len = (tok & 0xFFFFu) == 0xFFFEu ? (tok >> 16) & 0x7FFFu : (tok >> 24) & 0xFu;
        if ((tok & 0xFFFFu) == op)
            n++;
        i += 4 + 4 * len;
    }
    return n;
}

static void test_mul_dot(void)
{
    static const char src[] =
        "float4x4 m : register(c39);\n"
        "float4 main(float4 p : POSITION) : POSITION { return mul(p, m); }\n";
    uint32_t r[3];
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "vs_3_0", 0x200, r) == 0 && r[0] != 0);
    if (r[0] != 0) {
        const uint8_t *code = guest_ptr(M0(r[0], GET_BUFFER_POINTER));
        uint32_t size = M0(r[0], GET_BUFFER_SIZE);
        CHECK(count_opcode(code, size, 9u /* D3DSIO_DP4 */) == 4);
        CHECK(count_opcode(code, size, 2u /* D3DSIO_ADD */) == 0);
    }
    for (int i = 0; i < 3; i++)
        if (r[i] != 0)
            M0(r[i], RELEASE);
}

/* 4-bone skinning from a relative-indexed float4x4 array (the game's
 * computeVertexSkin4): each column is one c72[a0.x] register, so it compiles
 * inside the 32 vs_3_0 temps with one dp4 per output component. Loading every
 * element separately ran out of registers (E9015) and the character drew with
 * the game's fallback. */
static void test_mul_dot_skin(void)
{
    static const char src[] =
        "uniform float4x4 skinMatrix[24] : register(c72);\n"
        "float4 main(float4 p : POSITION, float4 bi : BLENDINDICES, float4 w : BLENDWEIGHT) : POSITION {\n"
        "  return mul(p, skinMatrix[int(bi.x)]) * w.x + mul(p, skinMatrix[int(bi.y)]) * w.y\n"
        "       + mul(p, skinMatrix[int(bi.z)]) * w.z + mul(p, skinMatrix[int(bi.w)]) * w.w; }\n";
    uint32_t r[3];
    CHECK(compile(src, sizeof src - 1, 0, 0, "main", "vs_3_0", 0x200, r) == 0 && r[0] != 0);
    if (r[0] != 0)
        CHECK(count_opcode(guest_ptr(M0(r[0], GET_BUFFER_POINTER)), M0(r[0], GET_BUFFER_SIZE), 9u) == 16);
    for (int i = 0; i < 3; i++)
        if (r[i] != 0)
            M0(r[i], RELEASE);
}

/* No cache: the check finds none (pe_boot then stamps at exit), and a cache
 * the game creates and the exit stamps is kept by the next check. A missing
 * or stale stamp zeroes the VERSION.DAT version int (the game then wipes its
 * CachedShaders); a current stamp leaves the file alone. */
static void test_game_cache_check(const char *home)
{
    char dir[600], ver[700], stamp[700];
    snprintf(dir, sizeof dir, "%s/save/Users/Player/AppData/Roaming/Pub/Game/CachedShaders", home);
    char save[600];
    snprintf(save, sizeof save, "%s/save", home);
    CHECK(d3dx_game_cache_check(save) == 0);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    CHECK(system(cmd) == 0);
    snprintf(ver, sizeof ver, "%s/VERSION.DAT", dir);
    snprintf(stamp, sizeof stamp, "%s/RECOMP_D3DX.TXT", dir);
    uint8_t v[20] = {0x32, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, got[20] = {0};
    FILE *f = fopen(ver, "wb"); /* the game creates its cache during the run */
    CHECK(f != NULL && fwrite(v, sizeof v, 1, f) == 1);
    if (f != NULL)
        fclose(f);
    d3dx_game_cache_stamp(save); /* clean exit */
    CHECK(d3dx_game_cache_check(save) == 1);
    f = fopen(ver, "rb");
    CHECK(f != NULL && fread(got, sizeof got, 1, f) == 1);
    if (f != NULL)
        fclose(f);
    CHECK(memcmp(got, v, sizeof v) == 0);
    CHECK(unlink(stamp) == 0); /* a cache with no stamp is stale */
    d3dx_game_cache_check(save);
    f = fopen(ver, "rb");
    CHECK(f != NULL && fread(got, sizeof got, 1, f) == 1);
    if (f != NULL)
        fclose(f);
    CHECK(got[0] == 0 && memcmp(got + 4, v + 4, 16) == 0);
    f = fopen(ver, "wb"); /* the game rewrites VERSION.DAT after its wipe */
    CHECK(f != NULL && fwrite(v, sizeof v, 1, f) == 1);
    if (f != NULL)
        fclose(f);
    d3dx_game_cache_check(save);
    f = fopen(ver, "rb");
    CHECK(f != NULL && fread(got, sizeof got, 1, f) == 1);
    if (f != NULL)
        fclose(f);
    CHECK(memcmp(got, v, sizeof v) == 0);
}

/* HPRECOMP_SHADER_DUMP: a good compile writes .hlsl, .json and .bin (the
 * byte code); a failed one writes .hlsl and .json with its error. */
static void test_dump(const char *home)
{
    static const char good[] = "float4 main() : COLOR { return 0.25; }\n";
    static const char bad[] = "float4 main() : COLOR { return undeclared_name; }\n";
    char dir[256];
    snprintf(dir, sizeof dir, "%s/dump", home);
    setenv("HPRECOMP_SHADER_DUMP", dir, 1);
    uint32_t a[3], b[3];
    CHECK(compile(good, sizeof good - 1, 0, 0, "main", "ps_3_0", 0, a) == 0 && a[0] != 0);
    CHECK(compile(bad, sizeof bad - 1, 0, 0, "main", "ps_3_0", 0, b) != 0 && b[1] != 0);
    unsetenv("HPRECOMP_SHADER_DUMP");
    unsigned hlsl = 0, json = 0, bin = 0, bin_ok = 0, json_err = 0;
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext == NULL || ext - e->d_name != 64)
            continue;
        char path[512], text[2048] = {0};
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        size_t n = f != NULL ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f != NULL)
            fclose(f);
        if (strcmp(ext, ".hlsl") == 0)
            hlsl += (n == sizeof good - 1 && memcmp(text, good, n) == 0) || (n == sizeof bad - 1 && memcmp(text, bad, n) == 0);
        else if (strcmp(ext, ".bin") == 0) {
            bin++;
            bin_ok = n == M0(a[0], GET_BUFFER_SIZE) && memcmp(text, guest_ptr(M0(a[0], GET_BUFFER_POINTER)), n) == 0;
        } else if (strcmp(ext, ".json") == 0) {
            json++;
            json_err += strstr(text, "\"profile\": \"ps_3_0\"") != NULL && strstr(text, "undeclared_name") != NULL &&
                        strstr(text, "\"bytecode_length\": 0}") != NULL;
        }
    }
    if (d != NULL)
        closedir(d);
    CHECK(hlsl == 2 && json == 2 && bin == 1 && bin_ok && json_err == 1);
    for (int i = 0; i < 3; i++) {
        if (a[i] != 0)
            M0(a[i], RELEASE);
        if (b[i] != 0)
            M0(b[i], RELEASE);
    }
}

int main(void)
{
    if (guest_mem_init() != 0 || guest_heap_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base; /* as startup.c does */
    if (mprotect(guest_ptr(STACK_LO), 0x00100000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    /* The shader cache lives under $HOME; keep test entries out of the
     * user's cache. */
    char home[] = "/tmp/test_d3dx_shader.XXXXXX";
    if (mkdtemp(home) == NULL || setenv("HOME", home, 1) != 0) {
        perror("mkdtemp");
        return 1;
    }
    g_dir = getenv("HP_SHADER_DIR") != NULL ? getenv("HP_SHADER_DIR") : "orig.nosync/shaders";
    int have_shaders = access(g_dir, F_OK) == 0;
    if (have_shaders) {
        test_deferred_light();
        test_shipped_blobs();
    }
    test_errors();
    test_sdk_version();
    test_set_float_array();
    test_cache();
    test_dump(home);
    test_mul_dot();
    test_mul_dot_skin();
    test_game_cache_check(home);
    CHECK(g_live_objects == 0);
    if (g_fail) {
        printf("test_d3dx_shader: FAILED\n");
        return 1;
    }
    if (!have_shaders) {
        printf("test_d3dx_shader: SKIP game shader checks: %s not found (run "
               "tools/pe/extract_shaders.py <exe> %s, or set HP_SHADER_DIR)\n",
               g_dir, g_dir);
        return 77;
    }
    printf("test_d3dx_shader: all checks passed\n");
    return 0;
}
