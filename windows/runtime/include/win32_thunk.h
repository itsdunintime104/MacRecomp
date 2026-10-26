/* Win32 import thunks: the contract every slice codes against.
 * Read docs/WIN32_THUNKS.md first; notes/slice_owners.md says which slice
 * owns which import. A slice writes only its own runtime/src/win32_<slice>.c
 * and its own test, and defines only its own export table. */
#ifndef WIN32_THUNK_H
#define WIN32_THUNK_H

#include <stdint.h>

#include "recomp_types.h"

/* ---- Calling convention ------------------------------------------------ */

/* Argument i (0-based) of a stdcall import; [g_esp] is the return address. */
#define W32_ARG(i) MEM32(g_esp + 4u + 4u * (uint32_t)(i))

/* Set the return value and pop the return address plus nargs 32-bit slots,
 * as a stdcall callee does. nargs comes from notes/win32_imports.md
 * ("Args bytes popped" / 4). */
#define W32_RETURN(nargs, value)                    \
    do {                                            \
        g_eax = (uint32_t)(value);                  \
        g_esp += 4u + 4u * (uint32_t)(nargs);       \
    } while (0)

/* Win32 BOOL values. */
#define W32_TRUE 1u
#define W32_FALSE 0u

/* ---- Export tables ----------------------------------------------------- */

typedef struct {
    const char *dll;  /* as in the import table; compared case-insensitively */
    const char *name; /* NULL for an import by ordinal */
    uint16_t ordinal; /* 0 for an import by name */
    recomp_func_t fn;
} win32_export_t;

/* One table per slice, each ending with an all-zero entry. The core slice
 * provides weak empty definitions, so a slice that is not written yet still
 * links. */
extern const win32_export_t win32_exports_core[];
extern const win32_export_t win32_exports_mem[];
extern const win32_export_t win32_exports_sync[];
extern const win32_export_t win32_exports_fs[];
extern const win32_export_t win32_exports_ui[];
extern const win32_export_t win32_exports_input[];
extern const win32_export_t win32_exports_audio[];
extern const win32_export_t win32_exports_dsound[];
extern const win32_export_t win32_exports_net[];
extern const win32_export_t win32_exports_com[];
extern const win32_export_t win32_exports_d3d[];
extern const win32_export_t win32_exports_winmm[];
extern const win32_export_t win32_exports_bink[];

/* pe_import_resolver_t for pe_load_image(): the slice entry for dll!name (or
 * dll#ordinal), registered with host_func_va(); otherwise the generated stub
 * for that import, which reports `UNSUPPORTED <dll>!<name>` once, pops the
 * import's arguments and returns 0. Never returns 0 for a listed import. */
uint32_t win32_resolve_import(const char *dll, const char *name,
                              uint16_t ordinal, int by_ordinal);

/* The D3DX SDK version the game links: NN of the first d3dx9_NN.dll an import
 * (or GetProcAddress) bound an entry from; 41 before any. LEGO Indiana Jones
 * links d3dx9_35, LEGO Harry Potter d3dx9_41. */
int win32_d3dx_sdk_version(void);

/* ---- Last error (per thread) ------------------------------------------- */

uint32_t w32_last_error(void);
void w32_set_last_error(uint32_t code);

/* ---- Handles ----------------------------------------------------------- */

/* Win32 handles have their own table, separate from the engine's handles.h:
 * one CloseHandle closes any kind, and waits apply across kinds. Values are
 * W32_HANDLE_BASE + 4 * index. That range is never mapped (a dereference
 * faults) and lies above the engine's own handle range. INVALID_HANDLE_VALUE
 * (0xFFFFFFFF) and NULL are never issued. */
#define W32_HANDLE_BASE 0xFF800000u
#define W32_HANDLE_MAX 65536u
#define W32_INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define W32_PSEUDO_CURRENT_PROCESS 0xFFFFFFFFu
#define W32_PSEUDO_CURRENT_THREAD 0xFFFFFFFEu

enum w32_kind {
    W32_KIND_ANY = 0, /* only as a w32_handle_get() filter */
    W32_KIND_FILE,
    W32_KIND_FIND,
    W32_KIND_REGKEY,
    W32_KIND_EVENT,
    W32_KIND_MUTEX,
    W32_KIND_SEMAPHORE,
    W32_KIND_THREAD,
    W32_KIND_PROCESS,
};

/* Header of every host object behind a Win32 handle. The owning slice embeds
 * it as the first member of its own struct. */
struct w32_object {
    enum w32_kind kind;
    int refs; /* handles open on this object; updated atomically by core */
    /* Called when the last handle closes. Frees the containing struct. */
    void (*destroy)(struct w32_object *self);
};

/* Issue a new handle for obj (sets obj->refs to 1). Returns 0 when full. */
uint32_t w32_handle_new(struct w32_object *obj);

/* The object behind h, or NULL if h is not live or not of that kind. Takes
 * no reference: valid until the guest closes h. */
struct w32_object *w32_handle_get(uint32_t h, enum w32_kind kind);

/* CloseHandle: drop h; destroy the object when its last handle closes.
 * Returns W32_TRUE, or W32_FALSE with ERROR_INVALID_HANDLE set. */
uint32_t w32_handle_close(uint32_t h);

/* ---- Thread information block ------------------------------------------ */

/* Give the calling thread a TIB in guest memory and point ts_fs_base at it
 * (docs/WIN32_THUNKS.md, "The thread environment block"). Owned by the sync
 * slice. Start-up calls it for the main thread, CreateThread's trampoline for
 * every new thread. stack_base/stack_limit are guest addresses. */
void w32_tib_attach(uint32_t stack_base, uint32_t stack_limit);

/* For a HOST thread that will call guest code (an audio mixer thread calling
 * the game's voice callbacks, for example): give it a guest stack
 * (guest_thread_attach) and a TIB for that stack, once. Lifted MSVC code reads
 * fs:[0] in its SEH prologs, so guest code must never run on a thread without
 * a TIB. Owned by the sync slice; safe to call more than once. */
void w32_host_thread_attach(void);

/* ---- Clock and events shared with other slices (sync slice) ------------ */

/* GetTickCount's value: milliseconds on the monotonic clock. timeGetTime
 * uses the same base. */
uint32_t w32_tick_ms(void);

/* SetEvent (pulse 0) or PulseEvent (pulse 1) on event handle h: a pulse
 * releases the threads waiting at that moment (all for a manual-reset
 * event, at most one for auto reset) and leaves the event unsignalled.
 * W32_TRUE, or W32_FALSE with ERROR_INVALID_HANDLE. */
uint32_t w32_event_set(uint32_t h, int pulse);

/* ---- Windows error codes used across slices ---------------------------- */

#define W32_ERROR_SUCCESS 0u
#define W32_ERROR_FILE_NOT_FOUND 2u
#define W32_ERROR_PATH_NOT_FOUND 3u
#define W32_ERROR_ACCESS_DENIED 5u
#define W32_ERROR_INVALID_HANDLE 6u
#define W32_ERROR_NOT_ENOUGH_MEMORY 8u
#define W32_ERROR_NO_MORE_FILES 18u
#define W32_ERROR_INVALID_PARAMETER 87u
#define W32_ERROR_INSUFFICIENT_BUFFER 122u
#define W32_ERROR_ALREADY_EXISTS 183u

#endif
