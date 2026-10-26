# MacRecomp

MacRecomp rebuilds old 32-bit Intel games as native arm64 programs for Apple Silicon Macs.

- **32-bit Mac games.** macOS dropped 32-bit apps in Catalina, and Rosetta 2 does not run them, so these games no longer start on a current Mac.
- **32-bit Windows games.** The same translator takes a Windows (PE32) binary, so a Windows-only game can run as a Mac app.

This repo is the shared engine. It holds no game code or data. Each game has its own repo, which pins this one as its `engine/` submodule.

## Play a game

1. Own the game. Install the build listed under [Games](#games).
2. Clone the game's repo with its submodules, and run `./setup.sh` in it.
3. Open the app that setup builds.

Setup finds the game folder, checks the binary's SHA-256, translates the game, and builds the app. For a Windows game it searches:

- Steam under Wine, CrossOver and Whisky, and Mac Steam;
- other Steam libraries;
- GOG installs;
- `~/Games` and external drives.

A game accepts only the build in the table, because the translation is made from that exact binary. Another store's copy works only if its binary is the same file.

## Games

| Game | Repo | Supported build | Checks that pass | Tested on an M1 Max |
|---|---|---|---|---|
| LEGO The Hobbit (Mac) | [HobbitRecompiled](https://github.com/itsdunintime104/HobbitRecompiled) | Steam, build 84019 | recomp-check, boot-check (60 s) | Levels 1–3 played, level 1 finished. 88–129 FPS at 2592 × 1674 in levels 2–3 |
| LEGO Marvel Super Heroes (Mac) | [SuperheroBrickRecomp](https://github.com/itsdunintime104/SuperheroBrickRecomp) | Steam, build 329428 | recomp-check, boot-check (60 s) | Intro, menus and level 1 played. 72.5 FPS (1% low 39.7) at 2592 × 1674 |
| LEGO Harry Potter: Years 1-4 (Windows) | [WizardBrick-Recomp](https://github.com/itsdunintime104/WizardBrick-Recomp) | Steam, build 660 | `make test` | Leaky Cauldron, Diagon Alley and Gringotts played, with sound. 120 FPS at 2056 × 1290 |
| LEGO Indiana Jones: The Original Adventures (Windows) | [IndyRecomp](https://github.com/itsdunintime104/IndyRecomp) | GOG | `make test` | About an hour played, with sound. 120 FPS (1% low 101) in play, about 100 FPS in cutscenes, at 1728 × 1117 |

The Hobbit and Marvel are Feral Interactive ports. They pass recomp-check and boot-check (see [Checks](#checks)). The two Windows games do not use these checks. They pass `make test` in the game repo, which runs `windows/tests/run_all.sh` and the game's own tests.

## How it works

1. The translator turns every function in the game binary (i386 Mach-O, or PE32 for a Windows game) into C.
2. The runtime replaces the parts of macOS, OpenGL, Core Audio and Steam that a Mac game calls. A Windows game uses the Win32 and Direct3D 9 layer in `windows/` instead.
3. Clang compiles both into one arm64 program. The program loads the game's data from your copy of the game.

One part of the Mac games still needs Rosetta 2. They compile their shaders with NVIDIA's Cg library, which has no arm64 version. The runtime sends each Cg call to `cghelper`, a small x86_64 program that runs under Rosetta.

## Which games can work

| Layer | Depends on | Other games |
|---|---|---|
| Translator | The i386 instruction set | Any 32-bit Intel Mac program. A 32-bit Windows program uses the PE tools in `windows/` |
| Front end (window, display, input) | Feral's front-end code, found by signature | Other Feral ports. Other publishers need their own replacements |
| Runtime | The macOS functions the two Mac games call, and the Win32, Direct3D 9, DirectSound and XAudio2 functions the two Windows games call | Each new import needs a replacement |

64-bit Intel Mac games run under Rosetta 2 and do not need this.

## Building a game

### A Mac game

A Mac game repo has `game.toml`, `recon/` (the analysis of its binary) and this repo as `engine/`. To start a new game, copy `game.toml` from an existing game repo. The repo's `setup.sh` runs these steps:

1. **Derive.** `make -C engine/build GAME=$PWD/game.toml game` reads the per-game values from the binary into `build.nosync/game/`: entry point, segments, static area, and front-end function and data addresses. It stops at the first value it cannot find.
2. **Translate.** From `engine/`: `../.venv/bin/python -m tools.xrecomp.run --analysis ... --switch ... --bin ... --out ../generated.nosync --prefix <id> --overrides ../build.nosync/game/native_overrides.txt`.
3. **Check.** `make -C engine/build GAME=$PWD/game.toml recomp-check`. Any entry stops the build.
4. **Build.** `make -C engine/build GAME=$PWD/game.toml`, then `make -C engine/build GAME=$PWD/game.toml app` for the app bundle.

When `derive` cannot find a value (a signature matches zero times, or more than once), fix it in `game.toml`:

- `[pins]` gives the address directly.
- `absent = [...]` lists front-end entries the game does not have, such as Retina support in an older port. `derive` skips them and writes `FE_HAS_<name>` 0, and the runtime leaves out the code that uses them.
- An `after` or `next` rule in `tools/game/feral_frontend.toml` finds a function by its position relative to a function that was found. `next` takes the function directly after it.

An import with no replacement links as a placeholder. When the game calls it, the program prints `missing import: <name>` and the caller, then exits with code 3. Run boot-check to find these.

### A Windows game

A Windows game repo also has this repo as `engine/`. It has no `game.toml`. Its `game.conf` holds the game's names and binary hash, and its `build/Makefile` includes `windows/build/game.mk`. The repo keeps only its own files: `recon/`, notes, game patches, its tests and a `setup.sh` wrapper.

- `windows/tools/pe/analyze_pe.py` writes the function list in `recon/` from the PE32 binary and its Ghidra exports.
- `windows/tools/pe/run_lift_pe.py` runs the translator on the PE32 binary.
- `windows/build/game.mk` compiles the translated C with the Win32 layer in `windows/runtime/`. Direct3D 9 goes to DXVK, which draws with Vulkan on KosmicKrisp. vkd3d-shader compiles the shaders.
- `windows/setup.sh` runs every step, from finding the game folder to installing the app.

`windows/README.md` describes the files a game repo must have. [WizardBrick-Recomp's docs/BUILDING.md](https://github.com/itsdunintime104/WizardBrick-Recomp/blob/main/docs/BUILDING.md) lists the build steps.

## Checks

These checks apply to the Mac games.

**recomp-check** reads the translation. It fails on:

- an unhandled instruction, except switch-table data and `ud2`/`hlt` traps;
- a reachable read of a flag that no instruction set, when the read changes control flow;
- a function that failed to translate;
- an indirect jump with no known target;
- a decode failure outside a switch table.

`FUZZ=1` also runs the leaf-function fuzzer. It compares translated functions with Unicorn and takes about a minute.

**boot-check** runs the built game for `SECONDS` (default 60, minimum 30) on a temporary copy of its saves. It passes only if:

- the game is still running at the end, and its last stats line is recent;
- that line shows more than 50 draws per frame, at least one texture upload and at least one audio render;
- the log has no GL error, `RECOMP:` line, `UNSUPPORTED <func>:` line from the runtime, guest-heap line or assertion.

Notices from Apple's GL driver (`UNSUPPORTED (log once) ...`) are reported but do not fail the check. `GUARD=1` adds the guest heap guard. The check does not start while the game is running. Logs go to `build.nosync/checks/` in the game repo.

```
make -C engine/build GAME=$PWD/game.toml boot-check
```

`make -C engine/build GAME=$PWD/game.toml glerr` builds `glerr.dylib`, which names the GL call that raised a GL error.

## Runtime switches

Environment variables the game program reads:

| Variable | Effect |
|---|---|
| `RECOMP_RENDER_SIZE=WxH` | render at this size and scale to the window |
| `RECOMP_STATS=N` | print draws, texture uploads and audio renders every N seconds |
| `RECOMP_GL_CHECK=1` | call `glGetError` once per frame, before the swap |
| `RECOMP_FRAME_DUMP=N`, `RECOMP_FRAME_DIR=dir` | save frames N, 2N, 4N … (8 at most) as PPM files |
| `RECOMP_HANGWATCH=S` | every S seconds, print each guest thread's stack pointer and the latest indirect calls; mark `HANG?` when a stack pointer has not changed for 3 intervals |
| `RECOMP_HEAP_GUARD=1` or `2` | guest heap guard: 1 aborts at the first report, 2 reports and continues |
| `RECOMP_TRACE_OPENS=1` | log file opens |
| `RECOMP_TRACE_AUDIO=1` | log audio graph lifetimes, callbacks, failed calls and each callback's first render |
| `RECOMP_STARTUP_VERBOSE=1` | log import binding and static initialisers at startup |

For the rest, search `runtime/src` for `getenv("RECOMP_`.

## Tests

The tests use The Hobbit (Steam build 84019) as the reference game. Run them from `engine/` inside a game repo:

```
../.venv/bin/python -m pytest tests
```

`RECOMP_GAME_ROOT` sets the game folder and `HOBBIT_BIN` the binary. The lifter tests for Windows code forms (`test_lift_import_tail.py`, `test_lift_entry_zf.py`, `test_lift_switch_byte_map.py`, `test_lift_fuzz_forms.py`, `test_seh_helpers.py`) build their own input and need no game files.

Some tests compare with Apple's `otool`. If an Intel-only `otool` (from MacPorts, for example) is first in `PATH`, they fail with an `xcrun` error. Put `/usr/bin` first in `PATH`.

## Layout

| Path | Contents |
|---|---|
| `tools/xrecomp/` | i386 to C translator for Mach-O and PE32 code (ported from xboxrecomp, MIT; see `tools/xrecomp/UPSTREAM.md`) |
| `tools/hobbit/` | Mach-O analysis, disassembly sweep, import signatures, code generators. The Marvel build uses them too |
| `tools/game/` | game profile (`game.toml`), `derive`, and `feral_frontend.toml`, the signatures of the Feral front-end functions the runtime replaces |
| `tools/cghelper/` | the x86_64 Cg helper |
| `tools/checks/` | `recomp-check` and `boot-check` |
| `tools/package/` | app bundle and launcher |
| `tools/perf/` | `plot_frames`, which turns a frame-time CSV into a chart |
| `runtime/` | startup, guest memory, import replacements, SDL2 front end, Steam stand-in, frame-time overlay |
| `build/Makefile` | compiles and links the translated C and the runtime |
| `windows/` | the shared Windows runtime, PE tools, tests, build rules and setup script (`windows/README.md`) |
| `tests/` | translator and runtime tests |

## License

GPL-3.0 (`LICENSE`). `tools/xrecomp/` keeps its upstream MIT notice.
