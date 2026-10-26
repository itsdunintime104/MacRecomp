/* binkw32 slice: the 17 Bink imports of LEGO Indiana Jones, as stubs. There
 * is no Bink decoder yet, so BinkOpen returns 0 and the game skips the
 * movie: its movie-open method stores the 0 and returns false. The
 * other 16 should then never be reached; each logs once and
 * returns 0. The import names are the decorated stdcall names, and the
 * decoration gives the bytes popped. */
#include "recomp_types.h"
#include "diag.h"
#include "win32_thunk.h"

#define BINK_STUB(name, bytes)                                                                 \
    static void imp_##name(void)                                                               \
    {                                                                                          \
        diag_unsupported("binkw32.dll!_" #name "@" #bytes, MEM32(g_esp),                       \
                         "no Bink playback, returns 0");                                       \
        W32_RETURN((bytes) / 4, 0);                                                            \
    }
#define BINK_ENTRY(name, bytes) {"binkw32.dll", "_" #name "@" #bytes, 0, imp_##name},

#define BINK_IMPORTS(X)                                                                        \
    X(BinkClose, 4)                                                                            \
    X(BinkCopyToBuffer, 28)                                                                    \
    X(BinkCopyToBufferRect, 44)                                                                \
    X(BinkDoFrame, 4)                                                                          \
    X(BinkGetFrameBuffersInfo, 8)                                                              \
    X(BinkGetRects, 8)                                                                         \
    X(BinkGoto, 12)                                                                            \
    X(BinkNextFrame, 4)                                                                        \
    X(BinkOpen, 8)                                                                             \
    X(BinkOpenDirectSound, 4)                                                                  \
    X(BinkPause, 8)                                                                            \
    X(BinkRegisterFrameBuffers, 8)                                                             \
    X(BinkSetSoundSystem, 8)                                                                   \
    X(BinkSetSoundTrack, 8)                                                                    \
    X(BinkSetVolume, 12)                                                                       \
    X(BinkShouldSkip, 4)                                                                       \
    X(BinkWait, 4)

BINK_IMPORTS(BINK_STUB)

const win32_export_t win32_exports_bink[] = {
    BINK_IMPORTS(BINK_ENTRY)
    {0},
};
