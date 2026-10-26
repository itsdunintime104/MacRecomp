/* PE32 loader: maps the original Windows image into the guest address window
 * and binds its import address table, as macho_load.c does for the Mach-O
 * games. The image has a fixed base and no relocation directory, so sections
 * go to their link-time addresses and nothing is relocated. The file is
 * untrusted: every offset and length is checked before use. */
#include <errno.h>
#include <stdint.h>

#include "pe_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_mem.h"

/* The value an import slot must hold: normally the guest address of a host
 * function thunk. 0 leaves the slot unbound. For an import by ordinal, name
 * is "" and ordinal is the number; otherwise by_ordinal is 0 and ordinal is
 * the hint. */

#define PE_MACHINE_I386 0x014Cu
#define PE_OPT_MAGIC_32 0x010Bu
#define PE_IMAGE_BASE 0x00400000u
/* The game image's entry point RVA and section count, which the loader
 * checks: the game repo's GAME_CFLAGS (-DPE_ENTRY_RVA=... -DPE_SECTION_COUNT=...). */
#if !defined(PE_ENTRY_RVA) || !defined(PE_SECTION_COUNT)
#error "set PE_ENTRY_RVA and PE_SECTION_COUNT in the game's GAME_CFLAGS"
#endif

#define PE_DIR_IMPORT 1u
#define PE_DIR_BASERELOC 5u
#define PE_DIR_ENTRY_SIZE 8u
#define PE_SECTION_HEADER_SIZE 40u
#define PE_NAME_MAX 256u

struct pe_file {
    const uint8_t *data;
    uint64_t size;
};

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int span_ok(uint64_t off, uint64_t len, uint64_t size)
{
    return off <= size && len <= size - off;
}

static int read_file(const char *path, uint8_t **data, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseeko(f, 0, SEEK_END) != 0) {
        perror("fseeko");
        fclose(f);
        return -1;
    }
    off_t len = ftello(f);
    if (len <= 0 || fseeko(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot determine file size\n", path);
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)len);
    if (buf == NULL) {
        fprintf(stderr, "%s: out of memory reading %lld bytes\n", path, (long long)len);
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "fread %s: unexpected end of file\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *data = buf;
    *size = (uint64_t)len;
    return 0;
}

static int bad_field(const char *field, uint64_t got, uint64_t want)
{
    fprintf(stderr, "pe_load_image: %s is 0x%llX, expected 0x%llX\n", field,
            (unsigned long long)got, (unsigned long long)want);
    return -1;
}

static int bad_range(const char *what)
{
    fprintf(stderr, "pe_load_image: %s lies outside the file or the image\n", what);
    return -1;
}

/* Header fields the loader uses. */
struct pe_headers {
    uint32_t entry_rva, size_of_image, size_of_headers;
    uint32_t import_rva, import_size;
    uint32_t nsections;
    uint64_t section_table; /* file offset */
};

