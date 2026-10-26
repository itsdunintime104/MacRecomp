/* Guest COM objects, GUIDs and the CoCreateInstance class registry.
 * Contract: runtime/include/com_guest.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "com_guest.h"
#include "frame_trace.h"

#define COM_OBJ_SIZE 8u
#define COM_OBJ_MAX 65536u
#define COM_CLASS_MAX 64u

/* ---- Objects ----------------------------------------------------------- */

/* One guest vtable per interface table, built on first use. */
struct com_vtbl {
    struct com_vtbl *next;
    const com_interface_t *iface;
    uint32_t guest; /* guest address of the n_methods slots */
};

/* A table entry is live while guest != 0. */
struct com_slot {
    void *host_self;
    const com_interface_t *iface;
    uint32_t guest; /* the guest object that points at this entry */
};

static struct com_vtbl *g_vtbls;
static struct com_slot g_slots[COM_OBJ_MAX];
static uint32_t g_next_slot;
static pthread_mutex_t g_com_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds g_com_lock. */
static uint32_t vtbl_for(const com_interface_t *iface)
{
    for (struct com_vtbl *v = g_vtbls; v != NULL; v = v->next)
        if (v->iface == iface)
            return v->guest;
    struct com_vtbl *v = malloc(sizeof *v);
    uint32_t guest = guest_malloc(4u * iface->n_methods);
    if (v == NULL || guest == 0) {
        fprintf(stderr, "com_guest_new: out of memory for the %s vtable\n", iface->name);
        abort();
    }
    for (uint32_t i = 0; i < iface->n_methods; i++)
        MEM32(guest + 4u * i) = ft_wrap_method(iface->methods[i], iface->name, i);
    v->iface = iface;
    v->guest = guest;
    v->next = g_vtbls;
    g_vtbls = v;
    return guest;
}

uint32_t com_guest_new(const com_interface_t *iface, void *host_self)
{
    pthread_mutex_lock(&g_com_lock);
    uint32_t vtbl = vtbl_for(iface);
    uint32_t idx = g_next_slot, n = 0;
    while (n < COM_OBJ_MAX && g_slots[idx].guest != 0) {
        idx = (idx + 1u) % COM_OBJ_MAX;
        n++;
    }
    uint32_t obj = n < COM_OBJ_MAX ? guest_malloc(COM_OBJ_SIZE) : 0;
    if (obj == 0) {
        fprintf(stderr, "com_guest_new: %s for %s\n",
                n < COM_OBJ_MAX ? "guest heap full" : "object table full", iface->name);
        abort();
    }
    MEM32(obj) = vtbl;
    MEM32(obj + 4u) = idx;
    g_slots[idx] = (struct com_slot){host_self, iface, obj};
    g_next_slot = (idx + 1u) % COM_OBJ_MAX;
    pthread_mutex_unlock(&g_com_lock);
    return obj;
}

/* The live entry for guest_this, or NULL. Objects only come from the guest
 * heap, so the range check also makes the +4 read safe. Caller holds
 * g_com_lock. */
static struct com_slot *slot_of(uint32_t guest_this)
{
    if (guest_this < GUEST_HEAP_BASE || guest_this > GUEST_HEAP_END - COM_OBJ_SIZE)
        return NULL;
    uint32_t idx = MEM32(guest_this + 4u);
    if (idx >= COM_OBJ_MAX || g_slots[idx].guest != guest_this)
        return NULL;
    return &g_slots[idx];
}

static void bad_object(uint32_t guest_this)
{
    fprintf(stderr, "UNSUPPORTED com: bad object 0x%08X (guest return 0x%08X)\n", guest_this,
            MEM32(g_esp));
    abort();
}

void *com_guest_self(uint32_t guest_this)
{
    pthread_mutex_lock(&g_com_lock);
    struct com_slot *s = slot_of(guest_this);
    void *self = s != NULL ? s->host_self : NULL;
    pthread_mutex_unlock(&g_com_lock);
    if (s == NULL)
        bad_object(guest_this);
    return self;
}

const com_interface_t *com_guest_iface(uint32_t guest_this)
{
    pthread_mutex_lock(&g_com_lock);
    struct com_slot *s = slot_of(guest_this);
    const com_interface_t *iface = s != NULL ? s->iface : NULL;
    pthread_mutex_unlock(&g_com_lock);
    return iface;
}

void com_guest_free(uint32_t guest_this)
{
    pthread_mutex_lock(&g_com_lock);
    struct com_slot *s = slot_of(guest_this);
    if (s != NULL) {
        *s = (struct com_slot){0};
        MEM32(guest_this) = 0;
        MEM32(guest_this + 4u) = 0;
        guest_free(guest_this);
    }
    pthread_mutex_unlock(&g_com_lock);
    if (s == NULL)
        bad_object(guest_this);
}

/* ---- GUIDs ------------------------------------------------------------- */

com_guid_t com_guid_read(uint32_t guest_addr)
{
    com_guid_t g;
    g.d1 = MEM32(guest_addr);
    g.d2 = MEM16(guest_addr + 4u);
    g.d3 = MEM16(guest_addr + 6u);
    for (uint32_t i = 0; i < 8; i++)
        g.d4[i] = MEM8(guest_addr + 8u + i);
    return g;
}

int com_guid_eq(const com_guid_t *a, const com_guid_t *b)
{
    return a->d1 == b->d1 && a->d2 == b->d2 && a->d3 == b->d3 &&
           memcmp(a->d4, b->d4, sizeof a->d4) == 0;
}

void com_guid_str(const com_guid_t *g, char out[39])
{
    snprintf(out, 39, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->d1, g->d2,
             g->d3, g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6],
             g->d4[7]);
}

/* ---- Class registry ---------------------------------------------------- */

struct com_class {
    com_guid_t clsid;
    const char *name;
    com_factory_t factory;
};

static struct com_class g_classes[COM_CLASS_MAX];
static uint32_t g_class_count;
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;

void com_register_class(const com_guid_t *clsid, const char *name, com_factory_t factory)
{
    char s[39];
    pthread_mutex_lock(&g_class_lock);
    for (uint32_t i = 0; i < g_class_count; i++) {
        if (com_guid_eq(&g_classes[i].clsid, clsid)) {
            com_guid_str(clsid, s);
            fprintf(stderr, "com_register_class: %s registered twice (%s, %s)\n", s,
                    g_classes[i].name, name);
            abort();
        }
    }
    if (g_class_count == COM_CLASS_MAX) {
        fprintf(stderr, "com_register_class: table full (%u classes)\n", COM_CLASS_MAX);
        abort();
    }
    g_classes[g_class_count++] = (struct com_class){*clsid, name, factory};
    pthread_mutex_unlock(&g_class_lock);
}

/* The factory registered for clsid, or NULL. Used by CoCreateInstance
 * (win32_com.c). */
com_factory_t com_class_factory(const com_guid_t *clsid)
{
    com_factory_t f = NULL;
    pthread_mutex_lock(&g_class_lock);
    for (uint32_t i = 0; i < g_class_count && f == NULL; i++)
        if (com_guid_eq(&g_classes[i].clsid, clsid))
            f = g_classes[i].factory;
    pthread_mutex_unlock(&g_class_lock);
    return f;
}
