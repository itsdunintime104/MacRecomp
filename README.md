# MacRecomp

Static recompilation of 32-bit Intel Mac games (Feral ports, i386 Mach-O) to native arm64 programs for Apple Silicon.

The translator turns every function of the game binary into C. The runtime stands in for the parts of macOS, OpenGL, Core Audio and Steam that the game calls. The result links into one native program, which runs the game's own data files from your Steam copy.

This repo holds no game code or data. Each game has its own repo, which includes this one as the `engine/` submodule.

## Layout

| Path | What |
|---|---|
| `tools/xrecomp/` | i386 to C translator (ported from xboxrecomp, MIT; see `tools/xrecomp/UPSTREAM.md`) |
| `tools/hobbit/` | Mach-O analysis, disassembly sweep, import signatures, generators |
| `tools/game/` | game profile (`game.toml`) and `derive`, which reads per-game values from the binary |
| `tools/game/feral_frontend.toml` | byte signatures of the Feral front-end functions the runtime replaces |
| `runtime/` | native runtime: startup, guest memory, thunks for the imports, SDL2 front end, Steam stand-in |
| `build/Makefile` | compiles and links the generated C and the runtime |
| `tools/package/` | app bundle and launcher |
| `tests/` | translator and runtime tests |

## How a game repo uses it

A game repo has `game.toml` at its root, `recon/` (analysis of its binary), and `engine/` (this repo). The game's `setup.sh` runs these steps:

1. `make -C engine/build GAME=$PWD/game.toml game` derives the per-game values into `build.nosync/game/`. They include the entry point, segments, static area, and front-end function and data addresses.
2. The translator writes `generated.nosync/`. Run it from `engine/`: `../.venv/bin/python -m tools.xrecomp.run --analysis ... --switch ... --bin ... --out ../generated.nosync --prefix <id> --overrides ../build.nosync/game/native_overrides.txt`.
3. `make -C engine/build GAME=$PWD/game.toml` and `make -C engine/build GAME=$PWD/game.toml app` build the program and the app bundle.

`derive` stops with a message when a value cannot be found, for example when a front-end signature matches zero times or more than once. Fix such a case with a `[pins]` entry in `game.toml` or a new signature.

## Tests

The tests use The Hobbit (Steam build 84019) as the reference game. Run them from `engine/` inside a game repo:

```
../.venv/bin/python -m pytest tests
```

`RECOMP_GAME_ROOT` sets the game folder, and `HOBBIT_BIN` sets the binary.

## License

GPL-3.0 (`LICENSE`). `tools/xrecomp/` keeps its upstream MIT notice.