static int parse_headers(const struct pe_file *pf, struct pe_headers *h)
{
    const uint8_t *d = pf->data;
    if (!span_ok(0, 0x40, pf->size))
        return bad_range("DOS header");
    if (d[0] != 'M' || d[1] != 'Z')
        return bad_field("DOS signature", rd16(d), 0x5A4D);

    uint32_t pe = rd32(d + 0x3C);
    /* Signature (4), COFF header (20), optional header through the data
     * directory count (96 + 4). */
    if (!span_ok(pe, 4 + 20 + 100, pf->size))
        return bad_range("e_lfanew");
    if (memcmp(d + pe, "PE\0\0", 4) != 0)
        return bad_field("PE signature", rd32(d + pe), 0x00004550);

    const uint8_t *coff = d + pe + 4;
    uint32_t machine = rd16(coff);
    if (machine != PE_MACHINE_I386)
        return bad_field("Machine", machine, PE_MACHINE_I386);
    h->nsections = rd16(coff + 2);
    if (h->nsections != PE_SECTION_COUNT)
        return bad_field("NumberOfSections", h->nsections, PE_SECTION_COUNT);
    uint32_t opt_size = rd16(coff + 16);

    const uint8_t *opt = coff + 20;
    uint32_t magic = rd16(opt);
    if (magic != PE_OPT_MAGIC_32)
        return bad_field("optional header Magic", magic, PE_OPT_MAGIC_32);
    h->entry_rva = rd32(opt + 16);
    if (h->entry_rva != PE_ENTRY_RVA)
        return bad_field("AddressOfEntryPoint", h->entry_rva, PE_ENTRY_RVA);
    uint32_t base = rd32(opt + 28);
    if (base != PE_IMAGE_BASE)
        return bad_field("ImageBase", base, PE_IMAGE_BASE);
    h->size_of_image = rd32(opt + 56);
    if (h->size_of_image == 0 || (uint64_t)base + h->size_of_image > 0xFFFFFFFFu)
        return bad_range("SizeOfImage");
    if (h->entry_rva >= h->size_of_image)
        return bad_range("AddressOfEntryPoint");
    h->size_of_headers = rd32(opt + 60);
    if (!span_ok(0, h->size_of_headers, pf->size) ||
        h->size_of_headers > h->size_of_image)
        return bad_range("SizeOfHeaders");

    uint32_t ndirs = rd32(opt + 92);
    if (ndirs <= PE_DIR_BASERELOC)
        return bad_field("NumberOfRvaAndSizes", ndirs, PE_DIR_BASERELOC + 1);
    if (opt_size < 96 + PE_DIR_ENTRY_SIZE * (uint64_t)ndirs ||
        !span_ok((uint64_t)(opt - d), opt_size, pf->size))
        return bad_range("optional header");
    const uint8_t *dirs = opt + 96;
    uint32_t reloc_size = rd32(dirs + PE_DIR_ENTRY_SIZE * PE_DIR_BASERELOC + 4);
    if (reloc_size != 0)
        return bad_field("base relocation directory size", reloc_size, 0);
    h->import_rva = rd32(dirs + PE_DIR_ENTRY_SIZE * PE_DIR_IMPORT);
    h->import_size = rd32(dirs + PE_DIR_ENTRY_SIZE * PE_DIR_IMPORT + 4);

    h->section_table = (uint64_t)(opt - d) + opt_size;
    if (!span_ok(h->section_table, (uint64_t)PE_SECTION_HEADER_SIZE * h->nsections, pf->size))
        return bad_range("section table");
    return 0;
}

static int map_sections(const struct pe_file *pf, const struct pe_headers *h)
{
    uint64_t page = (uint64_t)getpagesize();
    uint64_t start = PE_IMAGE_BASE & ~(page - 1);
    uint64_t end = ((uint64_t)PE_IMAGE_BASE + h->size_of_image + page - 1) & ~(page - 1);
    if (start < GUEST_LOW_RESERVED || end > 0x100000000ull)
        return bad_range("image address range");
    if (mprotect(guest_ptr((uint32_t)start), (size_t)(end - start), PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "mprotect image: %s\n", strerror(errno));
        return -1;
    }
    /* Zero the whole image first: the tail of each section beyond its raw
     * data (.bss) and the gaps between sections stay zero. */
    memset(guest_ptr(PE_IMAGE_BASE), 0, h->size_of_image);
    /* The MSVC start-up code reads the headers at the image base to find
     * the NT headers and the load configuration, so Windows maps them
     * too. Without them those reads see zeros. */
    memcpy(guest_ptr(PE_IMAGE_BASE), pf->data, h->size_of_headers);

    const char *verbose = getenv("RECOMP_STARTUP_VERBOSE");
    for (uint32_t i = 0; i < h->nsections; i++) {
        const uint8_t *s = pf->data + h->section_table + (uint64_t)PE_SECTION_HEADER_SIZE * i;
        char name[9] = {0};
        memcpy(name, s, 8);
        uint32_t vsz = rd32(s + 8), va = rd32(s + 12), rsz = rd32(s + 16), raw = rd32(s + 20);
        uint32_t n = rsz < vsz ? rsz : vsz;
        if (!span_ok(va, vsz, h->size_of_image) || !span_ok(raw, n, pf->size)) {
            fprintf(stderr, "pe_load_image: section %s lies outside the file or the image\n",
                    name);
            return -1;
        }
        memcpy(guest_ptr(PE_IMAGE_BASE + va), pf->data + raw, n);
        if (verbose)
            fprintf(stderr, "pe_load: %-8s va 0x%08X vsz 0x%08X raw 0x%08X copied 0x%08X\n",
                    name, PE_IMAGE_BASE + va, vsz, raw, n);
    }
    return 0;
}

