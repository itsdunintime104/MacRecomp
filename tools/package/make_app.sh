#!/bin/sh
# Build <app name>.app for the game described by game.toml, from build.nosync/<game id> and cghelper.
# Usage: PYTHON=<python> make_app.sh <game.toml>
# The launcher runs the Steam copy of the game binary, so the app works without the repo.
set -eu

ENGINE=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "${PYTHON:-}" ] || { echo "make_app: set PYTHON to a Python 3.11 or newer" >&2; exit 1; }
[ $# -eq 1 ] && [ -f "$1" ] || { echo "make_app: usage: make_app.sh <game.toml>" >&2; exit 1; }
eval "$("$PYTHON" "$ENGINE/tools/game/profile.py" --shell "$1")"
BUILD="$GAME_ROOT/build.nosync"
OUT_APP="$BUILD/$APP_NAME.app"
# Assemble and sign outside the repo: under ~/Documents the file provider adds
# Finder attributes to new folders at any moment, and codesign rejects them.
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/$GAME_ID-app.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$APP_NAME.app"
# The launcher name must match CFBundleExecutable in Info.plist.in.
LAUNCHER_EXE="$APP_EXE"
eval "GAME_APP=\"\${${ENV_PREFIX}_APP:-}\""
GAME_APP="${GAME_APP:-$HOME/Library/Application Support/Steam/steamapps/common/$STEAM_APP}"

for f in "$GAME_ID" cghelper; do
    [ -x "$BUILD/$f" ] || { echo "make_app: missing $BUILD/$f (build first)" >&2; exit 1; }
done

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/$GAME_ID" "$APP/Contents/MacOS/$GAME_ID"
cp "$BUILD/cghelper" "$APP/Contents/MacOS/cghelper"
sed -e "s|@APP_EXE@|$APP_EXE|g" -e "s|@BUNDLE_ID@|$BUNDLE_ID|g" -e "s|@APP_NAME@|$APP_NAME|g" \
    "$ENGINE/tools/package/Info.plist.in" > "$APP/Contents/Info.plist"

# Bundle SDL2 (and SDL3 below) so the app does not need Homebrew at run time.
# "brew --prefix sdl2" can print an alias path without lib/, so use the path the game executable links.
SDL_LINKED=$(otool -L "$APP/Contents/MacOS/$GAME_ID" | awk '/libSDL2-2\.0\.0\.dylib/ {print $1; exit}')
[ -n "$SDL_LINKED" ] || { echo "make_app: $GAME_ID does not link libSDL2-2.0.0.dylib" >&2; exit 1; }
SDL_SRC="$SDL_LINKED"
if [ ! -f "$SDL_SRC" ]; then
    SDL_SRC="$(brew --prefix)/opt/sdl2/lib/libSDL2-2.0.0.dylib"
fi
[ -f "$SDL_SRC" ] || { echo "make_app: SDL2 not found (run: brew install sdl2)" >&2; exit 1; }
SDL_NEW="@executable_path/../Frameworks/libSDL2-2.0.0.dylib"
mkdir -p "$APP/Contents/Frameworks"
cp "$SDL_SRC" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id "$SDL_NEW" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -change "$SDL_LINKED" "$SDL_NEW" "$APP/Contents/MacOS/$GAME_ID"
codesign --force -s - "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"

# Homebrew's sdl2 is now sdl2-compat: SDL2 on top of SDL3, loaded at run time.
# It looks for SDL3 first as libSDL3.dylib next to itself, so bundle SDL3 there.
if grep -q 'Failed loading SDL3 library' "$SDL_SRC"; then
    SDL3_SRC="$(brew --prefix)/opt/sdl3/lib/libSDL3.0.dylib"
    [ -f "$SDL3_SRC" ] || { echo "make_app: SDL3 not found (run: brew install sdl3)" >&2; exit 1; }
    cp "$SDL3_SRC" "$APP/Contents/Frameworks/libSDL3.dylib"
    chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
    install_name_tool -id "@loader_path/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
    codesign --force -s - "$APP/Contents/Frameworks/libSDL3.dylib"
fi

# Icon: copy the game's icon into the local build output (never committed).
ICON="$GAME_APP/Contents/Resources/GameIcon.icns"
if [ ! -f "$ICON" ]; then
    ICON=$(ls "$GAME_APP"/Contents/Resources/*.icns 2>/dev/null | grep -v DocIcon | head -n 1 || true)
fi
if [ -n "$ICON" ] && [ -f "$ICON" ]; then
    cp "$ICON" "$APP/Contents/Resources/AppIcon.icns"
    plutil -insert CFBundleIconFile -string AppIcon "$APP/Contents/Info.plist"
fi

# The launcher defaults to the game path in effect now; the path is single-quote-escaped.
DEFAULT_ESC=$(printf '%s' "$GAME_APP" | sed "s/'/'\\\\''/g")
{
    echo '#!/bin/sh'
    echo "DEFAULT_${ENV_PREFIX}_APP='$DEFAULT_ESC'"
    sed -e "s|@ENV_PREFIX@|$ENV_PREFIX|g" -e "s|@GAME_TITLE@|$GAME_TITLE|g" -e "s|@APP_NAME@|$APP_NAME|g" \
        -e "s|@SUPPORT_DIR@|$SUPPORT_DIR|g" -e "s|@GAME_EXE@|$GAME_EXE|g" -e "s|@GAME_ID@|$GAME_ID|g" \
        "$ENGINE/tools/package/launcher.sh.in"
} > "$APP/Contents/MacOS/$LAUNCHER_EXE"
chmod +x "$APP/Contents/MacOS/$LAUNCHER_EXE"

xattr -cr "$APP"
codesign --force --deep -s - "$APP"
rm -rf "$OUT_APP"
[ -z "$OLD_APP_NAME" ] || rm -rf "$BUILD/$OLD_APP_NAME"   # the bundle's old name
ditto "$APP" "$OUT_APP"
echo "built $OUT_APP"
