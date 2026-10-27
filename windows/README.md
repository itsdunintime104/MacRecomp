# Windows (PE32) runtime

This folder holds the parts that every Windows game repo shares: the Win32 and Direct3D 9 runtime, the PE tools, the runtime tests, the build rules and the setup script. A game repo has this engine as `engine/` and keeps only its own files: `recon/`, notes, game patches and hooks, its tests, `build/Makefile`, `game.conf`, a `setup.sh` wrapper, README and LICENSE.

The Mac (Mach-O) games do not use this folder. The translator (`tools/xrecomp/`) and the generic runtime (`runtime/src`: guest memory, guest calls, host functions, heap, diagnostics, overlay text) are shared with them.

## Contents

| Path | Contents |
|---|---|
| `runtime/src`, `runtime/include` | Win32 slices (`win32_*`; `win32_wmi.c` answers the WMI query games use to find XInput pads), COM objects, the D3D9 and D3DX layer (`d3d9_*`, `d3dx_*`, generated `gen_d3d9_guest.c`), the D3D9 overlay backend (`d3d9_hud.c`), frame trace, input scripts, the PE loader and the boot driver (`pe_boot.c`, `pe_boot_main.c`) |
| `runtime/include/game_hooks.h` | the C hooks a game can define (see [Game hooks](#game-hooks)) |
| `tools/` | generic tools (see [Tools](#tools)) |
| `tests/` | the runtime tests and `run_all.sh` |
| `build/game.mk` | the build rules a game's `build/Makefile` includes |
| `setup.sh` | the one-command setup a game's `setup.sh` runs |

## Game repo interface

### build/Makefile

```make
ENGINE ?= $(abspath ../engine)
GAME_SRCS := $(wildcard $(abspath ../runtime/src)/*.c)
GAME_CFLAGS := -I $(abspath ../runtime/include) -DPE_ENTRY_RVA=0x<rva>u -DPE_SECTION_COUNT=<n>u
GAME_TESTS := $(abspath ../tests/game_tests.sh)
include $(ENGINE)/windows/build/game.mk
```

| Variable | Required | Meaning |
|---|---|---|
| `GAME_SRCS` | no | the game's C files (absolute paths): patches, hooks, game-only input |
| `GAME_CFLAGS` | yes | `-DPE_ENTRY_RVA` and `-DPE_SECTION_COUNT`, which the PE loader checks; optional `-DGAME_SCREEN_WIDTH_VA`, `-DGAME_SCREEN_HEIGHT_VA` (the guest words that hold the configured screen size; without them the D3D9 layer leaves that size alone); any `-I` for the game's headers |
| `GAME_TESTS` | no | the game's test list for `run_all.sh --tests` |
| `GEN`, `DEPS`, `OPT`, `TSO`, `GAME_DIR`, `GAME_IMAGE` | no | lift output (default `generated.nosync`), graphics libraries (default `build.nosync/deps`), optimisation, x86 ordering, and the game folder and image the app launcher uses by default |

`game.mk` reads `GAME_ID`, the `APP_*` names and `GAME_REG_KEY` from `game.conf`, unless the Makefile or the command line sets them. It sets the C macros `GAME_SAVE_NAME` from `APP_SAVE_NAME`, `GAME_EXE_NAME` from `APP_GAME_FILE` and `GAME_REG_KEY` from `GAME_REG_KEY`. Targets: `all` (builds `build.nosync/$(GAME_ID)`), `app` (`build.nosync/$(APP_NAME).app`) and `test`.

### game.conf

Shell variables, read by `setup.sh` (sourced) and by `game.mk`. Values must not contain `$` or `#`. Sourcing it must only set variables and define functions.

| Key | Example (HP) | Used by |
|---|---|---|
| `GAME_ID` | `lhp` | binary name |
| `APP_NAME` | `LEGO Harry Potter Years 1-4` | app bundle name |
| `APP_EXE` | `HarryPotterRecompiled` | launcher name inside the app |
| `APP_ID` | `local.harrypotterrecomp` | bundle id |
| `APP_SAVE_NAME` | `HarryPotterRecomp` | save and log folder name |
| `APP_ENV_PREFIX` | `HPRECOMP` | prefix of the user settings: `<prefix>_GAME_DIR`, `<prefix>_EXE` (launcher), `<prefix>_DEPS_DIR`, `<prefix>_BUILD_LOCK` (setup) |
| `APP_GAME_FILE` | `LEGOHarryPotter.exe` | exe file name in the game folder; also the guest exe path `C:\Game\<file>` and the command line |
| `GAME_REG_KEY` | `SOFTWARE\WB Games\LEGOHarryPotter` | the one HKLM registry key the guest can open |
| `APP_GAME_TITLE` | `LEGO Harry Potter: Years 1-4` | title in messages |
| `APP_IMAGE` | `lhp_win32.exe` | file name of the image copy in `orig.nosync/` |
| `BINARY_SHA256` | | sha256 of the supported exe |
| `LIFT_PREFIX` | `lhp` | file prefix of the lifted C |
| `RECON_ANALYSIS`, `RECON_SWITCH` | `recon/analysis.json`, `recon/switch_tables.json` | lift inputs; `names.json` and `entry_zf.json` are read from the folder of `RECON_ANALYSIS`; `analyze_pe.py --entry-zf-out` writes `entry_zf.json` |
| `GAME_FOLDER` | `LEGO Harry Potter` | folder name under `steamapps/common`, `~/Games` and drives |
| `GOG_GLOB` | `LEGO*Harry*Potter*` | GOG folder pattern |
| `DATA_GLOB` | `GAME*.DAT` | data files that must sit next to the exe |
| `STORE_HINT`, `VERSION_HINT` | | text for "not found" and "wrong build" messages |
| `POST_INSTALL_OPTION`, `POST_INSTALL_HELP` | `--no-warm` | optional: the option that skips `post_install`, and its help line |

A game repo's `setup.sh`:

```sh
#!/bin/sh
exec "$(dirname "$0")/engine/windows/setup.sh" "$@"
```

### Game hooks

C hooks (`runtime/include/game_hooks.h`). Each has a weak default next to its caller; a game file that defines the function replaces it.

| Hook | Default | Called |
|---|---|---|
| `uint32_t game_patch_sleep_ms(uint32_t guest_ret, uint32_t ms)` | returns `ms` | by `Sleep`, with the guest return address |
| `int game_boot_hook(const char *save_dir, pe_import_resolver_t *resolve)` | returns 0 | once by `pe_boot_run`, after the save folder exists and before the image loads; it may replace the import resolver (HP's shader warm-up wraps `PeekMessageA` this way) |

Shell hook: `game.conf` may define `post_install`, run as the last setup step after the install. It can use `step`, `run`, `ROOT`, `ENGINE`, `VENV`, `GAME_DIR`, `APP`, `STEPS` and the `game.conf` keys.

### Game tests

`tests/run_all.sh` builds and runs the runtime tests. `--tests FILE` sources a game's bash file after the engine's test definitions. The file may define `def_<name>` functions (C tests set `FLAGS`, `LIBS` and `ENVS`; Python tests set `PYTEST`) and add the names to `C_TESTS`, `GPU_TESTS` or `PY_TESTS`. It can use `COMMON`, `SDL_I`, `SDL_L`, `FAUDIO_I`, `FAUDIO_L`, `VKD3D_I`, `VKD3D_L`, `DXVK_I`, `DXVK_L`, `GPU_ENV`, `R` (this runtime's sources), `S` (the generic runtime's sources) and `TESTS_DIR` (the folder of the file). The graphics libraries come from `RECOMP_DEPS_DIR` (`HPRECOMP_DEPS_DIR` is accepted too). Python tests run with `--python`, by default the `.venv` of the game repo.

## Tools

| Folder | Why it is generic |
|---|---|
| `pe/` | PE analysis, lift driver, names, symbols, shader extraction, coverage, `recon_check.py` (reports recon misses); each takes the binary and the recon files as inputs |
| `win32/` | import inventory, slice assignment and argument-count checks; they read the game's inventory files |
| `d3d9/` | generator of the D3D9 guest layer and the D3DX export list |
| `deps/` | builds DXVK, vkd3d-shader and KosmicKrisp |
| `dxvk_test/` | DXVK smoke test; its patches are the ones `deps/build_dxvk.sh` applies |
| `vkd3d/` | the vkd3d-shader patch |
| `ghidra/` | Ghidra export scripts for any PE |
| `package/` | app bundle, launcher, icon and self-containment check |
| `perf/` | report of a frame trace (`frame_trace.c` output) |

The HP repo keeps the HP-only tools: `boot/` (HP input scripts and run helpers), `check/` (HP reliability verdict), `shaders/` (HP shader-builder research and scene list), `trace/` (Wine golden-trace tools for the HP exe) and `d3d9_caps/` (a probe of HP's device set-up).

## Known limits

The developer settings keep the names from the first game the runtime ran (LEGO Harry Potter): `HPRECOMP_*` and `HP_TRACE_*`. The user settings of the launcher and of `setup.sh` use `APP_ENV_PREFIX`.