/* NUL-terminated string at rva inside the mapped image, or NULL. */
static const char *image_str(const struct pe_headers *h, uint32_t rva)
{
    if (rva >= h->size_of_image)
        return NULL;
    uint32_t room = h->size_of_image - rva;
    if (room > PE_NAME_MAX)
        room = PE_NAME_MAX;
    const char *s = guest_ptr(PE_IMAGE_BASE + rva);
    return memchr(s, 0, room) != NULL ? s : NULL;
}

static int bind_imports(const struct pe_headers *h, pe_import_resolver_t resolve,
                        uint32_t *unbound)
{
    const char *verbose = getenv("RECOMP_STARTUP_VERBOSE");
    *unbound = 0;
    if (h->import_rva == 0 || h->import_size == 0)
        return 0;
    if (!span_ok(h->import_rva, h->import_size, h->size_of_image))
        return bad_range("import directory");

    /* 20-byte descriptors; the last one is all zero. */
    for (uint32_t off = 0; off + 20 <= h->import_size; off += 20) {
        const uint8_t *desc = guest_ptr(PE_IMAGE_BASE + h->import_rva + off);
        uint32_t ilt = rd32(desc), name_rva = rd32(desc + 12), iat = rd32(desc + 16);
        if (name_rva == 0 && iat == 0)
            return 0;
        const char *dll = image_str(h, name_rva);
        if (dll == NULL)
            return bad_range("import DLL name");
        if (ilt == 0)
            ilt = iat; /* no separate lookup table: the IAT still holds the names */

        for (uint32_t k = 0;; k++) {
            if (!span_ok(ilt + 4ull * k, 4, h->size_of_image) ||
                !span_ok(iat + 4ull * k, 4, h->size_of_image))
                return bad_range("import thunk table");
            uint32_t entry = rd32(guest_ptr(PE_IMAGE_BASE + ilt + 4 * k));
            if (entry == 0)
                break;
            const char *name = "";
            uint16_t ordinal;
            int by_ordinal = (entry & 0x80000000u) != 0;
            if (by_ordinal) {
                ordinal = (uint16_t)(entry & 0xFFFFu);
            } else {
                /* IMAGE_IMPORT_BY_NAME: 2-byte hint, then the name. */
                name = image_str(h, entry + 2);
                if (name == NULL)
                    return bad_range("import function name");
                ordinal = rd16(guest_ptr(PE_IMAGE_BASE + entry));
            }
            uint32_t value = resolve != NULL ? resolve(dll, name, ordinal, by_ordinal) : 0;
            uint32_t slot = PE_IMAGE_BASE + iat + 4 * k;
            memcpy(guest_ptr(slot), &value, 4);
            if (value == 0) {
                (*unbound)++;
                if (verbose) {
                    if (by_ordinal)
                        fprintf(stderr, "pe_load: unbound %s ordinal %u (slot 0x%08X)\n", dll,
                                ordinal, slot);
                    else
                        fprintf(stderr, "pe_load: unbound %s!%s (slot 0x%08X)\n", dll, name,
                                slot);
                }
            }
        }
    }
    return bad_range("import directory terminator");
}

int pe_load_image(const char *path, pe_import_resolver_t resolve, pe_image_t *out)
{
    if (g_guest_base == NULL) {
        fprintf(stderr, "pe_load_image: guest_mem_init not called\n");
        return -1;
    }
    struct pe_file pf;
    uint8_t *data;
    if (read_file(path, &data, &pf.size) != 0)
        return -1;
    pf.data = data;

    struct pe_headers h;
    uint32_t unbound = 0;
    int rc = parse_headers(&pf, &h);
    if (rc == 0)
        rc = map_sections(&pf, &h);
    if (rc == 0)
        rc = bind_imports(&h, resolve, &unbound);
    free(data);
    if (rc != 0)
        return rc;

    out->image_base = PE_IMAGE_BASE;
    out->image_end = PE_IMAGE_BASE + h.size_of_image;
    out->entry_va = PE_IMAGE_BASE + h.entry_rva;
    out->unbound_imports = unbound;
    return 0;
}
