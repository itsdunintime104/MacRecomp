/* WMI slice: CoCreateInstance(CLSID_WbemLocator) and the four interfaces of
 * Microsoft's IsXInputDevice sample (IWbemLocator, IWbemServices,
 * IEnumWbemClassObject, IWbemClassObject). A game runs that sample on each
 * pad EnumDevices lists (win32_input.c) to learn that the pad is an XInput
 * device: Win32_PNPEntity holds one object per pad, with a DeviceID that
 * names an Xbox 360 pad (VID 045E, PID 028E) and contains "IG_". Every
 * other method returns E_NOTIMPL. Contract: com_guest.h; method order and
 * argument counts from the mingw-w64 i686 wbemcli.h. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "com_guest.h"
#include "diag.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "win32_thunk.h"

/* Defined in win32_input.c and win32_com.c. */
uint32_t w32_input_pads(void);
void w32_input_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t w32_bstr_alloc(const void *utf16, uint32_t len);

#define WBEM_E_NOT_FOUND 0x80041002u
#define WBEM_E_INVALID_CLASS 0x80041010u
#define VT_BSTR 8u
#define CIM_STRING 8u
/* VARIANT, 16 bytes: vt, three reserved WORDs, then the value. */
#define VARIANT_SIZE 16u
#define VARIANT_VT 0u
#define VARIANT_VAL 8u

