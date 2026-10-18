"""Game profile: load and validate game.toml (stdlib only).

Importable as `tools.game.profile`; runnable as
`python3 tools/game/profile.py --shell <game.toml>` (prints shell-quoted KEY=value lines).
"""
from __future__ import annotations

import re
import shlex
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast

_ID_RE = re.compile(r"^[a-z][a-z0-9]*$")
_PREFIX_RE = re.compile(r"^[A-Z][A-Z0-9]*$")
_SHA_RE = re.compile(r"^[0-9a-f]{64}$")
_FORBIDDEN = ('"', "'", "$", "`", "\\", "\n", "%", "|", "&", "<", ">")

_TOP_STR = (
    "id", "title", "window_title", "steam_build", "steam_app", "game_exe",
    "binary_sha256", "binary", "analysis", "switch_tables", "import_sigs", "env_prefix",
)
_TOP_KEYS = frozenset((*_TOP_STR, "steam_app_id", "app", "pins"))
_APP_REQUIRED = ("name", "exe", "bundle_id", "support_dir")
_APP_KEYS = frozenset((*_APP_REQUIRED, "old_name"))


@dataclass(frozen=True)
class Profile:
    root: Path
    id: str
    title: str
    window_title: str
    steam_app_id: int
    steam_build: str
    steam_app: str
    game_exe: str
    binary_sha256: str
    binary: Path
    analysis: Path
    switch_tables: Path
    import_sigs: Path
    env_prefix: str
    app_name: str
    app_exe: str
    bundle_id: str
    support_dir: str
    old_app_name: str | None
    pins: dict[str, int] = field(default_factory=lambda: {})


def _str(table: dict[str, Any], key: str, where: str) -> str:
    if key not in table:
        raise ValueError(f"{where}: missing key '{key}'")
    value = table[key]
    if not isinstance(value, str):
        raise ValueError(f"{where}: key '{key}' must be a string")
    bad = next((c for c in _FORBIDDEN if c in value), None)
    if bad is not None:
        raise ValueError(f"{where}: key '{key}' contains a forbidden character {bad!r}")
    return value


def _unknown(table: dict[str, Any], allowed: frozenset[str], where: str) -> None:
    for key in table:
        if key not in allowed:
            raise ValueError(f"{where}: unknown key '{key}'")


def load(path: str | Path) -> Profile:
    path = Path(path).resolve()
    where = str(path)
    with path.open("rb") as handle:
        raw: dict[str, Any] = tomllib.load(handle)
    _unknown(raw, _TOP_KEYS, where)
    strings = {key: _str(raw, key, where) for key in _TOP_STR}
    app_id = raw.get("steam_app_id")
    if not isinstance(app_id, int) or isinstance(app_id, bool):
        raise ValueError(f"{where}: key 'steam_app_id' must be an integer")
    if not _ID_RE.match(strings["id"]):
        raise ValueError(f"{where}: key 'id' must match {_ID_RE.pattern}")
    if not _PREFIX_RE.match(strings["env_prefix"]):
        raise ValueError(f"{where}: key 'env_prefix' must match {_PREFIX_RE.pattern}")
    if not _SHA_RE.match(strings["binary_sha256"]):
        raise ValueError(f"{where}: key 'binary_sha256' must be 64 lowercase hex digits")

    app = raw.get("app")
    if not isinstance(app, dict):
        raise ValueError(f"{where}: missing table 'app'")
    app_table: dict[str, Any] = app
    _unknown(app_table, _APP_KEYS, f"{where} [app]")
    app_vals = {key: _str(app_table, key, f"{where} [app]") for key in _APP_REQUIRED}
    old_name = _str(app_table, "old_name", f"{where} [app]") if "old_name" in app_table else None

    pins_raw = raw.get("pins", {})
    if not isinstance(pins_raw, dict):
        raise ValueError(f"{where}: key 'pins' must be a table")
    pins: dict[str, int] = {}
    for name, va in cast(dict[str, Any], pins_raw).items():
        if not isinstance(va, int) or isinstance(va, bool):
            raise ValueError(f"{where} [pins]: key '{name}' must be an integer address")
        pins[name] = va

    root = path.parent
    return Profile(
        root=root,
        id=strings["id"],
        title=strings["title"],
        window_title=strings["window_title"],
        steam_app_id=app_id,
        steam_build=strings["steam_build"],
        steam_app=strings["steam_app"],
        game_exe=strings["game_exe"],
        binary_sha256=strings["binary_sha256"],
        binary=root / strings["binary"],
        analysis=root / strings["analysis"],
        switch_tables=root / strings["switch_tables"],
        import_sigs=root / strings["import_sigs"],
        env_prefix=strings["env_prefix"],
        app_name=app_vals["name"],
        app_exe=app_vals["exe"],
        bundle_id=app_vals["bundle_id"],
        support_dir=app_vals["support_dir"],
        old_app_name=old_name,
        pins=pins,
    )


def shell_lines(profile: Profile) -> list[str]:
    pairs = [
        ("GAME_ROOT", str(profile.root)),
        ("GAME_ID", profile.id),
        ("GAME_TITLE", profile.title),
        ("WINDOW_TITLE", profile.window_title),
        ("STEAM_APP_ID", str(profile.steam_app_id)),
        ("STEAM_BUILD", profile.steam_build),
        ("STEAM_APP", profile.steam_app),
        ("GAME_EXE", profile.game_exe),
        ("BINARY_SHA256", profile.binary_sha256),
        ("BINARY", str(profile.binary)),
        ("ANALYSIS", str(profile.analysis)),
        ("SWITCH_TABLES", str(profile.switch_tables)),
        ("IMPORT_SIGS", str(profile.import_sigs)),
        ("ENV_PREFIX", profile.env_prefix),
        ("APP_NAME", profile.app_name),
        ("APP_EXE", profile.app_exe),
        ("BUNDLE_ID", profile.bundle_id),
        ("SUPPORT_DIR", profile.support_dir),
        ("OLD_APP_NAME", profile.old_app_name or ""),
    ]
    return [f"{key}={shlex.quote(value)}" for key, value in pairs]


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[0] != "--shell":
        print("usage: profile.py --shell <game.toml>", file=sys.stderr)
        return 2
    try:
        profile = load(argv[1])
    except (ValueError, OSError, tomllib.TOMLDecodeError) as exc:
        print(f"profile: {exc}", file=sys.stderr)
        return 1
    print("\n".join(shell_lines(profile)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
