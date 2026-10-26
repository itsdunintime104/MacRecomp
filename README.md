# MacRecomp

MacRecomp rebuilds 32-bit Intel Mac games as native arm64 programs for Apple Silicon. macOS dropped 32-bit apps in Catalina, and Rosetta 2 does not run them, so these games no longer start on a current Mac. The translator also takes a 32-bit Windows (PE) binary, so a Windows-only game can be rebuilt the same way.

It works by static recompilation:

1. The translator turns every function in the game binary (i386 Mach-O, or PE32 for a Windows game) into C.
2. The runtime replaces the parts of macOS, OpenGL, Core Audio and Steam that the game calls. A Windows game uses the Win32 and Direct3D 9 layer in `windows/` instead.
3. Clang compiles both into one arm64 program, which loads the game's data from your Steam or GOG copy.

One part of the Mac games still needs Rosetta 2. They compile their shaders with NVIDIA's Cg library, which has no arm64 version, so the runtime sends each Cg call to `cghelper`, a small x86_64 program that runs under Rosetta.

This repo holds no game code or data. Each game has its own repo, which pins this one as its `engine/` submodule.

## Games

| Game | App | Repo | Build | recomp-check | boot-check (60 s) | Tested on an M1 Max |
|---|---|---|---|---|---|---|
| LEGO The Hobbit | Hobbit Recompiled | [HobbitRecompiled](https://github.com/itsdunintime104/HobbitRecompiled) | Steam 84019 | PASS | PASS | Levels 1–3 played, level 1 finished. 88–129 FPS at 2592 × 1674 in levels 2–3 |
| LEGO Marvel Super Heroes | Superheros Bricks Recompiled | [SuperheroBrickRecomp](https://github.com/itsdunintime104/SuperheroBrickRecomp) | Steam 329428 | PASS | PASS | Intro, menus and level 1 played. 72.5 FPS (1% low 39.7) at 2592 × 1674 |
| LEGO Harry Potter: Years 1-4 (Windows) | Wizard Bricks Recompiled | [WizardBrick-Recomp](https://github.com/itsdunintime104/WizardBrick-Recomp) | Steam 660 | n/a | n/a | Plays the levels tested so far (Leaky Cauldron, Diagon Alley, Gringotts), with sound. 120 FPS at 2056 × 1290 |
| LEGO Indiana Jones: The Original Adventures (Windows) | LEGO Indiana Jones - The Original Adventures | IndyRecomp (private for now) | GOG | n/a | n/a | About an hour played, with sound. 120 FPS (1% low 101) in play and about 100 FPS in cutscenes at 1728 × 1117 |

The Hobbit and Marvel are Feral Interactive ports (Mach-O). Harry Potter and Indiana Jones are Windows releases (PE). They do not use recomp-check or boot-check; they use the Windows runtime tests (`windows/tests/run_all.sh`) and their own, through `make test` in the game repo.

## Which games can work

| Layer | Depends on | Other games |
|---|---|---|
| Translator | The i386 instruction set | Any 32-bit Intel Mac program. A 32-bit Windows program uses the PE tools and boot driver in `windows/` (see [A Windows game](#a-windows-game)) |
| Front end (window, display, input) | Feral's front-end code, found by signature | Other Feral ports. Other publishers need their own replacements |
| Runtime | The macOS functions the two Mac games call, and the Win32, Direct3D 9, DirectSound and XAudio2 functions the two Windows games call | Each new import needs a replacement |

64-bit Intel Mac games run under Rosetta 2 and do not need this.

## Building a game

A game repo has `game.toml` (copy one from an existing game repo to start a new game), `recon/` (the analysis of its binary) and this repo as `engine/`. Its `setup.sh` runs these steps:

1. **Derive.** `make -C engine/build GAME=$PWD/game.toml game` reads the per-game values from the binary into `build.nosync/game/`: entry point, segments, static area, and front-end function and data addresses. It stops at the first value it cannot find.
2. **Translate.** From `engine/`: `../.venv/bin/python -m tools.xrecomp.run --analysis ... --switch ... --bin ... --out ../generated.nosync --prefix <id> --overrides ../build.nosync/game/native_overrides.txt`.
3. **Check.** `make -C engine/build GAME=$PWD/game.toml recomp-check` lists every instruction, jump or flag read the translator does not handle. Any entry stops the build.
4. **Build.** `make -C engine/build GAME=$PWD/game.toml`, then `make -C engine/build GAME=$PWD/game.toml app` for the app bundle.

When `derive` cannot find a value (a signature matches zero times, or more than once), fix it in `game.toml`:

- `[pins]` gives the address directly.
- `absent = [...]` lists front-end entries the game does not have, such as Retina support in an older port. `derive` skips them and writes `FE_HAS_<name>` 0, and the runtime leaves out the code that uses them.
- An `after` or `next` rule in `feral_frontend.toml` finds a function by its position relative to a function that was found. `next` takes the function directly after it.

An import with no replacement links as a placeholder. When the game calls it, the program prints `missing import: <name>` and the caller, then exits with code 3. Run boot-check to find these.

### A Windows game

A Windows game repo ([WizardBrick-Recomp](https://github.com/itsdunintime104/WizardBrick-Recomp) for LEGO Harry Potter, IndyRecomp for LEGO Indiana Jones) also has this repo as `engine/`, but it has no `game.toml` and does not use `engine/build/Makefile`. It keeps only its own files (`recon/`, notes, game patches, its tests, `game.conf`, `build/Makefile`, a `setup.sh` wrapper) and uses `windows/`:

- `windows/tools/pe/analyze_pe.py` writes `recon/` from the PE32 binary.
- `windows/tools/pe/run_lift_pe.py` supplies the entry point from `recon/analysis.json` (`tools.xrecomp.run` reads it from a Mach-O header) and runs `tools.xrecomp.run`. It adds `--seh-prolog` and `--seh-epilog`, the addresses of the MSVC frame helpers (`__SEH_prolog4`, `__EH_prolog3*`), and `--entry-zf`, a JSON file of functions whose entry block reads ZF as every caller leaves it.
- The game repo's `build/Makefile` sets a few values and includes `windows/build/game.mk`, which compiles the lifted C, the generic part of this runtime (`recomp_glue`, `guest_mem`, `guest_call`, `host_funcs`, `guest_heap`, `dlmalloc`, `diag`, `hud_core`) and the Win32 layer in `windows/runtime/`. Direct3D 9 goes to DXVK, which draws with Vulkan on KosmicKrisp; vkd3d-shader compiles the shaders.
- `windows/setup.sh`, driven by the game repo's `game.conf`, runs the whole setup: it finds the game folder, checks the binary's hash, builds the graphics libraries, translates the code, builds the game and the app, and installs the app.

See `windows/README.md` for the game repo interface, and `docs/BUILDING.md` in [WizardBrick-Recomp](https://github.com/itsdunintime104/WizardBrick-Recomp/blob/main/docs/BUILDING.md) for the steps.

## Layout

| Path | Contents |
|---|---|
| `tools/xrecomp/` | i386 to C translator (ported from xboxrecomp, MIT; see `tools/xrecomp/UPSTREAM.md`), for Mach-O and PE32 code |
| `tools/hobbit/` | Mach-O analysis, disassembly sweep, import signatures, code generators. The name is historical: Marvel's build uses these tools too |
| `tools/game/` | game profile (`game.toml`) and `derive` |
| `tools/game/feral_frontend.toml` | signatures of the Feral front-end functions the runtime replaces |
| `tools/cghelper/` | the x86_64 Cg helper |
| `tools/checks/` | `recomp-check` and `boot-check` |
| `tools/package/` | app bundle and launcher |
| `tools/perf/` | `plot_frames`, which turns a frame-time CSV into a chart |
| `runtime/` | startup, guest memory, import replacements, SDL2 front end, Steam stand-in. `hud_core.c` holds the frame-time overlay logic; `frontend_hud.c` draws it with OpenGL, and a game repo can add another draw backend |
| `build/Makefile` | compiles and links the generated C and the runtime |
| `windows/` | the shared Windows (PE32) runtime, tools, tests, build rules and setup script for Windows game repos (`windows/README.md`) |
| `tests/` | translator and runtime tests, including lifter tests for code forms the Windows game uses (`test_seh_helpers.py`, `test_lift_import_tail.py`, `test_lift_entry_zf.py`, `test_lift_switch_byte_map.py`, `test_lift_fuzz_forms.py`) |

## Checks

**recomp-check** reads the translation. It fails on:

- an unhandled instruction, except switch-table data and `ud2`/`hlt` traps;
- a read of a flag that no instruction set, if the read is reachable and changes control flow;
- a function that failed to translate;
- an indirect jump with no known target;
- a decode failure outside a switch table.

`FUZZ=1` also runs the leaf-function fuzzer, which compares translated functions with Unicorn (about a minute).

**boot-check** runs the built game for `SECONDS` (default 60, minimum 30) on a temporary copy of its saves. It passes only if:

- the game is still running at the end, and its last stats line is recent;
- that line shows more than 50 draws per frame, at least one texture upload and at least one audio render;
- the log has no GL error, `RECOMP:` line, `UNSUPPORTED <func>:` line from the runtime, guest-heap line or assertion.

Notices from Apple's GL driver (`UNSUPPORTED (log once) ...`) are reported but do not fail the check. `GUARD=1` adds the guest heap guard. The check refuses to start while the game is running. Logs go to `build.nosync/checks/` in the game repo.

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

`RECOMP_GAME_ROOT` sets the game folder and `HOBBIT_BIN` the binary.

The lifter tests for the Windows code forms (listed under [Layout](#layout)) build their own input and need no game files.

Some tests compare with Apple's `otool`. If an Intel-only `otool` (from MacPorts, for example) is first in `PATH`, they fail with an `xcrun` error. Put `/usr/bin` first in `PATH`.

## License

GPL-3.0 (`LICENSE`). `tools/xrecomp/` keeps its upstream MIT notice.
