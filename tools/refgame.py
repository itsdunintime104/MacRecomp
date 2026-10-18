"""Paths of the reference game (The Hobbit, Steam build 84019), used by engine tests and tool defaults.

The engine is the engine/ folder of a game repo, so the game is its parent folder.
RECOMP_GAME_ROOT overrides the game folder; HOBBIT_BIN overrides the binary.
"""
from __future__ import annotations

import os
from pathlib import Path

ENGINE = Path(__file__).resolve().parents[1]
GAME_ROOT = Path(os.environ.get("RECOMP_GAME_ROOT") or ENGINE.parent).resolve()
RECON = GAME_ROOT / "recon"
GAME_TOML = GAME_ROOT / "game.toml"
GENERATED = GAME_ROOT / "generated.nosync"


def binary() -> Path:
    if "HOBBIT_BIN" in os.environ:
        return Path(os.environ["HOBBIT_BIN"])
    return GAME_ROOT / "orig.nosync" / "hobbit_i386"
