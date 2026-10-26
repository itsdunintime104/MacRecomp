/* d3dx9_41 shader imports: D3DXCompileShader on vkd3d-shader,
 * D3DXGetShaderConstantTable on a CTAB parser, and the guest ID3DXBuffer,
 * ID3DXConstantTable and ID3DXInclude objects. Contract:
 * runtime/include/d3dx_guest.h. Struct layouts and vtable orders come from
 * i686 probes of the mingw-w64 d3dx9shader.h, d3dx9core.h and d3d9.h. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <CommonCrypto/CommonDigest.h>
#include <vkd3d_shader.h>

#include "recomp_types.h"
#include "guest_call.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "com_guest.h"
#include "win32_thunk.h"
#include "d3dx_guest.h"

/* ---- Constants from the D3DX and D3D9 headers --------------------------- */

#define D3DXSHADER_DEBUG 0x1u
#define D3DXSHADER_PACKMATRIX_ROWMAJOR 0x8u
#define D3DXSHADER_PACKMATRIX_COLUMNMAJOR 0x10u
#define D3DXSHADER_AVOID_FLOW_CONTROL 0x200u
#define D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY 0x1000u
#define D3DXSHADER_IEEE_STRICTNESS 0x2000u
/* The flags vkd3d-shader has an option for, and the hints that need none
 * (AVOID_FLOW_CONTROL). */
#define D3DXSHADER_MAPPED                                                             \
    (D3DXSHADER_DEBUG | D3DXSHADER_PACKMATRIX_ROWMAJOR | D3DXSHADER_PACKMATRIX_COLUMNMAJOR | \
     D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY | D3DXSHADER_IEEE_STRICTNESS |                \
     D3DXSHADER_AVOID_FLOW_CONTROL)

/* The D3DX SDK version (the d3dx9_NN.dll the game links) is
 * win32_d3dx_sdk_version(); vkd3d-utils keys option defaults on it. */

#define D3DXINC_LOCAL 0u
#define D3DXINC_SYSTEM 1u

enum { PT_VOID = 0, PT_BOOL = 1, PT_INT = 2, PT_FLOAT = 3 };
enum { PC_SCALAR = 0, PC_VECTOR = 1, PC_MATRIX_ROWS = 2, PC_MATRIX_COLUMNS = 3, PC_OBJECT = 4,
       PC_STRUCT = 5 };
enum { RS_BOOL = 0, RS_INT4 = 1, RS_FLOAT4 = 2, RS_SAMPLER = 3 };

/* IDirect3DDevice9 vtable byte offsets. */
#define DEV_SET_VS_CONST_F 0x178u
#define DEV_SET_VS_CONST_I 0x180u
#define DEV_SET_VS_CONST_B 0x188u
#define DEV_SET_PS_CONST_F 0x1B4u
#define DEV_SET_PS_CONST_I 0x1BCu
#define DEV_SET_PS_CONST_B 0x1C4u

#define TOKEN_END 0x0000FFFFu
#define TOKEN_COMMENT 0xFFFEu
#define FOURCC_CTAB 0x42415443u /* 'C','T','A','B' */
#define CTAB_HEADER_SIZE 28u
#define CTAB_CONSTANTINFO_SIZE 20u
#define CTAB_TYPEINFO_SIZE 16u
#define CTAB_MEMBERINFO_SIZE 8u
/* Handles are 1-based node indices. Keeping them below 0x4000 keeps them in
 * the guest's never-mapped first page, so no handle is also a valid name. */
#define CTAB_MAX_NODES 0x3FFFu
#define CTAB_MAX_DEPTH 16u
/* Byte code passed by guest pointer has no length; this bounds the walk. */
#define BYTECODE_MAX 0x100000u
#define NAME_MAX_LEN 1024u