static const com_guid_t CLSID_WbemLocator = {
    0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
static const com_guid_t IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const com_guid_t IID_IWbemLocator = {
    0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
static const com_guid_t IID_IWbemServices = {
    0x9556DC99, 0x828C, 0x11CF, {0xA3, 0x7E, 0x00, 0xAA, 0x00, 0x32, 0x40, 0xC7}};
static const com_guid_t IID_IEnumWbemClassObject = {
    0x027947E1, 0xD731, 0x11CE, {0xA3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
static const com_guid_t IID_IWbemClassObject = {
    0xDC12A681, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};

/* One host object for all four interfaces. */
struct wmi_obj {
    int refs;
    const com_guid_t *iid; /* the interface the object is */
    uint32_t n_pads, next; /* IEnumWbemClassObject: snapshot and position */
    uint32_t pad;          /* IWbemClassObject: pad number */
};

static pthread_mutex_t s_wmi_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t wmi_new(const com_interface_t *iface, const com_guid_t *iid)
{
    struct wmi_obj *o = calloc(1, sizeof *o);
    o->refs = 1;
    o->iid = iid;
    return com_guest_new(iface, o);
}

static struct wmi_obj *wmi_self(void)
{
    return com_guest_self(W32_ARG(0));
}

/* 1 when the guest UTF-16 string at w equals ascii, ignoring ASCII case. */
static int wstr_ieq(uint32_t w, const char *ascii)
{
    if (w == 0)
        return 0;
    for (;; w += 2u, ascii++) {
        uint16_t c = MEM16(w);
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        char a = *ascii >= 'a' && *ascii <= 'z' ? (char)(*ascii - ('a' - 'A')) : *ascii;
        if (c != (uint8_t)a)
            return 0;
        if (c == 0)
            return 1;
    }
}

/* ---- IUnknown, shared ---------------------------------------------------- */

/* QueryInterface(REFIID riid, void **ppvObject): only the interface itself. */
static void wmi_QueryInterface(void)
{
    uint32_t self = W32_ARG(0), ppv = W32_ARG(2);
    com_guid_t iid = com_guid_read(W32_ARG(1));
    uint32_t hr = COM_E_NOINTERFACE;
    struct wmi_obj *o = wmi_self();
    if (com_guid_eq(&iid, o->iid) || com_guid_eq(&iid, &IID_IUnknown)) {
        pthread_mutex_lock(&s_wmi_lock);
        o->refs++;
        pthread_mutex_unlock(&s_wmi_lock);
        hr = COM_S_OK;
    } else {
        char s[39];
        com_guid_str(&iid, s);
        diag_unsupported("wbem", MEM32(g_esp), "%s::QueryInterface %s",
                         com_guest_iface(self)->name, s);
        self = 0;
    }
    if (ppv)
        MEM32(ppv) = self;
    W32_RETURN(3, hr);
}

static void wmi_AddRef(void)
{
    struct wmi_obj *o = wmi_self();
    pthread_mutex_lock(&s_wmi_lock);
    int refs = ++o->refs;
    pthread_mutex_unlock(&s_wmi_lock);
    W32_RETURN(1, refs);
}

static void wmi_Release(void)
{
    uint32_t self = W32_ARG(0);
    struct wmi_obj *o = wmi_self();
    pthread_mutex_lock(&s_wmi_lock);
    int refs = --o->refs;
    pthread_mutex_unlock(&s_wmi_lock);
    if (refs == 0) {
        com_guest_free(self);
        free(o);
    }
    W32_RETURN(1, refs);
}

/* A method that is not implemented: UNSUPPORTED once, E_NOTIMPL. nargs
 * excludes `this`. */
#define WMI_NOTIMPL(iface, method, nargs)                                    \
    static void iface##_##method(void)                                       \
    {                                                                        \
        diag_unsupported("wbem", MEM32(g_esp), "%s::%s", #iface, #method);   \
        W32_RETURN(1 + (nargs), COM_E_NOTIMPL);                              \
    }

/* ---- IWbemClassObject ---------------------------------------------------- */

/* Get(LPCWSTR wszName, LONG lFlags, VARIANT *pVal, CIMTYPE *pType,
 *     LONG *plFlavor): DeviceID only. */
static void IWbemClassObject_Get(void)
{
    struct wmi_obj *o = wmi_self();
    uint32_t name = W32_ARG(1), var = W32_ARG(3), type = W32_ARG(4), flavor = W32_ARG(5);
    if (!wstr_ieq(name, "DeviceID")) {
        W32_RETURN(6, WBEM_E_NOT_FOUND);
        return;
    }
    char id[64];
    int n = snprintf(id, sizeof id, "HID\\VID_045E&PID_028E&IG_00\\8&PAD&0&%04u", o->pad);
    uint16_t w[64];
    for (int i = 0; i < n; i++)
        w[i] = (uint8_t)id[i];
    if (var) {
        memset(guest_ptr(var), 0, VARIANT_SIZE);
        MEM16(var + VARIANT_VT) = VT_BSTR;
        MEM32(var + VARIANT_VAL) = w32_bstr_alloc(w, 2u * (uint32_t)n);
    }
    w32_input_trace("WMI Get DeviceID -> %s", id);
    if (type)
        MEM32(type) = CIM_STRING;
    if (flavor)
        MEM32(flavor) = 0; /* WBEM_FLAVOR_ORIGIN_LOCAL */
    W32_RETURN(6, COM_S_OK);
}

WMI_NOTIMPL(IWbemClassObject, GetQualifierSet, 1)
WMI_NOTIMPL(IWbemClassObject, Put, 4)
WMI_NOTIMPL(IWbemClassObject, Delete, 1)
WMI_NOTIMPL(IWbemClassObject, GetNames, 4)
WMI_NOTIMPL(IWbemClassObject, BeginEnumeration, 1)
WMI_NOTIMPL(IWbemClassObject, Next, 5)
WMI_NOTIMPL(IWbemClassObject, EndEnumeration, 0)
WMI_NOTIMPL(IWbemClassObject, GetPropertyQualifierSet, 2)
WMI_NOTIMPL(IWbemClassObject, Clone, 1)
WMI_NOTIMPL(IWbemClassObject, GetObjectText, 2)
WMI_NOTIMPL(IWbemClassObject, SpawnDerivedClass, 2)
WMI_NOTIMPL(IWbemClassObject, SpawnInstance, 2)
WMI_NOTIMPL(IWbemClassObject, CompareTo, 2)
WMI_NOTIMPL(IWbemClassObject, GetPropertyOrigin, 2)
WMI_NOTIMPL(IWbemClassObject, InheritsFrom, 1)
WMI_NOTIMPL(IWbemClassObject, GetMethod, 4)
WMI_NOTIMPL(IWbemClassObject, PutMethod, 4)
WMI_NOTIMPL(IWbemClassObject, DeleteMethod, 1)
WMI_NOTIMPL(IWbemClassObject, BeginMethodEnumeration, 1)
WMI_NOTIMPL(IWbemClassObject, NextMethod, 4)
WMI_NOTIMPL(IWbemClassObject, EndMethodEnumeration, 0)
WMI_NOTIMPL(IWbemClassObject, GetMethodQualifierSet, 2)
WMI_NOTIMPL(IWbemClassObject, GetMethodOrigin, 2)

/* wbemcli.h order (IWbemClassObject, 27 methods). */
static const recomp_func_t IWbemClassObject_methods[] = {
    wmi_QueryInterface,
    wmi_AddRef,
    wmi_Release,
    IWbemClassObject_GetQualifierSet,
    IWbemClassObject_Get,
    IWbemClassObject_Put,
    IWbemClassObject_Delete,
    IWbemClassObject_GetNames,
    IWbemClassObject_BeginEnumeration,
    IWbemClassObject_Next,
    IWbemClassObject_EndEnumeration,
    IWbemClassObject_GetPropertyQualifierSet,
    IWbemClassObject_Clone,
    IWbemClassObject_GetObjectText,
    IWbemClassObject_SpawnDerivedClass,
    IWbemClassObject_SpawnInstance,
    IWbemClassObject_CompareTo,
    IWbemClassObject_GetPropertyOrigin,
    IWbemClassObject_InheritsFrom,
    IWbemClassObject_GetMethod,
    IWbemClassObject_PutMethod,
    IWbemClassObject_DeleteMethod,
    IWbemClassObject_BeginMethodEnumeration,
    IWbemClassObject_NextMethod,
    IWbemClassObject_EndMethodEnumeration,
    IWbemClassObject_GetMethodQualifierSet,
    IWbemClassObject_GetMethodOrigin,
};
static const com_interface_t IWbemClassObject_iface = {
    "IWbemClassObject", IWbemClassObject_methods,
    sizeof IWbemClassObject_methods / sizeof IWbemClassObject_methods[0]};

/* ---- IEnumWbemClassObject ------------------------------------------------ */

/* Next(LONG lTimeout, ULONG uCount, IWbemClassObject **apObjects,
 *      ULONG *puReturned): S_OK when uCount objects came back, else S_FALSE. */
static void IEnumWbemClassObject_Next(void)
{
    struct wmi_obj *o = wmi_self();
    uint32_t count = W32_ARG(2), objs = W32_ARG(3), returned = W32_ARG(4);
    uint32_t n = 0;
    for (; n < count && o->next < o->n_pads; n++, o->next++) {
        uint32_t obj = wmi_new(&IWbemClassObject_iface, &IID_IWbemClassObject);
        ((struct wmi_obj *)com_guest_self(obj))->pad = o->next;
        MEM32(objs + 4u * n) = obj;
    }
    if (returned)
        MEM32(returned) = n;
    w32_input_trace("WMI Next %u -> %u objects", count, n);
    W32_RETURN(5, n == count ? COM_S_OK : COM_S_FALSE);
}

WMI_NOTIMPL(IEnumWbemClassObject, Reset, 0)
WMI_NOTIMPL(IEnumWbemClassObject, NextAsync, 2)
WMI_NOTIMPL(IEnumWbemClassObject, Clone, 1)
WMI_NOTIMPL(IEnumWbemClassObject, Skip, 2)

/* wbemcli.h order (IEnumWbemClassObject, 8 methods). */
static const recomp_func_t IEnumWbemClassObject_methods[] = {
    wmi_QueryInterface,
    wmi_AddRef,
    wmi_Release,
    IEnumWbemClassObject_Reset,
    IEnumWbemClassObject_Next,
    IEnumWbemClassObject_NextAsync,
    IEnumWbemClassObject_Clone,
    IEnumWbemClassObject_Skip,
};
static const com_interface_t IEnumWbemClassObject_iface = {
    "IEnumWbemClassObject", IEnumWbemClassObject_methods,
    sizeof IEnumWbemClassObject_methods / sizeof IEnumWbemClassObject_methods[0]};

/* ---- IWbemServices ------------------------------------------------------- */

/* CreateInstanceEnum(const BSTR strFilter, LONG lFlags, IWbemContext *pCtx,
 *                    IEnumWbemClassObject **ppEnum): Win32_PNPEntity only,
 * with the pads attached now. */
static void IWbemServices_CreateInstanceEnum(void)
{
    uint32_t cls = W32_ARG(1), out = W32_ARG(4);
    if (!wstr_ieq(cls, "Win32_PNPEntity")) {
        char a[64];
        size_t i = 0;
        for (; cls && i < sizeof a - 1 && MEM16(cls + 2u * i); i++)
            a[i] = (char)MEM16(cls + 2u * i);
        a[i] = 0;
        diag_unsupported("wbem", MEM32(g_esp), "IWbemServices::CreateInstanceEnum %s", a);
        if (out)
            MEM32(out) = 0;
        W32_RETURN(5, WBEM_E_INVALID_CLASS);
        return;
    }
    uint32_t obj = wmi_new(&IEnumWbemClassObject_iface, &IID_IEnumWbemClassObject);
    ((struct wmi_obj *)com_guest_self(obj))->n_pads = w32_input_pads();
    MEM32(out) = obj;
    W32_RETURN(5, COM_S_OK);
}

WMI_NOTIMPL(IWbemServices, OpenNamespace, 5)
WMI_NOTIMPL(IWbemServices, CancelAsyncCall, 1)
WMI_NOTIMPL(IWbemServices, QueryObjectSink, 2)
WMI_NOTIMPL(IWbemServices, GetObject, 5)
WMI_NOTIMPL(IWbemServices, GetObjectAsync, 4)
WMI_NOTIMPL(IWbemServices, PutClass, 4)
WMI_NOTIMPL(IWbemServices, PutClassAsync, 4)
WMI_NOTIMPL(IWbemServices, DeleteClass, 4)
WMI_NOTIMPL(IWbemServices, DeleteClassAsync, 4)
WMI_NOTIMPL(IWbemServices, CreateClassEnum, 4)
WMI_NOTIMPL(IWbemServices, CreateClassEnumAsync, 4)
WMI_NOTIMPL(IWbemServices, PutInstance, 4)
WMI_NOTIMPL(IWbemServices, PutInstanceAsync, 4)
WMI_NOTIMPL(IWbemServices, DeleteInstance, 4)
WMI_NOTIMPL(IWbemServices, DeleteInstanceAsync, 4)
WMI_NOTIMPL(IWbemServices, CreateInstanceEnumAsync, 4)
WMI_NOTIMPL(IWbemServices, ExecQuery, 5)
WMI_NOTIMPL(IWbemServices, ExecQueryAsync, 5)
WMI_NOTIMPL(IWbemServices, ExecNotificationQuery, 5)
WMI_NOTIMPL(IWbemServices, ExecNotificationQueryAsync, 5)
WMI_NOTIMPL(IWbemServices, ExecMethod, 7)
WMI_NOTIMPL(IWbemServices, ExecMethodAsync, 6)

/* wbemcli.h order (IWbemServices, 26 methods). */
static const recomp_func_t IWbemServices_methods[] = {
    wmi_QueryInterface,
    wmi_AddRef,
    wmi_Release,
    IWbemServices_OpenNamespace,
    IWbemServices_CancelAsyncCall,
    IWbemServices_QueryObjectSink,
    IWbemServices_GetObject,
    IWbemServices_GetObjectAsync,
    IWbemServices_PutClass,
    IWbemServices_PutClassAsync,
    IWbemServices_DeleteClass,
    IWbemServices_DeleteClassAsync,
    IWbemServices_CreateClassEnum,
    IWbemServices_CreateClassEnumAsync,
    IWbemServices_PutInstance,
    IWbemServices_PutInstanceAsync,
    IWbemServices_DeleteInstance,
    IWbemServices_DeleteInstanceAsync,
    IWbemServices_CreateInstanceEnum,
    IWbemServices_CreateInstanceEnumAsync,
    IWbemServices_ExecQuery,
    IWbemServices_ExecQueryAsync,
    IWbemServices_ExecNotificationQuery,
    IWbemServices_ExecNotificationQueryAsync,
    IWbemServices_ExecMethod,
    IWbemServices_ExecMethodAsync,
};
static const com_interface_t IWbemServices_iface = {
    "IWbemServices", IWbemServices_methods,
    sizeof IWbemServices_methods / sizeof IWbemServices_methods[0]};

/* ---- IWbemLocator -------------------------------------------------------- */

/* ConnectServer(const BSTR strNetworkResource, const BSTR strUser,
 *               const BSTR strPassword, const BSTR strLocale,
 *               LONG lSecurityFlags, const BSTR strAuthority,
 *               IWbemContext *pCtx, IWbemServices **ppNamespace): any
 * namespace. */
static void IWbemLocator_ConnectServer(void)
{
    MEM32(W32_ARG(8)) = wmi_new(&IWbemServices_iface, &IID_IWbemServices);
    W32_RETURN(9, COM_S_OK);
}

/* wbemcli.h order (IWbemLocator, 4 methods). */
static const recomp_func_t IWbemLocator_methods[] = {
    wmi_QueryInterface,
    wmi_AddRef,
    wmi_Release,
    IWbemLocator_ConnectServer,
};
static const com_interface_t IWbemLocator_iface = {
    "IWbemLocator", IWbemLocator_methods,
    sizeof IWbemLocator_methods / sizeof IWbemLocator_methods[0]};

static uint32_t wbem_locator_factory(const com_guid_t *iid, uint32_t *out)
{
    if (!com_guid_eq(iid, &IID_IWbemLocator) && !com_guid_eq(iid, &IID_IUnknown)) {
        *out = 0;
        return COM_E_NOINTERFACE;
    }
    *out = wmi_new(&IWbemLocator_iface, &IID_IWbemLocator);
    return COM_S_OK;
}

__attribute__((constructor)) static void wmi_register(void)
{
    com_register_class(&CLSID_WbemLocator, "WbemLocator", wbem_locator_factory);
}
