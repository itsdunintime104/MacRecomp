/* Hooks a game repo can define in its own sources (windows/README.md, "Game
 * hooks"). Each has a weak default next to its caller, so a game defines
 * only the hooks it needs; a strong definition in a game file replaces the
 * default at link time. */
#ifndef GAME_HOOKS_H
#define GAME_HOOKS_H

#include <stdint.h>

#include "pe_load.h"

/* Game patches keyed by the guest call site (the return address a guest call
 * pushes): the milliseconds a Sleep(ms) called from guest_ret sleeps, ms or
 * less for a patched call site. Default (win32_sync.c): ms. */
uint32_t game_patch_sleep_ms(uint32_t guest_ret, uint32_t ms);

/* Called once by pe_boot_run after the save folder save_dir exists and
 * before the image is loaded. It may replace *resolve, the import resolver
 * the loader binds every import slot with (for example to wrap one import),
 * and set up game-only run modes. Returns 0, or non-zero to stop the boot
 * with status 1. Default (pe_boot.c): does nothing and returns 0. */
int game_boot_hook(const char *save_dir, pe_import_resolver_t *resolve);

#endif
