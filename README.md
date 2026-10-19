# MacRecomp

Static recompilation of 32-bit Intel Mac games (Feral ports, i386 Mach-O) to native arm64 programs for Apple Silicon.

The translator turns every function of the game binary into C. The runtime stands in for the parts of macOS, OpenGL, Core Audio and Steam that the game calls. The result links into one native program, which runs the game's own data files from your Steam copy.

This repo holds no game code or data. Each game has its own repo, which includes this one as the `engine/` submodule.

## Games

| Game | Steam build | recomp-check | boot-check (60 s) | State |
|---|---|---|---|---|
| LEGO The Hobbit | 84019 | PASS | PASS | Plays through the levels tested so far, with sound; offers the screen's full pixel resolution |
| LEGO Marvel Super Heroes | 329428 | PASS | FAIL (one GL error) | Starts and runs its logos with audio; menus and levels not yet tested |

## Layout

| Path | What |
|---|---|
| `tools/xrecomp/` | i386 to C translator (ported from xboxrecomp, MIT; see `tools/xrecomp/UPSTREAM.md`) |
| `tools/hobbit/` | Mach-O analysis, disassembly sweep, import signatures, generators |
| `tools/game/` | game profile (`game.toml`) and `derive`, which reads per-game values from the binary |
| `tools/game/feral_frontend.toml` | byte signatures of the Feral front-end functions the runtime replaces |
| `runtime/` | native runtime: startup, guest memory, thunks for the imports, SDL2 front end, Steam stand-in |
| `build/Makefile` | compiles and links the generated C and the runtime |
| `tools/checks/` | `recomp-check` and `boot-check` |
| `tools/package/` | app bundle and launcher |
| `tests/` | translator and runtime tests |

## How a game repo uses it

A game repo has `game.toml` at its root, `recon/` (analysis of its binary), and `engine/` (this repo). The game's `setup.sh` runs these steps:

1. `make -C engine/build GAME=$PWD/game.toml game` derives the per-game values into `build.nosync/game/`. They include the entry point, segments, static area, and front-end function and data addresses.
2. The translator writes `generated.nosync/`. Run it from `engine/`: `../.venv/bin/python -m tools.xrecomp.run --analysis ... --switch ... --bin ... --out ../generated.nosync --prefix <id> --overrides ../build.nosync/game/native_overrides.txt`.
3. `make -C engine/build GAME=$PWD/game.toml recomp-check` stops the build if the translation has a gap.
4. `make -C engine/build GAME=$PWD/game.toml` and `make -C engine/build GAME=$PWD/game.toml app` build the program and the app bundle.

`derive` stops with a message when a value cannot be found, for example when a front-end signature matches zero times or more than once. Fix such a case in `game.toml` in one of these ways:

- a `[pins]` entry gives the address directly;
- `absent = [...]` lists the front-end entries the game does not have (an older port with no Retina code, for example). `derive` skips them and writes `FE_HAS_<name>` 0, and the runtime leaves out the code that uses them;
- a `next` rule in `feral_frontend.toml` finds a function as the one directly after a function that was found.

## Checks

**recomp-check** reads the translation and fails if any of these is found:

- an unhandled instruction, other than switch-table data or a `ud2`/`hlt` trap;
- a read of a flag no instruction has set, if it is reachable and changes control flow;
- a function that failed to translate;
- an indirect jump with no known target;
- a decode failure outside a switch table.

`FUZZ=1` also runs the leaf-function fuzzer (about a minute).

**boot-check** runs the built game for `SECONDS` (default 60, at least 30) on a temporary copy of its saves. It passes only if the game is still running at the end and its last stats line is recent. That line must show more than 50 draws per frame, texture uploads and audio renders. The log must contain no GL error, `RECOMP:`, `UNSUPPORTED`, guest-heap or assertion line. `GUARD=1` adds the guest heap guard. It refuses to start while the game is running. Logs go to `build.nosync/checks/` in the game repo.

```
make -C engine/build GAME=$PWD/game.toml boot-check
```

`make -C engine/build GAME=$PWD/game.toml glerr` builds `glerr.dylib`, which names the GL call that raised a GL error.

## Runtime switches

Environment variables read by the game program:

| Variable | Effect |
|---|---|
| `RECOMP_RENDER_SIZE=WxH` | render at this size and scale to the window |
| `RECOMP_STATS=N` | print draws, texture uploads and audio renders every N seconds |
| `RECOMP_GL_CHECK=1` | check `glGetError` once per frame, before the swap |
| `RECOMP_FRAME_DUMP=N`, `RECOMP_FRAME_DIR=dir` | save frames N, 2N, 4N … (8 at most) as PPM files |
| `RECOMP_HANGWATCH=S` | every S seconds, print each guest thread's stack pointer and the latest indirect calls; a thread whose stack pointer stays the same for 3 prints is marked `HANG?` |
| `RECOMP_HEAP_GUARD=2` | guest heap guard |
| `RECOMP_TRACE_OPENS=1` | log file opens |
| `RECOMP_TRACE_AUDIO=1` | log audio graph lifetimes, callbacks, failed calls and each callback's first render |
| `RECOMP_STARTUP_VERBOSE=1` | log import binding and static initialisers at startup |

The other switches are in `runtime/src` (search for `getenv("RECOMP_`).

## Tests

The tests use The Hobbit (Steam build 84019) as the reference game. Run them from `engine/` inside a game repo:

```
../.venv/bin/python -m pytest tests
```

`RECOMP_GAME_ROOT` sets the game folder, and `HOBBIT_BIN` sets the binary.

Some tests compare against Apple's `otool`. If an Intel-only `otool` (MacPorts, for example) comes first in `PATH`, those tests fail with an `xcrun` error. Put `/usr/bin` first in `PATH`.

## License

GPL-3.0 (`LICENSE`). `tools/xrecomp/` keeps its upstream MIT notice.