static const com_guid_t IID_IUnknown = {0x00000000, 0x0000, 0x0000,
                                        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t IID_ID3DXBuffer = {0x8BA5FB08, 0x5195, 0x40E2,
                                           {0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02}};
/* For D3DX SDK version < 43, and from 43 on (Wine d3dx9shader.h). */
static const com_guid_t IID_ID3DXConstantTable_42 = {0x9DCA3190, 0x38B9, 0x4FC3,
                                                     {0x92, 0xE3, 0x39, 0xC6, 0xDD, 0xFB, 0x35, 0x8B}};
static const com_guid_t IID_ID3DXConstantTable_43 = {0xAB3C758F, 0x093E, 0x4356,
                                                     {0xB7, 0x62, 0x4D, 0xB1, 0x8F, 0x1B, 0x3A, 0x01}};

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t sub_sat(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }

/* A guest NUL-terminated string as a new host string of at most max bytes. */
static char *guest_strdup(uint32_t a, size_t max)
{
    const char *s = guest_ptr(a);
    size_t n = strnlen(s, max);
    char *out = malloc(n + 1);
    if (out != NULL) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
    return out;
}

/* The guest address of a host pointer into the guest window, or 0. */
static uint32_t guest_addr_of(const void *p)
{
    uintptr_t base = (uintptr_t)g_guest_base, a = (uintptr_t)p;
    if (p == NULL || a < base || a - base > 0xFFFFFFFFu)
        return 0;
    return (uint32_t)(a - base);
}

static uint32_t guest_copy(const void *data, uint32_t size)
{
    uint32_t a = guest_malloc(size != 0 ? size : 1u);
    if (a != 0 && size != 0)
        memcpy(guest_ptr(a), data, size);
    return a;
}

/* QueryInterface(this, riid, ppv) shared by both interfaces. */
static void query_interface(const com_guid_t *own, void (*addref)(uint32_t))
{
    uint32_t self = W32_ARG(0), riid = W32_ARG(1), ppv = W32_ARG(2);
    if (ppv == 0) {
        W32_RETURN(3, COM_E_POINTER);
        return;
    }
    com_guid_t iid = com_guid_read(riid);
    int ok = com_guid_eq(&iid, &IID_IUnknown) || com_guid_eq(&iid, &IID_ID3DXBuffer) ||
             (own != NULL && com_guid_eq(&iid, own));
    if (ok)
        addref(self);
    MEM32(ppv) = ok ? self : 0u;
    W32_RETURN(3, ok ? COM_S_OK : COM_E_NOINTERFACE);
}

/* ---- ID3DXBuffer -------------------------------------------------------- */

struct d3dx_buffer {
    atomic_uint refs;
    uint32_t data; /* guest copy */
    uint32_t size;
};

static void buf_addref_obj(uint32_t self)
{
    struct d3dx_buffer *b = com_guest_self(self);
    atomic_fetch_add(&b->refs, 1u);
}
static void buf_qi(void) { query_interface(NULL, buf_addref_obj); }
static void buf_addref(void)
{
    struct d3dx_buffer *b = com_guest_self(W32_ARG(0));
    W32_RETURN(1, atomic_fetch_add(&b->refs, 1u) + 1u);
}
static void buf_release(void)
{
    uint32_t self = W32_ARG(0);
    struct d3dx_buffer *b = com_guest_self(self);
    uint32_t left = atomic_fetch_sub(&b->refs, 1u) - 1u;
    if (left == 0) {
        guest_free(b->data);
        com_guest_free(self);
        free(b);
    }
    W32_RETURN(1, left);
}
static void buf_get_pointer(void)
{
    struct d3dx_buffer *b = com_guest_self(W32_ARG(0));
    W32_RETURN(1, b->data);
}
static void buf_get_size(void)
{
    struct d3dx_buffer *b = com_guest_self(W32_ARG(0));
    W32_RETURN(1, b->size);
}

static const recomp_func_t g_buffer_methods[] = {buf_qi, buf_addref, buf_release,
                                                 buf_get_pointer, buf_get_size};
static const com_interface_t g_buffer_iface = {"ID3DXBuffer", g_buffer_methods, 5};

uint32_t d3dx_buffer_new(const void *data, uint32_t size)
{
    struct d3dx_buffer *b = calloc(1, sizeof *b);
    uint32_t copy = guest_copy(data, size);
    if (b == NULL || copy == 0) {
        free(b);
        guest_free(copy);
        return 0;
    }
    atomic_init(&b->refs, 1u);
    b->data = copy;
    b->size = size;
    return com_guest_new(&g_buffer_iface, b);
}

/* ---- ID3DXConstantTable: CTAB parsing ------------------------------------ */

/* One constant, array element or struct member. Children (elements, or the
 * members of a non-array struct) are contiguous at first. */
struct ctab_node {
    uint32_t nameoff; /* into the CTAB */
    uint32_t regset, regindex, regcount;
    uint32_t cls, type, rows, cols, elements, members, bytes;
    uint32_t defval; /* guest address, 0 if none */
    uint32_t first, n_children;
};

struct d3dx_ctab {
    atomic_uint refs;
    uint8_t *host;  /* host copy of the CTAB payload, for names */
    uint32_t guest; /* guest copy: GetBufferPointer, Name, DefaultValue */
    uint32_t size;
    uint32_t creator; /* guest address, 0 if none */
    uint32_t version; /* the CTAB Version field (GetDesc) */
    int vertex;       /* from the byte code's version token */
    uint32_t n_top;
    uint32_t n_nodes;
    struct ctab_node *nodes;
};

struct ctab_parse {
    const uint8_t *p;
    uint32_t size, guest;
    struct ctab_node *nodes;
    uint32_t n, cap;
};

/* 1 if off starts a NUL-terminated string inside the CTAB. */
static int ctab_str_ok(const struct ctab_parse *c, uint32_t off)
{
    return off < c->size && memchr(c->p + off, 0, c->size - off) != NULL;
}

static int nodes_alloc(struct ctab_parse *c, uint32_t count, uint32_t *first)
{
    if (count > CTAB_MAX_NODES - c->n)
        return -1;
    if (c->n + count > c->cap) {
        uint32_t cap = c->cap != 0 ? c->cap : 16u;
        while (cap < c->n + count)
            cap *= 2u;
        struct ctab_node *n = realloc(c->nodes, cap * sizeof *n);
        if (n == NULL)
            return -1;
        c->nodes = n;
        c->cap = cap;
    }
    memset(&c->nodes[c->n], 0, count * sizeof *c->nodes);
    *first = c->n;
    c->n += count;
    return 0;
}

/* Fill node idx from the type at typeoff, recursing into elements and members.
 * Register counts, default-value offsets and byte sizes follow the reference
 * D3DX rules: an element or member starts where the previous one ended, and a
 * leaf advances the default-value cursor by whole registers. */
static int parse_type(struct ctab_parse *c, uint32_t idx, uint32_t typeoff, int is_element,
                      uint32_t index, uint32_t max_index, uint32_t *defoff, uint32_t nameoff,
                      uint32_t regset, uint32_t depth)
{
    if (depth > CTAB_MAX_DEPTH || typeoff > c->size || c->size - typeoff < CTAB_TYPEINFO_SIZE ||
        !ctab_str_ok(c, nameoff))
        return -1;
    const uint8_t *t = c->p + typeoff;
    uint32_t cls = rd16(t), rows = rd16(t + 4), cols = rd16(t + 6), elements = rd16(t + 8);
    uint32_t members = rd16(t + 10), memberinfo = rd32(t + 12);
    struct ctab_node *n = &c->nodes[idx];
    n->nameoff = nameoff;
    n->regset = regset;
    n->regindex = index & 0xFFFFu;
    n->cls = cls;
    n->type = rd16(t + 2);
    n->rows = rows;
    n->cols = cols;
    n->elements = is_element ? 1u : elements;
    n->members = members;
    n->defval = defoff != NULL ? c->guest + *defoff : 0u;

    uint32_t count = 0, size = 0;
    int by_member = 0;
    if (elements > 1 && !is_element) {
        count = elements;
    } else if (cls == PC_STRUCT && members != 0) {
        if (memberinfo > c->size || (c->size - memberinfo) / CTAB_MEMBERINFO_SIZE < members)
            return -1;
        count = members;
        by_member = 1;
    }

    if (count != 0) {
        uint32_t first;
        if (nodes_alloc(c, count, &first) != 0)
            return -1;
        c->nodes[idx].first = first;
        c->nodes[idx].n_children = count;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *m = c->p + memberinfo + CTAB_MEMBERINFO_SIZE * i;
            uint32_t ctype = by_member ? rd32(m + 4) : typeoff;
            uint32_t cname = by_member ? rd32(m) : nameoff;
            if (parse_type(c, first + i, ctype, !by_member, (index + size) & 0xFFFFu, max_index,
                           defoff, cname, regset, depth + 1) != 0)
                return -1;
            size += c->nodes[first + i].regcount;
        }
    } else {
        uint32_t step = cols * rows; /* default-value words this leaf covers */
        size = cols * rows;
        if (regset == RS_FLOAT4 || regset == RS_INT4) {
            switch (cls) {
            case PC_VECTOR:
                size = 1;
                step = rows * 4u;
                break;
            case PC_SCALAR:
                step = rows * 4u;
                break;
            case PC_MATRIX_ROWS:
                step = rows * 4u;
                size = rows;
                break;
            case PC_MATRIX_COLUMNS:
                step = cols * 4u;
                size = cols;
                break;
            default:
                break;
            }
        } else if (regset == RS_SAMPLER) {
            size = 1;
        }
        if (defoff != NULL) {
            if (*defoff > c->size || (c->size - *defoff) / 4u < step)
                return -1;
            *defoff += step * 4u;
        }
    }

    n = &c->nodes[idx];
    int64_t avail = (int64_t)max_index - (int64_t)(index & 0xFFFFu);
    n->regcount = avail < 0 ? 0u : (uint32_t)(avail < size ? avail : size);
    n->bytes = 4u * n->elements * rows * cols;
    return 0;
}

static const recomp_func_t g_ctab_methods[27];
static const com_interface_t g_ctab_iface = {"ID3DXConstantTable", g_ctab_methods, 27};

/* Find the CTAB comment in byte code of at most max bytes and build a guest
 * ID3DXConstantTable from it. Returns an HRESULT; *out is the object. */
static uint32_t ctab_create(const uint8_t *code, uint64_t max, uint32_t *out)
{
    *out = 0;
    if (max < 4)
        return D3DXERR_INVALIDDATA;
    uint32_t version = rd32(code), kind = version >> 16;
    if (kind != 0x4658 && kind != 0x5458 && kind != 0x7FFE && kind != 0x7FFF && kind != 0xFFFE &&
        kind != 0xFFFF)
        return D3DXERR_INVALIDDATA;

    const uint8_t *data = NULL;
    uint32_t size = 0;
    for (uint64_t pos = 4; pos + 4 <= max;) {
        uint32_t tok = rd32(code + pos);
        if (tok == TOKEN_END)
            break;
        if ((tok & 0xFFFFu) != TOKEN_COMMENT) {
            pos += 4;
            continue;
        }
        uint64_t words = (tok >> 16) & 0x7FFFu;
        if (pos + 4 + 4 * words > max)
            break;
        if (words >= 1 && rd32(code + pos + 4) == FOURCC_CTAB) {
            data = code + pos + 8;
            size = (uint32_t)(words - 1) * 4u;
            break;
        }
        pos += 4 + 4 * words;
    }
    if (data == NULL || size < CTAB_HEADER_SIZE || rd32(data) != CTAB_HEADER_SIZE)
        return D3DXERR_INVALIDDATA;

    struct d3dx_ctab *t = calloc(1, sizeof *t);
    struct ctab_parse c = {.p = data, .size = size};
    if (t == NULL || (t->host = malloc(size)) == NULL || (t->guest = guest_copy(data, size)) == 0) {
        if (t != NULL)
            free(t->host);
        free(t);
        return COM_E_OUTOFMEMORY;
    }
    memcpy(t->host, data, size);
    c.p = t->host;
    c.guest = t->guest;

    uint32_t creator = rd32(data + 4), n_top = rd32(data + 12), info = rd32(data + 16);
    int bad = (creator != 0 && !ctab_str_ok(&c, creator)) || info > size ||
              (size - info) / CTAB_CONSTANTINFO_SIZE < n_top;
    uint32_t first = 0;
    if (!bad && n_top != 0)
        bad = nodes_alloc(&c, n_top, &first) != 0;
    for (uint32_t i = 0; !bad && i < n_top; i++) {
        const uint8_t *ci = t->host + info + CTAB_CONSTANTINFO_SIZE * i;
        uint32_t regset = rd16(ci + 4), regindex = rd16(ci + 6), regcount = rd16(ci + 8);
        uint32_t defval = rd32(ci + 16);
        bad = parse_type(&c, i, rd32(ci + 12), 0, regindex, regindex + regcount,
                         defval != 0 ? &defval : NULL, rd32(ci), regset, 0) != 0;
        /* Top-level int4 counts keep the CTAB's own value. */
        if (!bad && regset == RS_INT4)
            c.nodes[i].regcount = regcount;
    }
    if (bad) {
        free(c.nodes);
        guest_free(t->guest);
        free(t->host);
        free(t);
        return D3DXERR_INVALIDDATA;
    }

    atomic_init(&t->refs, 1u);
    t->size = size;
    t->creator = creator != 0 ? t->guest + creator : 0u;
    t->version = rd32(data + 8);
    t->vertex = (version & 0xFFFF0000u) == 0xFFFE0000u;
    t->n_top = n_top;
    t->n_nodes = c.n;
    t->nodes = c.nodes;
    *out = com_guest_new(&g_ctab_iface, t);
    return COM_S_OK;
}

