# MacRecomp

MacRecomp rebuilds old 32-bit Intel games as Mac apps for Apple Silicon. It turns each game's code into C, then compiles that C for the Mac's own processor. No emulator runs while you play.

- **32-bit Mac games.** macOS stopped running 32-bit apps in Catalina, and Rosetta 2 does not run them either, so these games no longer start on a current Mac.
- **32-bit Windows games.** The same translator reads a Windows exe, so a game that only came out for Windows can run as a Mac app.

This repo is the shared engine. It holds no game code or game files. Each game has its own repo, which includes this one as its `engine/` folder (a git submodule).

## Play a game

1. Own the game, and install the version listed under [Games](#games).
2. Clone the game's repo with its submodules, and run `./setup.sh` in it.
3. Open the app that setup builds.

Setup finds the game folder, checks that the game file is the right version, translates the game and builds the app. For a Windows game it looks in:

- Steam under Wine, CrossOver and Whisky, and Steam for Mac;
- other Steam library folders;
- GOG installs;
- `~/Games` and external drives.

Each game works with only one version of its game file, because the translation is made from that exact file. A copy from another store works only if its file is identical.

## Games

| Game | Repo | Version that works | Checks that pass | Tested on an M1 Max |
|---|---|---|---|---|
| LEGO The Hobbit (Mac) | [HobbitRecompiled](https://github.com/itsdunintime104/HobbitRecompiled) | Steam, build 84019 | recomp-check, boot-check (60 s) | Levels 1–3 played, level 1 finished. 88–129 FPS at 2592 × 1674 in levels 2–3 |
| LEGO Marvel Super Heroes (Mac) | [SuperheroBrickRecomp](https://github.com/itsdunintime104/SuperheroBrickRecomp) | Steam, build 329428 | recomp-check, boot-check (60 s) | Intro, menus and level 1 played. 72.5 FPS (slowest 1%: 39.7) at 2592 × 1674 |
| LEGO Harry Potter: Years 1-4 (Windows) | [WizardBrick-Recomp](https://github.com/itsdunintime104/WizardBrick-Recomp) | Steam, build 660 | recon_check, `make test` | Leaky Cauldron, Diagon Alley and Gringotts played, with sound. 120 FPS at 2056 × 1290 |
| LEGO Indiana Jones: The Original Adventures (Windows) | [IndyRecomp](https://github.com/itsdunintime104/IndyRecomp) | GOG | recon_check, `make test` | About an hour played, with sound. 120 FPS (slowest 1%: 101) in play, about 100 FPS in cutscenes, at 1728 × 1117 |

The Hobbit and Marvel are Mac ports made by Feral Interactive. See [Checks](#checks) for what each check proves.

## How it works

1. The translator turns every function in the game's code into C. A Mac game's code is a 32-bit Intel Mac program (Mach-O); a Windows game's code is a 32-bit Windows exe (PE).
2. The runtime stands in for the parts of the system the game calls. For a Mac game, that is macOS, OpenGL, Core Audio and Steam. For a Windows game, it is Windows and Direct3D 9, in `windows/`.
3. Clang compiles both into one program for Apple Silicon. The program loads the game's data from your copy of the game.

One part of the Mac games still needs Rosetta 2. They compile their shaders with NVIDIA's Cg library, which has no Apple Silicon version. The runtime sends each Cg call to `cghelper`, a small Intel program that runs under Rosetta.

## Which other games can work

| Part | What it depends on | What another game needs |
|---|---|---|
| Translator | The 32-bit Intel instruction set | Nothing, if it is a 32-bit Intel Mac program or Windows exe |
| Mac front end (window, display, input) | Feral's front-end code, which the build finds by its byte patterns | Another Feral port works the same way. A game from another publisher needs its own replacement |
| Runtime | The macOS functions the two Mac games call, and the Windows, Direct3D 9, DirectSound and XAudio2 functions the two Windows games call | A replacement for each system function it calls that the runtime does not have yet |

64-bit Intel Mac games run under Rosetta 2 and do not need this.

## Building a game

### A Mac game

A Mac game repo has `game.toml`, `recon/` (what is known about the game's code) and this repo as `engine/`. To start a new game, copy `game.toml` from an existing game repo. The repo's `setup.sh` runs these steps:

1. **Find the game's values.** `make -C engine/build GAME=$PWD/game.toml game` reads them from the game's code into `build.nosync/game/`: the start address, the memory layout, and the addresses of the front-end functions and data. It stops at the first value it cannot find.
2. **Translate.** From `engine/`: `../.venv/bin/python -m tools.xrecomp.run --analysis ... --switch ... --bin ... --out ../generated.nosync --prefix <id> --overrides ../build.nosync/game/native_overrides.txt`.
3. **Check.** `make -C engine/build GAME=$PWD/game.toml recomp-check`. Any finding stops the build.
4. **Build.** `make -C engine/build GAME=$PWD/game.toml`, then `make -C engine/build GAME=$PWD/game.toml app` for the app.

If step 1 cannot find a value (its byte pattern matches nowhere, or in more than one place), set it in `game.toml`:

- `[pins]` gives the address directly.
- `absent = [...]` lists front-end functions the game does not have, such as Retina support in an older port. Step 1 skips them, and the runtime leaves out the code that uses them.
- An `after` or `next` rule in `tools/game/feral_frontend.toml` finds a function by its place next to a function that was found. `next` takes the function right after it.

A system function with no replacement still links. If the game calls it, the program prints `missing import: <name>` and the caller, and exits with code 3. Run boot-check to find these.

### A Windows game

A Windows game repo also has this repo as `engine/`, but no `game.toml`. Its `game.conf` holds the game's names, the checksum of its exe and its registry key. Its `build/Makefile` includes `windows/build/game.mk`. The repo keeps only its own files: `recon/`, notes, game patches, its tests and a `setup.sh` that calls the engine's.

- `windows/tools/pe/analyze_pe.py` writes the list of the game's functions to `recon/`, from the exe and a Ghidra export.
- `windows/tools/pe/run_lift_pe.py` translates the exe.
- `windows/build/game.mk` compiles the translated C with the Windows runtime in `windows/runtime/`. Direct3D 9 goes to DXVK, which draws with Vulkan on KosmicKrisp. vkd3d-shader compiles the shaders.
- `windows/setup.sh` runs every step, from finding the game folder to installing the app.

`windows/README.md` lists the files a game repo must have. [WizardBrick-Recomp's docs/BUILDING.md](https://github.com/itsdunintime104/WizardBrick-Recomp/blob/main/docs/BUILDING.md) lists the build steps.

## Checks

**recomp-check** (Mac games) reads the translated code. It fails on:

- an instruction the translator could not handle, except data inside switch tables and deliberate crash instructions (`ud2`, `hlt`);
- code that reads a CPU flag that nothing set before it, when the value decides where the code goes next;
- a function that failed to translate;
- a jump through a pointer whose targets are not known;
- bytes that do not decode, outside a switch table.

`FUZZ=1` also runs a fuzzer. It runs small translated functions and the original code (in the Unicorn emulator) on random inputs and compares the results. It takes about a minute.

**boot-check** (Mac games) runs the built game for `SECONDS` (default 60, at least 30) on a temporary copy of its saves. It passes only if:

- the game is still running at the end, and its last stats line is recent;
- that line shows more than 50 draws per frame, at least one texture upload and at least one audio render;
- the log has no OpenGL error, no `RECOMP:` line, no `UNSUPPORTED <func>:` line from the runtime, no game-memory report and no assertion.

Notices from Apple's OpenGL driver (`UNSUPPORTED (log once) ...`) are reported but do not fail the check. `GUARD=1` adds checks on the game's memory use. The check does not start while the game is running. Logs go to `build.nosync/checks/` in the game repo.

```
make -C engine/build GAME=$PWD/game.toml boot-check
```

`make -C engine/build GAME=$PWD/game.toml glerr` builds `glerr.dylib`, which names the OpenGL call that caused an OpenGL error.

**recon_check** (Windows games) reads the exe and `recon/`. It fails when the translation would miss a function the game can call:

- data in the exe points at code that is not in the function list;
- a math function reads a CPU flag that its caller set, but `recon/entry_zf.json` does not say so;
- a function start splits a compare from the jump that reads its result.

Run it from the game repo; a clean run prints `findings 0`:

```
.venv/bin/python -I engine/windows/tools/pe/recon_check.py orig.nosync/<exe> --recon recon
```

**make test** (Windows games), in the game repo, runs the Windows runtime tests (`windows/tests/run_all.sh`) and the game's own tests.

## Runtime settings

The game program reads these environment variables:

| Variable | Effect |
|---|---|
| `RECOMP_RENDER_SIZE=WxH` | Draw at this size and scale to the window |
| `RECOMP_STATS=N` | Print draws, texture uploads and audio renders every N seconds |
| `RECOMP_GL_CHECK=1` | Check for an OpenGL error once per frame, before the frame is shown |
| `RECOMP_FRAME_DUMP=N`, `RECOMP_FRAME_DIR=dir` | Save frames N, 2N, 4N … (8 at most) as PPM images |
| `RECOMP_HANGWATCH=S` | Every S seconds, print where each game thread is; mark `HANG?` when a thread has not moved for 3 intervals |
| `RECOMP_HEAP_GUARD=1` or `2` | Check the game's memory use: 1 stops at the first problem, 2 reports and goes on |
| `RECOMP_TRACE_OPENS=1` | Log each file the game opens |
| `RECOMP_TRACE_AUDIO=1` | Log the audio set-up, callbacks, failed calls and each callback's first sound |
| `RECOMP_STARTUP_VERBOSE=1` | Log how system functions are connected, and the start-up code, when the game starts |

For the rest, search `runtime/src` for `getenv("RECOMP_`.

## Tests

The tests use The Hobbit (Steam build 84019) as the reference game. Run them from `engine/` inside a game repo:

```
../.venv/bin/python -m pytest tests
```

`RECOMP_GAME_ROOT` sets the game folder and `HOBBIT_BIN` the game's code file. The translator tests for Windows code (`test_lift_import_tail.py`, `test_lift_entry_zf.py`, `test_lift_switch_byte_map.py`, `test_lift_fuzz_forms.py`, `test_seh_helpers.py`) make their own input and need no game files.

Some tests compare with Apple's `otool`. If an Intel-only `otool` (from MacPorts, for example) comes first in `PATH`, they fail with an `xcrun` error. Put `/usr/bin` first in `PATH`.

## Layout

| Path | Contents |
|---|---|
| `tools/xrecomp/` | the translator from 32-bit Intel code to C, for Mac and Windows games (from xboxrecomp, MIT license; see `tools/xrecomp/UPSTREAM.md`) |
| `tools/hobbit/` | tools that read a Mac game's code: disassembly, system-function patterns, code generators. The Marvel build uses them too |
| `tools/game/` | the game profile (`game.toml`), step 1 of the build, and `feral_frontend.toml`, the byte patterns of the Feral front-end functions the runtime replaces |
| `tools/cghelper/` | the Intel Cg helper |
| `tools/checks/` | `recomp-check` and `boot-check` |
| `tools/package/` | the app and its launcher |
| `tools/perf/` | `plot_frames`, which turns a frame-time CSV file into a chart |
| `runtime/` | start-up, game memory, system-function replacements, the SDL2 front end, a Steam stand-in and the frame-time overlay |
| `build/Makefile` | compiles and links the translated C and the runtime |
| `windows/` | the shared Windows runtime, the Windows-exe tools, tests, build rules and setup script (see `windows/README.md`) |
| `tests/` | translator and runtime tests |

## License

GPL-3.0 (`LICENSE`). `tools/xrecomp/` keeps its MIT notice from xboxrecomp.
