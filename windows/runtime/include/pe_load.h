/* Load the game's original Windows PE32 image into guest memory.
 *
 * The Mach-O games use macho_load.c for this. A PE needs no relocation pass
 * and no dynamic loader: the image has a fixed base, so the loader maps the
 * sections, zero-fills what has no file bytes, and binds the import table. */
#ifndef PE_LOAD_H
#define PE_LOAD_H

#include <stdint.h>

/* Returns the guest address to write into one IAT slot, or 0 to leave it
 * unbound. `name` is "" for an import by ordinal. */
typedef uint32_t (*pe_import_resolver_t)(const char *dll, const char *name,
                                         uint16_t ordinal, int by_ordinal);

typedef struct {
    uint32_t image_base;      /* guest address of RVA 0 */
    uint32_t image_end;       /* image_base + SizeOfImage */
    uint32_t entry_va;        /* image_base + AddressOfEntryPoint */
    uint32_t unbound_imports; /* IAT slots the resolver returned 0 for */
} pe_image_t;

/* Map the PE32 file at path, bind its imports through resolve, and fill *out.
 * Returns 0 on success. Requires guest_mem_init(). */
int pe_load_image(const char *path, pe_import_resolver_t resolve,
                  pe_image_t *out);

#endif