/* ---- ID3DXConstantTable: handles and names ------------------------------ */

static int32_t by_name(const struct d3dx_ctab *t, int32_t parent, const char *name);

/* "<n>]..." after a '[' in a constant name. */
static int32_t by_element(const struct d3dx_ctab *t, int32_t node, const char *name)
{
    const char *close = strchr(name, ']');
    if (*name == '\0' || close == NULL)
        return -1;
    uint32_t el = (uint32_t)atoi(name);
    const struct ctab_node *n = &t->nodes[node];
    if (n->elements <= el)
        return -1;
    int32_t c = n->n_children != 0 ? (int32_t)(n->first + el) : node;
    const char *part = close + 1;
    switch (*part) {
    case '.':
        return by_name(t, c, part + 1);
    case '[':
        return by_element(t, c, part + 1);
    case '\0':
        return c;
    default:
        return -1;
    }
}

/* A constant by name among the top-level constants (parent < 0) or the
 * members of parent. Names may continue with ".member" and "[index]". */
static int32_t by_name(const struct d3dx_ctab *t, int32_t parent, const char *name)
{
    if (name == NULL || *name == '\0')
        return -1;
    uint32_t first = 0, count = t->n_top;
    if (parent >= 0) {
        first = t->nodes[parent].first;
        count = min_u32(t->nodes[parent].members, t->nodes[parent].n_children);
    }
    size_t len = strcspn(name, "[.");
    const char *part = name + len;
    for (uint32_t i = 0; i < count; i++) {
        const char *nm = (const char *)t->host + t->nodes[first + i].nameoff;
        if (strlen(nm) != len || strncmp(nm, name, len) != 0)
            continue;
        switch (*part) {
        case '.':
            return by_name(t, (int32_t)(first + i), part + 1);
        case '[':
            return by_element(t, (int32_t)(first + i), part + 1);
        default:
            return (int32_t)(first + i);
        }
    }
    return -1;
}

static uint32_t handle_of(int32_t node) { return node < 0 ? 0u : (uint32_t)node + 1u; }

/* The node for a D3DXHANDLE: one of this table's handles first, otherwise a
 * guest char* name of a top-level constant, as the reference does when the
 * table was not created D3DXCONSTTABLE_LARGEADDRESSAWARE. */
static int32_t resolve(const struct d3dx_ctab *t, uint32_t h)
{
    if (h == 0)
        return -1;
    if (h <= t->n_nodes)
        return (int32_t)h - 1;
    char *name = guest_strdup(h, NAME_MAX_LEN);
    int32_t n = by_name(t, -1, name);
    free(name);
    return n;
}

/* ---- ID3DXConstantTable: writing registers through the guest device ----- */

/* Call device->SetXShaderConstant{F,I,B}(start, data, count) through the
 * guest device's vtable. data holds count registers of `words` values. */
static uint32_t dev_set(const struct d3dx_ctab *t, uint32_t dev, uint32_t regset, uint32_t start,
                        const uint32_t *data, uint32_t count, uint32_t guest_data)
{
    uint32_t off, words;
    switch (regset) {
    case RS_BOOL:
        off = t->vertex ? DEV_SET_VS_CONST_B : DEV_SET_PS_CONST_B, words = 1;
        break;
    case RS_INT4:
        off = t->vertex ? DEV_SET_VS_CONST_I : DEV_SET_PS_CONST_I, words = 4;
        break;
    case RS_FLOAT4:
        off = t->vertex ? DEV_SET_VS_CONST_F : DEV_SET_PS_CONST_F, words = 4;
        break;
    default:
        return COM_E_NOTIMPL;
    }
    uint32_t buf = guest_data;
    if (buf == 0 && (buf = guest_copy(data, 4u * words * count)) == 0)
        return COM_E_OUTOFMEMORY;
    uint32_t args[4] = {dev, start, buf, count};
    uint32_t hr = guest_call_cdecl(MEM32(MEM32(dev) + off), 4, args);
    if (buf != guest_data)
        guest_free(buf);
    return hr;
}

static uint32_t to_float(uint32_t type, uint32_t v)
{
    float f;
    switch (type) {
    case PT_FLOAT:
    case PT_VOID:
        return v;
    case PT_INT:
        f = (float)(int32_t)v;
        break;
    case PT_BOOL:
        f = v != 0 ? 1.0f : 0.0f;
        break;
    default:
        f = 0.0f;
        break;
    }
    memcpy(&v, &f, 4);
    return v;
}
static uint32_t to_bool(uint32_t type, uint32_t v)
{
    return type == PT_VOID ? v : (type <= PT_FLOAT && v != 0) ? 1u : 0u;
}
static uint32_t to_int(uint32_t type, uint32_t v)
{
    float f;
    switch (type) {
    case PT_FLOAT:
        memcpy(&f, &v, 4);
        return (uint32_t)(int32_t)f;
    case PT_INT:
    case PT_VOID:
        return v;
    case PT_BOOL:
        return v != 0;
    default:
        return 0;
    }
}
static uint32_t convert(uint32_t out, uint32_t v, uint32_t in)
{
    if (out == in)
        return v;
    switch (out) {
    case PT_FLOAT:
        return to_float(in, v);
    case PT_BOOL:
        return to_bool(in, v);
    case PT_INT:
        return to_int(in, v);
    default:
        return 0;
    }
}

/* Input values for a Set* call: host values, or a guest array (of 16-value
 * blocks behind pointers for the *PointerArray methods). NULL reads zeros. */
struct set_src {
    const uint32_t *host;
    uint32_t host_len;
    uint32_t guest;
    int is_pointer;
};

static uint32_t src_get(const struct set_src *s, uint32_t i)
{
    if (s == NULL)
        return 0;
    if (s->host != NULL)
        return i < s->host_len ? s->host[i] : 0u;
    if (s->guest == 0)
        return 0;
    if (s->is_pointer)
        return MEM32(MEM32(s->guest + 4u * (i / 16u)) + 4u * (i % 16u));
    return MEM32(s->guest + 4u * i);
}

/* Write node ni from src, starting at input value index. *size counts the
 * input values left; inclass and incol describe the input's shape. Returns
 * how many input values this node used. The rules are those of the reference
 * D3DX, including its handling of structs and arrays. */
