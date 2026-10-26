/* COM objects the guest holds: the contract every interface slice codes
 * against (DirectInput8, Direct3D 9, XAudio2, anything CoCreateInstance
 * returns). The COM slice implements it in runtime/src/com_guest.c.
 *
 * Win32 x86 COM methods are __stdcall with `this` as the first stack
 * argument (not in ecx): `this` is W32_ARG(0) and the method's own arguments
 * start at W32_ARG(1). A method pops 4 + 4 * (1 + nargs) bytes.
 *
 * Guest object layout, 8 bytes in guest memory:
 *   +0  guest address of the vtable (an array of host_func_va addresses)
 *   +4  index into a host-side table of host objects
 * A host pointer is 64-bit and cannot live in a 32-bit guest slot, hence the
 * index. A guest that writes over its own object breaks the link loudly
 * (com_guest_self aborts on a bad index) instead of silently. */
#ifndef COM_GUEST_H
#define COM_GUEST_H

#include <stdint.h>

#include "recomp_types.h"

/* One interface: its method thunks in published vtable order (IUnknown's
 * QueryInterface, AddRef, Release first). The guest vtable is built once per
 * table and shared by every object of that interface. */
typedef struct {
    const char *name; /* e.g. "IDirect3DDevice9", for logs */
    const recomp_func_t *methods;
    uint32_t n_methods;
} com_interface_t;

/* Allocate a guest object of interface iface that stands for host_self.
 * Returns its guest address; aborts when guest memory or the table is full. */
uint32_t com_guest_new(const com_interface_t *iface, void *host_self);

/* The host object behind a guest `this`. Aborts with
 * `UNSUPPORTED com: bad object 0x%08X` if guest_this is not a live object. */
void *com_guest_self(uint32_t guest_this);

/* The interface a guest object was created with, or NULL. */
const com_interface_t *com_guest_iface(uint32_t guest_this);

/* Free the guest object (its 8 bytes) and its table slot. The owner calls it
 * from its Release when the reference count reaches 0; the host object is the
 * owner's to free. */
void com_guest_free(uint32_t guest_this);

/* Read and write 16-byte GUIDs in guest memory, and print one as
 * {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} into a 39-byte buffer. */
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } com_guid_t;
com_guid_t com_guid_read(uint32_t guest_addr);
int com_guid_eq(const com_guid_t *a, const com_guid_t *b);
void com_guid_str(const com_guid_t *g, char out[39]);

/* HRESULTs used across slices. */
#define COM_S_OK 0x00000000u
#define COM_S_FALSE 0x00000001u
#define COM_E_NOTIMPL 0x80004001u
#define COM_E_NOINTERFACE 0x80004002u
#define COM_E_POINTER 0x80004003u
#define COM_E_FAIL 0x80004005u
#define COM_E_OUTOFMEMORY 0x8007000Eu
#define COM_E_INVALIDARG 0x80070057u
#define COM_REGDB_E_CLASSNOTREG 0x80040154u

/* CoCreateInstance registry: an interface slice registers a factory for each
 * CLSID it serves (call from a constructor or from start-up). The factory
 * returns an HRESULT and writes the new guest object's address to *out. */
typedef uint32_t (*com_factory_t)(const com_guid_t *iid, uint32_t *out);
void com_register_class(const com_guid_t *clsid, const char *name,
                        com_factory_t factory);

#endif
