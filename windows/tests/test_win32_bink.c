/* binkw32 slice test: every Bink import resolves by its decorated name to
 * the slice entry, pops the bytes its decoration gives, returns 0 and logs
 * once. Also the DSOUND ordinal 11 fallback with no DirectSound slice
 * linked: the inventory names it DirectSoundCreate8 and its stub pops 12.
 * Links with win32_bink.c, win32_core.c, win32_unsupported.c and the engine
 * runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "win32_thunk.h"

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define STACK_TOP 0x00FF0000u

/* Push n zero arguments and a return address, call fn; returns g_eax and
 * the bytes popped beyond the return address. */
static uint32_t call(recomp_func_t fn, unsigned n, uint32_t *popped)
{
    g_esp = STACK_TOP;
    for (unsigned i = 0; i < n; i++)
        PUSH32(g_esp, 0);
    PUSH32(g_esp, 0xDEAD0001u);
    uint32_t start = g_esp;
    g_ebx = 0xB0B0B0B0u, g_esi = 0x51515151u, g_edi = 0xD1D1D1D1u, g_ebp = 0xBEBEBEBEu;
    g_eax = 0xCCCCCCCCu;
    fn();
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBEBEBEBEu);
    *popped = g_esp - start - 4u;
    return g_eax;
}

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
    static char buf[16384];
    fflush(stderr);
    dup2(g_saved_fd, 2);
    close(g_saved_fd);
    rewind(g_cap);
    size_t n = fread(buf, 1, sizeof buf - 1, g_cap);
    buf[n] = '\0';
    fclose(g_cap);
    return buf;
}

static int count(const char *text, const char *s)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    return n;
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00100000u, PROT_READ | PROT_WRITE) == 0);

    /* The 17 names LEGOIndy.exe imports, with the decoration's byte count. */
    static const char *const names[] = {
        "_BinkGoto@12", "_BinkOpenDirectSound@4", "_BinkSetSoundSystem@8", "_BinkSetSoundTrack@8",
        "_BinkOpen@8", "_BinkGetFrameBuffersInfo@8", "_BinkSetVolume@12", "_BinkCopyToBuffer@28",
        "_BinkCopyToBufferRect@44", "_BinkGetRects@8", "_BinkClose@4", "_BinkWait@4",
        "_BinkNextFrame@4", "_BinkShouldSkip@4", "_BinkDoFrame@4", "_BinkRegisterFrameBuffers@8",
        "_BinkPause@8",
    };
    int n = 0;
    for (const win32_export_t *e = win32_exports_bink; e->dll != NULL; e++)
        n++;
    CHECK(n == 17);
    capture_begin();
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        unsigned bytes = (unsigned)atoi(strchr(names[i], '@') + 1);
        recomp_func_t fn = guest_resolve(win32_resolve_import("binkw32.dll", names[i], 0, 0));
        const win32_export_t *e = win32_exports_bink;
        while (e->dll != NULL && strcmp(e->name, names[i]) != 0)
            e++;
        CHECK(e->dll != NULL && fn == e->fn); /* the slice entry, not the stub */
        for (int k = 0; k < 2 && fn != NULL; k++) {
            uint32_t popped;
            CHECK(call(fn, bytes / 4u, &popped) == 0);
            if (popped != bytes) {
                fprintf(stderr, "FAIL %s popped %u, want %u\n", names[i], popped, bytes);
                g_fail = 1;
            }
        }
    }
    char *out = capture_end();
    CHECK(count(out, "UNSUPPORTED binkw32.dll!_BinkOpen@8: no Bink playback, returns 0") == 1);
    CHECK(count(out, "UNSUPPORTED binkw32.dll!") == 17); /* once each */

    /* DSOUND #11 with no DirectSound slice: the inventory stub for
     * DirectSoundCreate8, 3 arguments. */
    uint32_t va = win32_resolve_import("dsound.dll", "", 11, 1);
    CHECK(va != 0);
    capture_begin();
    uint32_t popped = 0;
    CHECK(va != 0 && call(guest_resolve(va), 3, &popped) == 0);
    out = capture_end();
    CHECK(popped == 12);
    CHECK(count(out, "UNSUPPORTED dsound.dll!DirectSoundCreate8: not implemented") == 1);

    if (g_fail)
        return 1;
    printf("ok: 17 Bink stubs (BinkOpen returns 0), DSOUND #11 stub\n");
    return 0;
}