static uint32_t set_node(const struct d3dx_ctab *t, uint32_t dev, uint32_t ni,
                         const struct set_src *src, uint32_t intype, uint32_t *size,
                         uint32_t incol, uint32_t inclass, uint32_t index, int is_pointer)
{
    const struct ctab_node *d = &t->nodes[ni];
    uint32_t regcount, regsize, cin = 1, rin = 1, ret, last = 0;

    if (*size < d->rows * d->cols) {
        *size = 0;
        return 0;
    }

    if (d->cls == PC_STRUCT) {
        int first_only = (is_pointer || inclass == PC_MATRIX_ROWS) && d->regset != RS_BOOL;
        if (d->n_children == 0)
            return 0;
        if (d->elements > 1) {
            /* A struct array sets only its first element, from the last
             * complete input. */
            uint32_t per = d->rows * d->cols;
            if (per == 0)
                return 0;
            last = min_u32(d->elements - 1u, *size / per - 1u) * per;
            if (first_only)
                set_node(t, dev, d->first, NULL, intype, size, incol, inclass, 0, is_pointer);
            else
                last += set_node(t, dev, d->first, src, intype, size, incol, inclass, index + last,
                                 is_pointer);
        } else if (inclass == PC_MATRIX_ROWS && d->regset == RS_BOOL) {
            /* Bool structs take a transposed matrix, one value per member. */
            uint32_t m[16], tr[16];
            for (uint32_t k = 0; k < 16; k++)
                m[k] = is_pointer ? src_get(src, (index / 16u) * 16u + k) : src_get(src, k);
            for (uint32_t r = 0; r < 4; r++)
                for (uint32_t col = 0; col < 4; col++)
                    tr[r * 4 + col] = m[col * 4 + r];
            struct set_src hs = {tr, 16, 0, 0};
            for (uint32_t i = 0; i < d->n_children; i++)
                last += set_node(t, dev, d->first + i, &hs, intype, size, incol, PC_SCALAR,
                                 index + last, is_pointer);
        } else if (first_only) {
            /* Only the first member takes input; the others are zeroed. */
            last = set_node(t, dev, d->first, src, intype, size, incol, inclass, index, is_pointer);
            for (uint32_t i = 1; i < d->n_children; i++)
                set_node(t, dev, d->first + i, NULL, intype, size, incol, inclass, 0, is_pointer);
        } else {
            for (uint32_t i = 0; i < d->n_children; i++)
                last += set_node(t, dev, d->first + i, src, intype, size, incol, PC_SCALAR,
                                 index + last, is_pointer);
        }
        return last;
    }

    if (d->elements > 1) {
        for (uint32_t i = 0; i < d->n_children && *size > 0; i++) {
            last += set_node(t, dev, d->first + i, src, intype, size, incol, inclass, index + last,
                             is_pointer);
            /* Vector arrays from matrix rows skip to the next matrix. */
            if (inclass == PC_MATRIX_ROWS && d->cls == PC_VECTOR && i % 4u == 3u) {
                last += 12;
                *size = sub_sat(*size, 12);
            }
        }
        return last;
    }

    switch (d->cls) {
    case PC_SCALAR:
    case PC_VECTOR:
    case PC_MATRIX_ROWS:
        regcount = min_u32(d->regcount, d->rows);
        if (inclass == PC_MATRIX_ROWS)
            cin = incol;
        else
            rin = incol;
        regsize = d->cols;
        break;
    case PC_MATRIX_COLUMNS:
        regcount = min_u32(d->regcount, d->cols);
        if (inclass == PC_MATRIX_ROWS)
            rin = incol;
        else
            cin = incol;
        regsize = d->rows;
        break;
    default:
        return 0;
    }

    switch (inclass) {
    case PC_SCALAR:
        ret = d->cols * d->rows;
        *size = sub_sat(*size, ret);
        break;
    case PC_VECTOR:
        switch (d->cls) {
        case PC_MATRIX_ROWS:
            if (*size < regcount * 4u) {
                *size = 0;
                return 0;
            }
            ret = 4u * regcount;
            *size -= 4u * regcount;
            break;
        case PC_MATRIX_COLUMNS:
            ret = 4u * regsize;
            *size = sub_sat(*size, 4u * regcount);
            break;
        case PC_SCALAR:
            ret = 1;
            *size = sub_sat(*size, 1);
            break;
        default: /* PC_VECTOR */
            ret = 4;
            *size = sub_sat(*size, 4);
            break;
        }
        break;
    case PC_MATRIX_ROWS:
    case PC_MATRIX_COLUMNS:
        if (d->cls == PC_MATRIX_ROWS || d->cls == PC_MATRIX_COLUMNS) {
            if (*size < 16) {
                *size = 0;
                return 0;
            }
            ret = 16;
        } else if (d->cls == PC_SCALAR) {
            ret = inclass == PC_MATRIX_ROWS ? 4u : 1u;
        } else {
            ret = inclass == PC_MATRIX_ROWS ? 1u : 4u;
        }
        *size = sub_sat(*size, ret);
        break;
    default:
        return 0;
    }

    uint32_t width = min_u32(regsize, 4); /* a register holds 4 values */
    if (d->regset == RS_BOOL) {
        regcount = min_u32(d->regcount, d->cols * d->rows);
        for (uint32_t i = 0, l = 0; i < regcount; i++) {
            uint32_t v = src_get(src, index + i / regsize * rin + l * cin);
            uint32_t b = convert(PT_BOOL, convert(d->type, v, intype), d->type);
            dev_set(t, dev, RS_BOOL, d->regindex + i, &b, 1, 0);
            if (++l >= regsize)
                l = 0;
        }
    } else if (d->regset == RS_INT4 || d->regset == RS_FLOAT4) {
        uint32_t reg_type = d->regset == RS_INT4 ? PT_INT : PT_FLOAT;
        for (uint32_t i = 0; i < regcount; i++) {
            uint32_t vec[4] = {0, 0, d->regset == RS_INT4 ? 1u : 0u, 0};
            for (uint32_t l = 0; l < width; l++) {
                uint32_t v = src_get(src, index + i * rin + l * cin);
                vec[l] = convert(reg_type, convert(d->type, v, intype), d->type);
            }
            dev_set(t, dev, d->regset, d->regindex + i, vec, 1, 0);
        }
    } else {
        return 0;
    }
    return ret;
}

/* The class gates of the reference's set_scalar, set_vector and friends:
 * which classes a method writes (1), accepts as a no-op (0) or rejects (-1). */
enum set_kind { SET_SCALAR, SET_SCALAR_ARRAY, SET_VECTOR, SET_VECTOR_ARRAY, SET_MATRIX };

static int set_gate(enum set_kind k, uint32_t cls)
{
    if (cls > PC_STRUCT || cls == PC_OBJECT)
        return -1;
    if (k == SET_SCALAR)
        return cls == PC_SCALAR;
    if (k == SET_VECTOR)
        return cls != PC_MATRIX_ROWS && cls != PC_MATRIX_COLUMNS;
    return 1;
}

static uint32_t set_common(enum set_kind k, const struct set_src *src, uint32_t intype,
                           uint32_t count, uint32_t inclass, int is_pointer)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t dev = W32_ARG(1);
    int32_t c = resolve(t, W32_ARG(2));
    if (c < 0)
        return D3DERR_INVALIDCALL;
    int gate = set_gate(k, t->nodes[c].cls);
    if (gate < 0)
        return D3DERR_INVALIDCALL;
    if (gate == 0)
        return COM_S_OK;
    uint32_t incol = k == SET_SCALAR || k == SET_SCALAR_ARRAY ? t->nodes[c].cols : 4u;
    set_node(t, dev, (uint32_t)c, src, intype, &count, incol, inclass, 0, is_pointer);
    return COM_S_OK;
}

/* SetBool, SetInt, SetFloat (this, device, handle, value). */
static void set_scalar_method(uint32_t type)
{
    uint32_t v = W32_ARG(3);
    struct set_src s = {&v, 1, 0, 0};
    W32_RETURN(4, set_common(SET_SCALAR, &s, type, 1, PC_SCALAR, 0));
}
/* Set*Array (this, device, handle, values, count). */
static void set_array_method(enum set_kind k, uint32_t type, uint32_t per, uint32_t inclass,
                             int is_pointer)
{
    struct set_src s = {NULL, 0, W32_ARG(3), is_pointer};
    W32_RETURN(5, set_common(k, &s, type, W32_ARG(4) * per, inclass, is_pointer));
}
/* SetVector, SetMatrix, SetMatrixTranspose (this, device, handle, value). */
static void set_one_method(enum set_kind k, uint32_t per, uint32_t inclass)
{
    struct set_src s = {NULL, 0, W32_ARG(3), 0};
    W32_RETURN(4, set_common(k, &s, PT_FLOAT, per, inclass, 0));
}

