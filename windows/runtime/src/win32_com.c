/* Win32 COM slice: the 7 ole32/oleaut32 imports (notes/slice_owners.md, slice
 * "com"). Guest COM objects and the class registry are in com_guest.c.
 * Contract: runtime/include/win32_thunk.h and docs/WIN32_THUNKS.md. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "com_guest.h"
#include "win32_thunk.h"

/* Defined in com_guest.c. */
com_factory_t com_class_factory(const com_guid_t *clsid);

#define CLASS_E_NOAGGREGATION 0x80040110u

/* ---- ole32 ------------------------------------------------------------- */

static _Thread_local uint32_t t_com_inits;

/* HRESULT CoInitialize(LPVOID reserved) */
static void imp_CoInitialize(void)
{
    W32_RETURN(1, t_com_inits++ == 0 ? COM_S_OK : COM_S_FALSE);
}

/* HRESULT CoInitializeEx(LPVOID reserved, DWORD coinit) */
static void imp_CoInitializeEx(void)
{
    W32_RETURN(2, t_com_inits++ == 0 ? COM_S_OK : COM_S_FALSE);
}

/* void CoUninitialize(void) */
static void imp_CoUninitialize(void)
{
    if (t_com_inits > 0)
        t_com_inits--;
    W32_RETURN(0, 0);
}

/* Classes Windows XP does not have and the game handles being absent: no
 * UNSUPPORTED line. Game Explorer (WinMain registers the game with
 * it) is a Vista feature. */
static const com_guid_t k_absent_on_xp[] = {
    {0x9A5EA990u, 0x3034u, 0x4D6Fu, {0x91, 0x28, 0x01, 0xF3, 0xC6, 0x10, 0x22, 0xBC}},
};

static int absent_on_xp(const com_guid_t *clsid)
{
    for (size_t i = 0; i < sizeof k_absent_on_xp / sizeof k_absent_on_xp[0]; i++)
        if (com_guid_eq(&k_absent_on_xp[i], clsid))
            return 1;
    return 0;
}

/* Print the UNSUPPORTED line for an unregistered CLSID once per CLSID: it
 * names the COM classes the game asks for. */
static void log_unregistered(const com_guid_t *clsid, const com_guid_t *iid, uint32_t ret)
{
    struct seen {
        struct seen *next;
        com_guid_t clsid;
    };
    static struct seen *head;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (struct seen *s = head; s != NULL; s = s->next) {
        if (com_guid_eq(&s->clsid, clsid)) {
            pthread_mutex_unlock(&lock);
            return;
        }
    }
    struct seen *s = malloc(sizeof *s);
    if (s != NULL) {
        s->clsid = *clsid;
        s->next = head;
        head = s;
    }
    pthread_mutex_unlock(&lock);
    char c[39], i[39];
    com_guid_str(clsid, c);
    com_guid_str(iid, i);
    fprintf(stderr,
            "UNSUPPORTED ole32.dll!CoCreateInstance: CLSID %s IID %s (guest return 0x%08X)\n", c,
            i, ret);
}

/* HRESULT CoCreateInstance(REFCLSID, LPUNKNOWN outer, DWORD clsctx, REFIID, LPVOID *ppv) */
static void imp_CoCreateInstance(void)
{
    com_guid_t clsid = com_guid_read(W32_ARG(0)), iid = com_guid_read(W32_ARG(3));
    uint32_t outer = W32_ARG(1), ppv = W32_ARG(4);
    uint32_t hr;
    com_factory_t factory = com_class_factory(&clsid);
    if (outer != 0) {
        hr = CLASS_E_NOAGGREGATION;
    } else if (factory != NULL) {
        uint32_t obj = 0;
        hr = factory(&iid, &obj);
        MEM32(ppv) = obj;
    } else {
        MEM32(ppv) = 0;
        if (!absent_on_xp(&clsid))
            log_unregistered(&clsid, &iid, MEM32(g_esp));
        hr = COM_REGDB_E_CLASSNOTREG;
    }
    W32_RETURN(5, hr);
}

/* HRESULT CoSetProxyBlanket(IUnknown *, DWORD authn, DWORD authz, OLECHAR *principal,
 *                           DWORD level, DWORD imp, RPC_AUTH_IDENTITY_HANDLE, DWORD caps) */
static void imp_CoSetProxyBlanket(void)
{
    W32_RETURN(8, COM_S_OK);
}

/* ---- oleaut32 ---------------------------------------------------------- */

/* BSTR SysAllocString(const OLECHAR *). A BSTR is UTF-16 preceded by its
 * byte length (4 bytes) and followed by a 2-byte NUL; the BSTR points just
 * after the length. */
static void imp_SysAllocString(void)
{
    uint32_t src = W32_ARG(0), bstr = 0;
    if (src != 0) {
        uint32_t len = 0;
        while (MEM16(src + len) != 0)
            len += 2u;
        g_heap_site = MEM32(g_esp);
        uint32_t block = guest_malloc(4u + len + 2u);
        g_heap_site = 0;
        if (block != 0) {
            MEM32(block) = len;
            memcpy(guest_ptr(block + 4u), guest_ptr(src), len);
            MEM16(block + 4u + len) = 0;
            bstr = block + 4u;
        }
    }
    W32_RETURN(1, bstr);
}

/* void SysFreeString(BSTR) */
static void imp_SysFreeString(void)
{
    uint32_t bstr = W32_ARG(0);
    if (bstr != 0) {
        g_heap_site = MEM32(g_esp);
        guest_free(bstr - 4u);
        g_heap_site = 0;
    }
    W32_RETURN(1, 0);
}

const win32_export_t win32_exports_com[] = {
    {"OLEAUT32.dll", NULL, 2, imp_SysAllocString},
    {"OLEAUT32.dll", NULL, 6, imp_SysFreeString},
    {"ole32.dll", "CoCreateInstance", 0, imp_CoCreateInstance},
    {"ole32.dll", "CoInitialize", 0, imp_CoInitialize},
    {"ole32.dll", "CoInitializeEx", 0, imp_CoInitializeEx},
    {"ole32.dll", "CoSetProxyBlanket", 0, imp_CoSetProxyBlanket},
    {"ole32.dll", "CoUninitialize", 0, imp_CoUninitialize},
    {0},
};