static void ct_set_bool(void) { set_scalar_method(PT_BOOL); }
static void ct_set_int(void) { set_scalar_method(PT_INT); }
static void ct_set_float(void) { set_scalar_method(PT_FLOAT); }
static void ct_set_bool_array(void) { set_array_method(SET_SCALAR_ARRAY, PT_BOOL, 1, PC_SCALAR, 0); }
static void ct_set_int_array(void) { set_array_method(SET_SCALAR_ARRAY, PT_INT, 1, PC_SCALAR, 0); }
static void ct_set_float_array(void)
{
    set_array_method(SET_SCALAR_ARRAY, PT_FLOAT, 1, PC_SCALAR, 0);
}
static void ct_set_vector(void) { set_one_method(SET_VECTOR, 4, PC_VECTOR); }
static void ct_set_vector_array(void)
{
    set_array_method(SET_VECTOR_ARRAY, PT_FLOAT, 4, PC_VECTOR, 0);
}
static void ct_set_matrix(void) { set_one_method(SET_MATRIX, 16, PC_MATRIX_COLUMNS); }
static void ct_set_matrix_array(void)
{
    set_array_method(SET_MATRIX, PT_FLOAT, 16, PC_MATRIX_COLUMNS, 0);
}
static void ct_set_matrix_pointer_array(void)
{
    set_array_method(SET_MATRIX, PT_FLOAT, 16, PC_MATRIX_COLUMNS, 1);
}
static void ct_set_matrix_transpose(void) { set_one_method(SET_MATRIX, 16, PC_MATRIX_ROWS); }
static void ct_set_matrix_transpose_array(void)
{
    set_array_method(SET_MATRIX, PT_FLOAT, 16, PC_MATRIX_ROWS, 0);
}
static void ct_set_matrix_transpose_pointer_array(void)
{
    set_array_method(SET_MATRIX, PT_FLOAT, 16, PC_MATRIX_ROWS, 1);
}

/* SetValue(this, device, handle, data, bytes): data in the constant's type. */
static void ct_set_value(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t dev = W32_ARG(1), data = W32_ARG(3), count = W32_ARG(4) / 4u;
    int32_t c = resolve(t, W32_ARG(2));
    if (dev == 0 || c < 0 || data == 0 || set_gate(SET_SCALAR_ARRAY, t->nodes[c].cls) < 0) {
        W32_RETURN(5, D3DERR_INVALIDCALL);
        return;
    }
    struct set_src s = {NULL, 0, data, 0};
    set_node(t, dev, (uint32_t)c, &s, t->nodes[c].type, &count, t->nodes[c].cols, PC_SCALAR, 0, 0);
    W32_RETURN(5, COM_S_OK);
}

/* SetDefaults(this, device): every top-level default, register-wise. */
static void ct_set_defaults(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t dev = W32_ARG(1), hr = COM_S_OK;
    if (dev == 0) {
        W32_RETURN(2, D3DERR_INVALIDCALL);
        return;
    }
    for (uint32_t i = 0; i < t->n_top && hr == COM_S_OK; i++) {
        const struct ctab_node *d = &t->nodes[i];
        if (d->defval != 0)
            hr = dev_set(t, dev, d->regset, d->regindex, NULL, d->regcount, d->defval);
    }
    W32_RETURN(2, hr);
}

/* ---- ID3DXConstantTable: queries ---------------------------------------- */

static void ctab_addref_obj(uint32_t self)
{
    struct d3dx_ctab *t = com_guest_self(self);
    atomic_fetch_add(&t->refs, 1u);
}
static void ct_qi(void)
{
    query_interface(win32_d3dx_sdk_version() < 43 ? &IID_ID3DXConstantTable_42 : &IID_ID3DXConstantTable_43,
                    ctab_addref_obj);
}
static void ct_addref(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    W32_RETURN(1, atomic_fetch_add(&t->refs, 1u) + 1u);
}
static void ct_release(void)
{
    uint32_t self = W32_ARG(0);
    struct d3dx_ctab *t = com_guest_self(self);
    uint32_t left = atomic_fetch_sub(&t->refs, 1u) - 1u;
    if (left == 0) {
        guest_free(t->guest);
        com_guest_free(self);
        free(t->nodes);
        free(t->host);
        free(t);
    }
    W32_RETURN(1, left);
}
static void ct_get_buffer_pointer(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    W32_RETURN(1, t->guest);
}
static void ct_get_buffer_size(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    W32_RETURN(1, t->size);
}

/* GetDesc(this, D3DXCONSTANTTABLE_DESC *): Creator +0, Version +4,
 * Constants +8. */
static void ct_get_desc(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t desc = W32_ARG(1);
    if (desc == 0) {
        W32_RETURN(2, D3DERR_INVALIDCALL);
        return;
    }
    MEM32(desc) = t->creator;
    MEM32(desc + 4u) = t->version;
    MEM32(desc + 8u) = t->n_top;
    W32_RETURN(2, COM_S_OK);
}

/* GetConstantDesc(this, handle, D3DXCONSTANT_DESC *, UINT *count). The
 * 48-byte desc: Name, RegisterSet, RegisterIndex, RegisterCount, Class, Type,
 * Rows, Columns, Elements, StructMembers, Bytes, DefaultValue. */
static void ct_get_constant_desc(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t desc = W32_ARG(2), pcount = W32_ARG(3);
    int32_t c = resolve(t, W32_ARG(1));
    if (c < 0) {
        W32_RETURN(4, D3DERR_INVALIDCALL);
        return;
    }
    const struct ctab_node *d = &t->nodes[c];
    if (desc != 0) {
        const uint32_t v[12] = {t->guest + d->nameoff, d->regset, d->regindex, d->regcount,
                                d->cls, d->type, d->rows, d->cols, d->elements, d->members,
                                d->bytes, d->defval};
        for (uint32_t i = 0; i < 12; i++)
            MEM32(desc + 4u * i) = v[i];
    }
    if (pcount != 0)
        MEM32(pcount) = 1;
    W32_RETURN(4, COM_S_OK);
}

static void ct_get_sampler_index(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    int32_t c = resolve(t, W32_ARG(1));
    W32_RETURN(2, c >= 0 && t->nodes[c].regset == RS_SAMPLER ? t->nodes[c].regindex : 0xFFFFFFFFu);
}

/* GetConstant(this, parent, index): a member of parent, or a top-level
 * constant when parent is NULL. */
static void ct_get_constant(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t parent = W32_ARG(1), index = W32_ARG(2);
    int32_t r = -1;
    if (parent != 0) {
        int32_t c = resolve(t, parent);
        if (c >= 0 && index < t->nodes[c].members && index < t->nodes[c].n_children)
            r = (int32_t)(t->nodes[c].first + index);
    } else if (index < t->n_top) {
        r = (int32_t)index;
    }
    W32_RETURN(3, handle_of(r));
}

static void ct_get_constant_by_name(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t name = W32_ARG(2);
    int32_t r = -1;
    if (name != 0) {
        char *s = guest_strdup(name, NAME_MAX_LEN);
        r = by_name(t, resolve(t, W32_ARG(1)), s);
        free(s);
    }
    W32_RETURN(3, handle_of(r));
}

static void ct_get_constant_element(void)
{
    struct d3dx_ctab *t = com_guest_self(W32_ARG(0));
    uint32_t index = W32_ARG(2);
    int32_t c = resolve(t, W32_ARG(1)), r = -1;
    if (c >= 0 && index < t->nodes[c].elements)
        r = t->nodes[c].elements > 1 ? (int32_t)(t->nodes[c].first + index) : c;
    W32_RETURN(3, handle_of(r));
}

/* Published ID3DXConstantTable order (ID3DXBuffer methods first). */
static const recomp_func_t g_ctab_methods[27] = {
    ct_qi,
    ct_addref,
    ct_release,
    ct_get_buffer_pointer,
    ct_get_buffer_size,
    ct_get_desc,
    ct_get_constant_desc,
    ct_get_sampler_index,
    ct_get_constant,
    ct_get_constant_by_name,
    ct_get_constant_element,
    ct_set_defaults,
    ct_set_value,
    ct_set_bool,
    ct_set_bool_array,
    ct_set_int,
    ct_set_int_array,
    ct_set_float,
    ct_set_float_array,
    ct_set_vector,
    ct_set_vector_array,
    ct_set_matrix,
    ct_set_matrix_array,
    ct_set_matrix_pointer_array,
    ct_set_matrix_transpose,
    ct_set_matrix_transpose_array,
    ct_set_matrix_transpose_pointer_array,
};

/* ---- D3DXGetShaderConstantTable ----------------------------------------- */

void thunk_D3DXGetShaderConstantTable(void)
{
    uint32_t code = W32_ARG(0), out = W32_ARG(1), hr;
    if (out != 0)
        MEM32(out) = 0;
    if (code == 0 || out == 0) {
        hr = D3DERR_INVALIDCALL;
    } else if ((MEM32(code) & 0xFFFE0000u) != 0xFFFE0000u) {
        hr = COM_S_OK; /* not shader byte code: no table, and no error */
    } else {
        uint32_t obj;
        uint64_t max = (uint64_t)0x100000000u - code;
        hr = ctab_create(guest_ptr(code), max < BYTECODE_MAX ? max : BYTECODE_MAX, &obj);
        MEM32(out) = obj;
    }
    W32_RETURN(2, hr);
}

/* ---- D3DXCompileShader -------------------------------------------------- */

/* The guest ID3DXInclude (vtable: Open, Close; no IUnknown), or 0. */
struct include_ctx {
    uint32_t include;
};

/* Open(this, IncludeType, pFileName, pParentData, ppData, pBytes). The data
 * stays in guest memory until Close; vkd3d reads it in place, so the parent
 * data vkd3d hands back maps to the guest pointer Open returned. */
static int open_include(const char *filename, bool local, const char *parent_data, void *context,
                        struct vkd3d_shader_code *out)
{
    const struct include_ctx *ic = context;
    if (ic->include == 0)
        return VKD3D_ERROR; /* from memory with no handler, D3DX fails includes */
    uint32_t len = (uint32_t)strlen(filename) + 1u;
    uint32_t name = guest_copy(filename, len), slots = guest_malloc(8);
    if (name == 0 || slots == 0) {
        guest_free(name);
        guest_free(slots);
        return VKD3D_ERROR_OUT_OF_MEMORY;
    }
    MEM32(slots) = 0;
    MEM32(slots + 4u) = 0;
    uint32_t args[6] = {ic->include, local ? D3DXINC_LOCAL : D3DXINC_SYSTEM, name,
                        guest_addr_of(parent_data), slots, slots + 4u};
    uint32_t hr = guest_call_cdecl(MEM32(MEM32(ic->include)), 6, args);
    uint32_t data = MEM32(slots), bytes = MEM32(slots + 4u);
    guest_free(name);
    guest_free(slots);
    if (hr & 0x80000000u)
        return VKD3D_ERROR;
    out->code = data != 0 ? guest_ptr(data) : NULL;
    out->size = data != 0 ? bytes : 0u;
    return VKD3D_OK;
}

/* Close(this, pData). */
static void close_include(const struct vkd3d_shader_code *code, void *context)
{
    const struct include_ctx *ic = context;
    uint32_t data = guest_addr_of(code->code);
    if (data == 0)
        return;
    uint32_t args[2] = {ic->include, data};
    guest_call_cdecl(MEM32(MEM32(ic->include) + 4u), 2, args);
}

/* Shader Model 1-3 profiles compile to legacy Direct3D byte code. */
static int is_d3dbc_profile(const char *p)
{
    static const char *const prefixes[] = {"vs_1_", "vs_2_", "vs_3_", "ps_1_", "ps_2_", "ps_3_"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncmp(p, prefixes[i], 5) == 0)
            return 1;
    return 0;
}

/* One UNSUPPORTED line per flag bit that has no vkd3d option. */
static void report_flags(uint32_t flags)
{
    static atomic_uint reported;
    uint32_t todo = flags & ~D3DXSHADER_MAPPED;
    for (uint32_t bit = 1; todo != 0; bit <<= 1) {
        if ((todo & bit) == 0)
            continue;
        todo &= ~bit;
        if ((atomic_fetch_or(&reported, bit) & bit) == 0)
            fprintf(stderr, "UNSUPPORTED d3dx9: D3DXCompileShader flag 0x%X\n", bit);
    }
}

/* The guest D3DXMACRO array: Name, Definition pairs up to a NULL Name. */
static struct vkd3d_shader_macro *read_macros(uint32_t defines, unsigned *count)
{
    unsigned n = 0;
    if (defines != 0)
        while (MEM32(defines + 8u * n) != 0)
            n++;
    *count = n;
    struct vkd3d_shader_macro *m = calloc(n != 0 ? n : 1u, sizeof *m);
    for (unsigned i = 0; m != NULL && i < n; i++) {
        uint32_t def = MEM32(defines + 8u * i + 4u);
        m[i].name = guest_strdup(MEM32(defines + 8u * i), NAME_MAX_LEN);
        m[i].value = def != 0 ? guest_strdup(def, 0x10000u) : strdup("");
    }
    return m;
}

static void free_macros(struct vkd3d_shader_macro *m, unsigned count)
{
    for (unsigned i = 0; m != NULL && i < count; i++) {
        free((void *)m[i].name);
        free((void *)m[i].value);
    }
    free(m);
}

/* ---- Disk cache of compiled shaders ------------------------------------- */

/* One file per compile, named by the SHA-256 of everything that decides the
 * output: source, macros, entry, profile, flags and the compiler version.
 * The file holds the byte code (its CTAB comment is the constant table) and
 * the compiler messages. Only compiles without an ID3DXInclude are cached:
 * included text is not in the key. The game always passes no include.
 * HPRECOMP_SHADER_CACHE=0 turns the cache off. */
#define CACHE_FORMAT "HarryPotterRecomp d3dx shader cache 1"
#define CACHE_MAGIC 0x43535048u /* 'H','P','S','C' */
/* Our local vkd3d patches change the byte code without changing
 * vkd3d_shader_get_version(); bump this with every patch so stale disk-cache
 * entries and the game's own CachedShaders miss (d3dx_game_cache_check). */
#define D3DX_COMPILER_REVISION "vkd3d fed0c34 + tools/vkd3d/patches/0001 (mul emits dot from single registers)"

static atomic_uint g_cache_hits;

uint32_t d3dx_shader_cache_hits(void) { return atomic_load(&g_cache_hits); }

static int cache_enabled(void)
{
    const char *e = getenv("HPRECOMP_SHADER_CACHE");
    return e == NULL || strcmp(e, "0") != 0;
}

/* $HOME/Library/Caches/HarryPotterRecomp/shaders, created on first use. */
static int cache_dir(char *dir, size_t size)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return 0;
    static const char *const parts[] = {"/Library", "/Caches", "/HarryPotterRecomp", "/shaders"};
    size_t n = (size_t)snprintf(dir, size, "%s", home);
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        n += (size_t)snprintf(dir + n, n < size ? size - n : 0, "%s", parts[i]);
        if (n >= size || (mkdir(dir, 0755) != 0 && access(dir, W_OK) != 0))
            return 0;
    }
    return 1;
}

static void sha_field(CC_SHA256_CTX *c, const void *p, size_t len)
{
    uint64_t n = len;
    CC_SHA256_Update(c, &n, sizeof n);
    CC_SHA256_Update(c, p, (CC_LONG)len);
}

static void sha_str(CC_SHA256_CTX *c, const char *s) { sha_field(c, s != NULL ? s : "", s != NULL ? strlen(s) : 0); }

static void cache_key(const struct vkd3d_shader_compile_info *info, const struct vkd3d_shader_preprocess_info *pre,
                      const struct vkd3d_shader_hlsl_source_info *hlsl, uint32_t flags, char hex[65])
{
    CC_SHA256_CTX c;
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Init(&c);
    sha_str(&c, CACHE_FORMAT);
    sha_str(&c, vkd3d_shader_get_version(NULL, NULL));
    sha_str(&c, D3DX_COMPILER_REVISION);
    sha_field(&c, info->source.code, info->source.size);
    sha_field(&c, &pre->macro_count, sizeof pre->macro_count);
    for (unsigned i = 0; i < pre->macro_count; i++) {
        sha_str(&c, pre->macros[i].name);
        sha_str(&c, pre->macros[i].value);
    }
    sha_str(&c, hlsl->entry_point);
    sha_str(&c, hlsl->profile);
    sha_field(&c, &flags, sizeof flags);
    /* The options follow the D3DX SDK version too, and games share the cache. */
    sha_field(&c, info->options, info->option_count * sizeof *info->options);
    CC_SHA256_Final(md, &c);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(hex + 2 * i, 3, "%02x", md[i]);
}

/* File: magic, code size, message size (u32 each), code, messages. The
 * buffers are malloc'd: vkd3d frees shader code and messages with free(). */
static int cache_load(const char *path, struct vkd3d_shader_code *out, char **messages)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    uint32_t h[3];
    int ok = fread(h, sizeof h, 1, f) == 1 && h[0] == CACHE_MAGIC && h[1] != 0 && h[1] <= BYTECODE_MAX &&
             h[2] <= 0x100000u;
    void *code = ok ? malloc(h[1]) : NULL;
    char *msg = ok ? malloc(h[2] + 1u) : NULL;
    ok = ok && code != NULL && msg != NULL && fread(code, 1, h[1], f) == h[1] &&
         fread(msg, 1, h[2], f) == h[2] && fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        free(code);
        free(msg);
        return 0;
    }
    msg[h[2]] = '\0';
    out->code = code;
    out->size = h[1];
    *messages = msg;
    return 1;
}

/* Written to a unique temporary name, then renamed, so parallel compiles and
 * a crash mid-write never leave a partial file under the final name. */
static void cache_store(const char *dir, const char *path, const struct vkd3d_shader_code *code,
                        const char *messages)
{
    static atomic_uint seq;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s/.tmp.%d.%u", dir, (int)getpid(), atomic_fetch_add(&seq, 1u));
    uint32_t h[3] = {CACHE_MAGIC, (uint32_t)code->size, messages != NULL ? (uint32_t)strlen(messages) : 0u};
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return;
    int ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(code->code, 1, code->size, f) == code->size &&
             fwrite(messages != NULL ? messages : "", 1, h[2], f) == h[2];
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0)
        unlink(tmp);
}

/* vkd3d_shader_compile through the cache. Failed compiles are not cached. */
static int compile_cached(const struct vkd3d_shader_compile_info *info, uint32_t flags, int cacheable,
                          struct vkd3d_shader_code *out, char **messages)
{
    const struct vkd3d_shader_preprocess_info *pre = info->next;
    const struct vkd3d_shader_hlsl_source_info *hlsl = pre->next;
    char dir[1024], path[1200], hex[65];
    cacheable = cacheable && cache_enabled() && cache_dir(dir, sizeof dir);
    if (cacheable) {
        cache_key(info, pre, hlsl, flags, hex);
        snprintf(path, sizeof path, "%s/%s.bin", dir, hex);
        if (cache_load(path, out, messages)) {
            atomic_fetch_add(&g_cache_hits, 1u);
            return VKD3D_OK;
        }
    }
    int ret = vkd3d_shader_compile(info, out, messages);
    if (cacheable && ret >= 0)
        cache_store(dir, path, out, *messages);
    return ret;
}

/* ---- Compile dump -------------------------------------------------------- */

/* HPRECOMP_SHADER_DUMP=<dir> writes every compile request as <key>.hlsl (the
 * source), <key>.json (entry, profile, flags, macros, source length, result,
 * messages, byte code length) and, on success, <key>.bin (the byte code).
 * <key> is the disk cache key. Unset: nothing is computed. */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s != NULL ? s : ""); *p != '\0'; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if (*p == '\n')
            fputs("\\n", f);
        else if (*p < 0x20 || *p >= 0x7f)
            fprintf(f, "\\u%04x", *p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}

static FILE *dump_open(const char *dir, const char *hex, const char *ext)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.%s", dir, hex, ext);
    return fopen(path, "wb");
}

static void dump_compile(const struct vkd3d_shader_compile_info *info, uint32_t flags, uint32_t hr,
                         const char *messages, const struct vkd3d_shader_code *out)
{
    const char *dir = getenv("HPRECOMP_SHADER_DUMP");
    if (dir == NULL || dir[0] == '\0')
        return;
    const struct vkd3d_shader_preprocess_info *pre = info->next;
    const struct vkd3d_shader_hlsl_source_info *hlsl = pre->next;
    char hex[65];
    cache_key(info, pre, hlsl, flags, hex);
    mkdir(dir, 0755);
    FILE *f = dump_open(dir, hex, "hlsl");
    if (f != NULL) {
        fwrite(info->source.code, 1, info->source.size, f);
        fclose(f);
    }
    if (hr == COM_S_OK && (f = dump_open(dir, hex, "bin")) != NULL) {
        fwrite(out->code, 1, out->size, f);
        fclose(f);
    }
    if ((f = dump_open(dir, hex, "json")) == NULL)
        return;
    fputs("{\"entry\": ", f);
    json_str(f, hlsl->entry_point != NULL ? hlsl->entry_point : "main");
    fputs(", \"profile\": ", f);
    json_str(f, hlsl->profile);
    fprintf(f, ", \"flags\": %u, \"macros\": [", flags);
    for (unsigned i = 0; i < pre->macro_count; i++) {
        fputs(i != 0 ? ", [" : "[", f);
        json_str(f, pre->macros[i].name);
        fputs(", ", f);
        json_str(f, pre->macros[i].value);
        fputc(']', f);
    }
    fprintf(f, "], \"source_length\": %zu, \"hr\": %u, \"errors\": ", info->source.size, hr);
    json_str(f, messages);
    fprintf(f, ", \"bytecode_length\": %zu}\n", hr == COM_S_OK ? out->size : (size_t)0);
    fclose(f);
}

/* HPRECOMP_SHADER_OVERRIDE=<dir>: D3DXCompileShader returns the byte code in
 * <dir>/<sha256 of the source text>.bin instead of compiling, when that file
 * exists. Diagnostic switch: it lets a run use Microsoft's d3dx9_41 output
 * (tools/shaders/override_dir.py builds the folder from the oracle dump) for
 * all or a subset of shaders, to compare against our compiler. */
static int d3dx_shader_override(const void *src, size_t len, void **code, size_t *size)
{
    const char *dir = getenv("HPRECOMP_SHADER_OVERRIDE");
    if (dir == NULL || dir[0] == '\0')
        return 0;
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(src, (CC_LONG)len, md);
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/", dir);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH && n > 0 && (size_t)n < sizeof path; i++)
        n += snprintf(path + n, sizeof path - (size_t)n, "%02x", md[i]);
    snprintf(path + n, n > 0 && (size_t)n < sizeof path ? sizeof path - (size_t)n : 0, ".bin");
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = sz > 0 ? malloc((size_t)sz) : NULL;
    int ok = buf != NULL && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!ok) {
        free(buf);
        return 0;
    }
    *code = buf;
    *size = (size_t)sz;
    return 1;
}

/* The game keeps its own byte code cache in <AppData>/<publisher>/<game>/
 * CachedShaders and skips D3DXCompileShader on a hit. Its only check is
 * VERSION.DAT: int 0x32, then 16 bytes that must equal a table in the
 * game; on a mismatch it deletes the *.DAT files and
 * writes a new VERSION.DAT. Nothing in that check sees our compiler, so we
 * keep a stamp next to it and, when the stamp is stale, zero the version int:
 * the game then wipes the cache itself. save_dir is the redirected root. */
int d3dx_game_cache_check(const char *save_dir)
{
    char pat[1200];
    snprintf(pat, sizeof pat, "%s/Users/*/AppData/Roaming/*/*/CachedShaders/VERSION.DAT", save_dir);
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0)
        return 0;
    int found = (int)g.gl_pathc;
    static const char rev[] = D3DX_COMPILER_REVISION "\n";
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char stamp[1200], cur[256] = {0};
        snprintf(stamp, sizeof stamp, "%.*s/RECOMP_D3DX.TXT",
                 (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
        FILE *f = fopen(stamp, "rb");
        if (f != NULL) {
            fread(cur, 1, sizeof cur - 1, f);
            fclose(f);
        }
        if (strcmp(cur, rev) == 0)
            continue;
        static const uint32_t stale = 0;
        if ((f = fopen(g.gl_pathv[i], "r+b")) == NULL)
            continue;
        int ok = fwrite(&stale, sizeof stale, 1, f) == 1;
        ok = fclose(f) == 0 && ok;
        if (ok && (f = fopen(stamp, "wb")) != NULL) {
            fputs(rev, f);
            fclose(f);
            fprintf(stderr, "d3dx9: compiler changed, game shader cache marked stale (%s)\n", g.gl_pathv[i]);
        }
    }
    globfree(&g);
    return found;
}

/* Writes our compiler revision beside each VERSION.DAT, so the next start's
 * d3dx_game_cache_check keeps a cache this build filled (a game's shader
 * warm-up, and the clean exit of a run that started with no cache). A cache
 * made without a stamp is marked stale on the next start. */
void d3dx_game_cache_stamp(const char *save_dir)
{
    char pat[1200];
    snprintf(pat, sizeof pat, "%s/Users/*/AppData/Roaming/*/*/CachedShaders/VERSION.DAT", save_dir);
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0)
        return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char stamp[1200];
        snprintf(stamp, sizeof stamp, "%.*s/RECOMP_D3DX.TXT",
                 (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
        FILE *f = fopen(stamp, "wb");
        if (f != NULL) {
            fputs(D3DX_COMPILER_REVISION "\n", f);
            fclose(f);
        }
    }
    globfree(&g);
}

static uint32_t compile_shader(uint32_t src, uint32_t len, uint32_t defines, uint32_t include,
                               uint32_t func, uint32_t profile, uint32_t flags, uint32_t pp_shader,
                               uint32_t pp_errors, uint32_t pp_table)
{
    if (pp_shader != 0)
        MEM32(pp_shader) = 0;
    if (pp_errors != 0)
        MEM32(pp_errors) = 0;
    if (pp_table != 0)
        MEM32(pp_table) = 0;
    if (src == 0 || profile == 0)
        return D3DERR_INVALIDCALL;

    char *prof = guest_strdup(profile, 64), *entry = func != 0 ? guest_strdup(func, NAME_MAX_LEN) : NULL;
    if (!is_d3dbc_profile(prof)) {
        char msg[128];
        snprintf(msg, sizeof msg, "D3DXCompileShader: %s is not a Shader Model 1-3 profile.\n", prof);
        if (pp_errors != 0)
            MEM32(pp_errors) = d3dx_buffer_new(msg, (uint32_t)strlen(msg) + 1u);
        free(prof);
        free(entry);
        return D3DXERR_INVALIDDATA;
    }
    report_flags(flags);

    unsigned n_macros = 0;
    struct vkd3d_shader_macro *macros = read_macros(defines, &n_macros);
    struct include_ctx ic = {include};
    /* Option set of vkd3d-utils' D3DCompile2VKD3D for this SDK version, plus
     * half globals as float: in Shader Model 1-3 half is only a precision
     * hint, D3DX accepts half globals and vkd3d rejects them without this. A
     * "#define half float" macro instead breaks the game's half/float
     * overload pairs (deferredFilterCommon.hlsl: float sq(float), half
     * sq(half)). */
    struct vkd3d_shader_compile_option opts[8];
    unsigned n_opts = 0;
    int sdk = win32_d3dx_sdk_version();
    opts[n_opts++] = (struct vkd3d_shader_compile_option){VKD3D_SHADER_COMPILE_OPTION_API_VERSION,
                                                          VKD3D_SHADER_API_VERSION_2_1};
    if (!(flags & D3DXSHADER_DEBUG))
        opts[n_opts++] = (struct vkd3d_shader_compile_option){VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1};
    if (flags & (D3DXSHADER_PACKMATRIX_ROWMAJOR | D3DXSHADER_PACKMATRIX_COLUMNMAJOR))
        opts[n_opts++] = (struct vkd3d_shader_compile_option){
            VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER,
            (flags & D3DXSHADER_PACKMATRIX_ROWMAJOR) ? VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR
                                                     : VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR};
    opts[n_opts++] = (struct vkd3d_shader_compile_option){VKD3D_SHADER_COMPILE_OPTION_IEEE_754_FP,
                                                          (flags & D3DXSHADER_IEEE_STRICTNESS) != 0};
    opts[n_opts++] = (struct vkd3d_shader_compile_option){
        VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY,
        VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_ALLOW_HALF_GLOBALS |
            ((flags & D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY)
                 ? VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES
                 : (sdk >= 37 ? VKD3D_SHADER_COMPILE_OPTION_CONST_GLOBAL_UNIFORMS : 0))};
    if (sdk < 42)
        opts[n_opts++] = (struct vkd3d_shader_compile_option){
            VKD3D_SHADER_COMPILE_OPTION_WARN_IMPLICIT_TRUNCATION, 0};

    struct vkd3d_shader_hlsl_source_info hlsl = {
        .type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO, .entry_point = entry, .profile = prof};
    struct vkd3d_shader_preprocess_info pre = {
        .type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO,
        .next = &hlsl,
        .macros = macros,
        .macro_count = macros != NULL ? n_macros : 0u,
        .pfn_open_include = open_include,
        .pfn_close_include = close_include,
        .include_context = &ic,
    };
    struct vkd3d_shader_compile_info info = {
        .type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO,
        .next = &pre,
        .source = {.code = guest_ptr(src), .size = len},
        .source_type = VKD3D_SHADER_SOURCE_HLSL,
        .target_type = VKD3D_SHADER_TARGET_D3D_BYTECODE,
        .options = opts,
        .option_count = n_opts,
        .log_level = VKD3D_SHADER_LOG_INFO,
    };
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;
    void *ovr_code = NULL;
    size_t ovr_size = 0;
    int ret = d3dx_shader_override(info.source.code, info.source.size, &ovr_code, &ovr_size)
                  ? (out.code = ovr_code, out.size = ovr_size, 0)
                  : compile_cached(&info, flags, include == 0, &out, &messages);

    uint32_t hr = COM_S_OK;
    if (ret < 0) {
        fprintf(stderr, "d3dx9: D3DXCompileShader(%s, %s) failed:\n%s", prof,
                entry != NULL ? entry : "main", messages != NULL ? messages : "\n");
        hr = D3DXERR_INVALIDDATA;
    } else {
        uint32_t table = 0;
        if (pp_table != 0)
            hr = ctab_create(out.code, out.size, &table);
        if (hr == COM_S_OK) {
            if (pp_table != 0)
                MEM32(pp_table) = table;
            if (pp_shader != 0)
                MEM32(pp_shader) = d3dx_buffer_new(out.code, (uint32_t)out.size);
        }
    }
    if (pp_errors != 0 && messages != NULL && messages[0] != '\0')
        MEM32(pp_errors) = d3dx_buffer_new(messages, (uint32_t)strlen(messages) + 1u);
    dump_compile(&info, flags, hr, messages, &out);
    free_macros(macros, n_macros);
    vkd3d_shader_free_messages(messages);
    vkd3d_shader_free_shader_code(&out);
    free(prof);
    free(entry);
    return hr;
}

void thunk_D3DXCompileShader(void)
{
    uint32_t hr = compile_shader(W32_ARG(0), W32_ARG(1), W32_ARG(2), W32_ARG(3), W32_ARG(4),
                                 W32_ARG(5), W32_ARG(6), W32_ARG(7), W32_ARG(8), W32_ARG(9));
    W32_RETURN(10, hr);
}
