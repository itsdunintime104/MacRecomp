#!/bin/sh
# One-command setup for a Windows (PE32) game repo: checks the machine, finds
# the user's own Windows copy of the game, translates it, builds the native
# program and installs the app. The game repo's game.conf names the game
# (windows/README.md); its setup.sh runs this script:
#   exec "$(dirname "$0")/engine/windows/setup.sh" "$@"
# Usage: ./setup.sh [--check] [--force] [--no-install] [<post-install option>] [--dry-run] [--help] [game folder]
set -eu

# The engine (lifter and generic runtime) holds this script; the game repo
# holds the engine as engine/ (GAME_ROOT overrides).
ENGINE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${GAME_ROOT:-$(cd "$ENGINE/.." && pwd)}
cd "$ROOT"
[ -f "$ROOT/game.conf" ] || { echo "setup: no game.conf in $ROOT" >&2; exit 2; }
POST_INSTALL_OPTION=""
POST_INSTALL_HELP=""
. "$ROOT/game.conf"
STEPS=8
if command -v post_install >/dev/null 2>&1; then STEPS=9; fi
for v in GAME_ID APP_NAME APP_EXE APP_SAVE_NAME APP_ENV_PREFIX APP_GAME_FILE APP_GAME_TITLE APP_IMAGE \
    BINARY_SHA256 LIFT_PREFIX RECON_ANALYSIS RECON_SWITCH GAME_FOLDER GOG_GLOB DATA_GLOB STORE_HINT VERSION_HINT; do
    eval "[ -n \"\${$v:-}\" ]" || { echo "setup: game.conf does not set $v" >&2; exit 2; }
done
# The user's settings: <APP_ENV_PREFIX>_<name>, for example HPRECOMP_GAME_DIR.
env_setting() { eval "printf '%s' \"\${${APP_ENV_PREFIX}_$1:-}\""; }
GAME_EXE=$APP_GAME_FILE
ORIG="$ROOT/orig.nosync/$APP_IMAGE"
GEN="$ROOT/generated.nosync"
VENV="$ROOT/.venv"
WIN="$ENGINE/windows"
# The graphics libraries windows/tools/deps builds: DXVK, vkd3d-shader, patched KosmicKrisp.
DEPS=$(env_setting DEPS_DIR)
DEPS=${DEPS:-$ROOT/build.nosync/deps}
BUILD_LOCK=$(env_setting BUILD_LOCK)
DXVK_PREFIX="$DEPS/dxvk"
VKD3D_PREFIX="$DEPS/vkd3d"
KK_PREFIX="$DEPS/kosmickrisp"

CHECK_ONLY=0
FORCE=0
INSTALL=1
POST_INSTALL=1
DRY_RUN=0
GAME_ARG=""

usage() {
    cat <<USAGE
Usage: ./setup.sh [options] [game folder]

Builds the app from your own copy of the Windows game ($APP_GAME_TITLE).

game folder   The folder that holds $GAME_EXE and its $DATA_GLOB files,
              or the path of $GAME_EXE. Without it, setup.sh
              searches Steam under Wine, CrossOver and Whisky, other Steam
              libraries, GOG installs, ~/Games and external drives, and asks
              for the folder if it finds none.

Options:
  --check       Check the requirements only, then stop.
  --force       Translate again even if generated.nosync is complete.
  --no-install  Build the app but do not copy it to ~/Applications.
USAGE
    if [ -n "$POST_INSTALL_OPTION" ]; then
        printf '  %-12s  %s\n' "$POST_INSTALL_OPTION" "$POST_INSTALL_HELP"
    fi
    cat <<USAGE
  --dry-run     Print each step and its commands without running them.
  --help        Show this text.

Environment:
  ${APP_ENV_PREFIX}_GAME_DIR  the game folder, if you do not give it as an argument.
  ${APP_ENV_PREFIX}_DEPS_DIR  where windows/tools/deps builds and installs the
      graphics libraries (default: build.nosync/deps).
USAGE
}

for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --force) FORCE=1 ;;
        --no-install) INSTALL=0 ;;
        --dry-run) DRY_RUN=1 ;;
        --help|-h) usage; exit 0 ;;
        -*)
            if [ -n "$POST_INSTALL_OPTION" ] && [ "$arg" = "$POST_INSTALL_OPTION" ]; then
                POST_INSTALL=0
            else
                echo "setup: unknown option: $arg" >&2; usage >&2; exit 2
            fi ;;
        *)
            if [ -n "$GAME_ARG" ]; then
                echo "setup: give only one game folder" >&2; usage >&2; exit 2
            fi
            GAME_ARG=$arg ;;
    esac
done

step() { echo "==> $*"; }
fail() { echo "setup: $*" >&2; exit 1; }
# Print a command; run it unless this is a dry run.
run() {
    printf '+'
    for a in "$@"; do
        case "$a" in *" "*) printf " '%s'" "$a" ;; *) printf ' %s' "$a" ;; esac
    done
    echo
    [ "$DRY_RUN" -eq 1 ] || "$@"
}
# Long lift and build commands share the job build lock when <APP_ENV_PREFIX>_BUILD_LOCK is set.
run_locked() {
    if [ -n "$BUILD_LOCK" ]; then
        run /usr/bin/lockf -k "$BUILD_LOCK" "$@"
    else
        run "$@"
    fi
}

MISSING=0
ok() { printf 'OK       %s\n' "$1"; }
missing() { printf 'MISSING  %s\n         fix: %s\n' "$1" "$2"; MISSING=$((MISSING + 1)); }
need_file() { if [ -e "$2" ]; then ok "$1 ($2)"; else missing "$1: $2" "$3"; fi; }

# The Homebrew packages windows/tools/deps/build_$1.sh needs (its need_brew line).
dep_pkgs() { sed -n 's/^need_brew //p' "$WIN/tools/deps/build_$1.sh"; }

# The graphics libraries not built yet, and the Homebrew packages that build them.
check_deps() {
    todo=""
    [ -f "$DXVK_PREFIX/lib/libdxvk_d3d9.0.dylib" ] || todo="$todo dxvk"
    [ -f "$VKD3D_PREFIX/lib/libvkd3d-shader.1.dylib" ] || todo="$todo vkd3d"
    [ -f "$KK_PREFIX/lib/libvulkan_kosmickrisp.dylib" ] || todo="$todo kosmickrisp"
    if [ -z "$todo" ]; then
        ok "DXVK, vkd3d-shader, KosmicKrisp ($DEPS)"
        return
    fi
    printf 'TO BUILD %s (step 5, into %s)\n' "${todo# }" "$DEPS"
    absent=""
    for p in $(for d in $todo; do dep_pkgs "$d"; done | tr ' ' '\n' | sort -u); do
        [ -e "$BREW_PREFIX/opt/$p" ] || absent="$absent $p"
    done
    if [ -n "$absent" ]; then
        missing "Homebrew packages to build them:$absent" "brew install$absent"
    else
        ok "Homebrew packages to build them"
    fi
    case "$todo" in *kosmickrisp*)
        want=$(sed -n 's/^LLVM_MAJOR=//p' "$WIN/tools/deps/build_kosmickrisp.sh")
        have=$("$BREW_PREFIX/opt/llvm/bin/llvm-config" --version 2>/dev/null | cut -d. -f1) || have=""
        if [ "$have" = "$want" ]; then ok "LLVM $want"; else missing "LLVM $want (Homebrew llvm: ${have:-not installed})" "brew install llvm (KosmicKrisp needs version $want)"; fi
    esac
}

# Print the first python3 on PATH that is 3.11 or newer (the default python3 may be older).
find_python() {
    for cand in python3 python3.14 python3.13 python3.12 python3.11; do
        if command -v "$cand" >/dev/null 2>&1 \
            && "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
            command -v "$cand"
            return
        fi
    done
}

# Run one build tool with a harmless command and report it.
# Usage: check_tool NAME FIX COMMAND...
# A tool that is found but fails is often another copy earlier in PATH
# (for example MacPorts); Apple's copy is in /usr/bin.
check_tool() {
    name=$1; fix=$2; shift 2
    path=$(command -v "$1" 2>/dev/null) || { missing "$name: not found" "$fix"; return; }
    if err=$("$@" 2>&1 >/dev/null); then
        ok "$name ($path)"
        return
    fi
    case "$path" in
        /usr/bin/*) ;;
        *) fix="$path is not Apple's copy. Put Apple's tools first: PATH=/usr/bin:\$PATH ./setup.sh" ;;
    esac
    missing "$name: $path fails: $(printf '%s\n' "$err" | head -n 1 | cut -c 1-120)" "$fix"
}

check_build_tools() {
    T=$(mktemp -d "${TMPDIR:-/tmp}/recomp-setup.XXXXXX")
    trap 'rm -rf "$T"' EXIT INT TERM
    printf 'int main(void) { return 0; }\n' > "$T/t.c"
    check_tool "C compiler (clang, arm64)" "xcode-select --install" clang -arch arm64 "$T/t.c" -o "$T/t"
    check_tool "make" "xcode-select --install" make --version
    check_tool "otool" "xcode-select --install" /usr/bin/otool -h /usr/bin/true
    if [ -f "$T/t" ]; then
        check_tool "install_name_tool" "xcode-select --install" /usr/bin/install_name_tool -add_rpath @loader_path "$T/t"
        check_tool "codesign" "xcode-select --install" codesign --force -s - "$T/t"
    else
        printf 'SKIPPED  install_name_tool, codesign (need the C compiler)\n'
    fi
    rm -rf "$T"
    trap - EXIT INT TERM
}

preflight() {
    step "1/$STEPS Checking requirements"

    if [ "$(uname -s)" = Darwin ]; then ok "macOS"; else missing "macOS (this is $(uname -s))" "run this on a Mac"; fi
    if [ "$(uname -m)" = arm64 ]; then ok "Apple Silicon (arm64)"; else missing "Apple Silicon (this is $(uname -m))" "use a Mac with an M-series chip"; fi

    if xcode-select -p >/dev/null 2>&1; then
        ok "Xcode Command Line Tools"
        check_build_tools
    else
        missing "Xcode Command Line Tools (C compiler, make, otool)" "xcode-select --install"
    fi

    BREW_PREFIX=""
    if command -v brew >/dev/null 2>&1; then
        BREW_PREFIX=$(brew --prefix)
        # windows/tools/deps reads it too.
        export BREW_PREFIX
        ok "Homebrew ($BREW_PREFIX)"
        # "brew --prefix <formula>" can print an alias path without lib/, so build paths from the Homebrew prefix.
        need_file "SDL2 (sdl2-compat)" "$BREW_PREFIX/opt/sdl2/lib/libSDL2-2.0.0.dylib" "brew install sdl2"
        need_file "SDL3" "$BREW_PREFIX/opt/sdl3/lib/libSDL3.0.dylib" "brew install sdl3"
        need_file "FAudio" "$BREW_PREFIX/opt/faudio/lib/libFAudio.0.dylib" "brew install faudio"
        need_file "Vulkan loader" "$BREW_PREFIX/opt/vulkan-loader/lib/libvulkan.1.dylib" "brew install vulkan-loader"
        check_deps
    else
        missing "Homebrew (needed for SDL2, SDL3, FAudio, Vulkan and the graphics library builds)" \
            "install Homebrew from https://brew.sh, then run ./setup.sh --check again"
    fi
    need_file "engine (engine/ submodule)" "$ENGINE/tools/xrecomp/run.py" "git submodule update --init"

    PYTHON=$(find_python)
    if [ -n "$PYTHON" ]; then ok "Python 3.11 or newer ($PYTHON)"; else missing "Python 3.11 or newer" "brew install python"; fi

    FREE_KB=$(df -k "$ROOT" | awk 'NR==2 {print $4}')
    if [ "${FREE_KB:-0}" -ge 4194304 ]; then
        ok "Free disk space (at least 4 GB)"
    else
        missing "Free disk space (at least 4 GB)" "free up disk space"
    fi

    if [ "$MISSING" -gt 0 ]; then
        echo "setup: $MISSING requirement(s) missing. Run the fix commands above, then run ./setup.sh again." >&2
        # A dry run shows the remaining steps anyway.
        [ "$DRY_RUN" -eq 1 ] || exit 1
    fi
}

# Print the Wine prefixes: ~/.wine*, CrossOver and Whisky bottles, Wine app
# wrappers in ~/Applications, and Heroic prefixes.
wine_prefixes() {
    for p in "$HOME"/.wine* \
        "$HOME/Library/Application Support/CrossOver/Bottles"/* \
        "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"/* \
        "$HOME/Applications"/*.app/Contents/SharedSupport/prefix \
        "$HOME/Games/Heroic/Prefixes"/*; do
        if [ -d "$p/drive_c" ]; then printf '%s\n' "$p"; fi
    done
}

# Print each library folder in the libraryfolders.vdf of the Steam in $1.
# $2 is the Wine prefix of that Steam (empty for Mac Steam); a Windows path
# such as D:\\SteamLibrary maps through its dosdevices/d: link.
steam_libraries() {
    vdf="$1/steamapps/libraryfolders.vdf"
    [ -f "$vdf" ] || return 0
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | while IFS= read -r lib; do
        case "$lib" in
            [A-Za-z]:*)
                if [ -n "$2" ]; then
                    drive=$(printf '%s' "$lib" | cut -c 1 | tr 'A-Z' 'a-z')
                    rest=$(printf '%s' "${lib#?:}" | sed -e 's/\\\\/\//g' -e 's/\\/\//g')
                    printf '%s\n' "$2/dosdevices/$drive:$rest"
                fi ;;
            /*) printf '%s\n' "$lib" ;;
        esac
    done
}

# Print every folder setup.sh searches for the game, in order.
game_search_dirs() {
    STEAM_REL="Program Files (x86)/Steam/steamapps/common/$GAME_FOLDER"
    for dir in "$HOME"/.wine*/drive_c/"$STEAM_REL" \
        "$HOME/Library/Application Support/CrossOver/Bottles"/*/drive_c/"$STEAM_REL" \
        "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"/*/drive_c/"$STEAM_REL" \
        "$HOME/Library/Application Support/Steam/steamapps/common/$GAME_FOLDER" \
        "$HOME/Games/$GAME_FOLDER"; do
        printf '%s\n' "$dir"
    done
    wine_prefixes | while IFS= read -r p; do
        for dir in "$p/drive_c/GOG Games/"$GOG_GLOB \
            "$p/drive_c/Program Files (x86)/GOG Galaxy/Games/"$GOG_GLOB; do
            printf '%s\n' "$dir"
        done
    done
    {
        wine_prefixes | while IFS= read -r p; do
            steam_libraries "$p/drive_c/Program Files (x86)/Steam" "$p"
        done
        steam_libraries "$HOME/Library/Application Support/Steam" ""
    } | while IFS= read -r lib; do
        printf '%s\n' "$lib/steamapps/common/$GAME_FOLDER"
    done
    for vol in /Volumes/*; do
        # Skip the link to the boot volume.
        if [ -L "$vol" ]; then continue; fi
        for dir in "$vol/SteamLibrary/steamapps/common/$GAME_FOLDER" \
            "$vol/Steam/steamapps/common/$GAME_FOLDER" \
            "$vol/Program Files (x86)/Steam/steamapps/common/$GAME_FOLDER" \
            "$vol/Games/$GAME_FOLDER" "$vol/$GAME_FOLDER" \
            "$vol/GOG Games/"$GOG_GLOB; do
            printf '%s\n' "$dir"
        done
    done
}

# Print the folder for $1, a game folder or the path of its exe, as an
# absolute path. Fails when the folder has no game exe.
game_dir_from() {
    p=${1%/}
    case "$p" in
        "$GAME_EXE") p=. ;;
        */"$GAME_EXE") p=${p%/*} ;;
    esac
    case "$p" in /*) ;; *) p="./$p" ;; esac
    [ -f "$p/$GAME_EXE" ] || return 1
    (cd "$p" && pwd -P)
}

# Set GAME_DIR to the first folder found whose exe matches BINARY_SHA256,
# else the first found, else empty. Lists the copies when it finds more than one.
find_game_dir() {
    found=$(game_search_dirs | while IFS= read -r dir; do
        if [ -f "$dir/$GAME_EXE" ]; then (cd "$dir" && pwd -P); fi
    done | awk '!seen[$0]++')
    GAME_DIR=""
    [ -n "$found" ] || return 0
    count=$(printf '%s\n' "$found" | wc -l | tr -d ' ')
    if [ "$count" -gt 1 ]; then echo "found $count copies of $GAME_EXE:"; fi
    while IFS= read -r dir; do
        sum=$(shasum -a 256 "$dir/$GAME_EXE" | awk '{print $1}')
        if [ "$sum" = "$BINARY_SHA256" ]; then
            label="matches"
            if [ -z "$GAME_DIR" ]; then GAME_DIR=$dir; fi
        else
            label="different build"
        fi
        if [ "$count" -gt 1 ]; then echo "  $dir ($label)"; fi
    done <<EOF
$found
EOF
    if [ -z "$GAME_DIR" ]; then GAME_DIR=$(printf '%s\n' "$found" | head -n 1); fi
}

# Clean a path dragged into Terminal: trim spaces, strip one pair of quotes,
# undo backslash escapes.
clean_path() {
    printf '%s\n' "$1" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
        -e "s/^'\(.*\)'\$/\1/" -e 's/^"\(.*\)"$/\1/' -e 's/\\\(.\)/\1/g'
}

# Ask for the game folder, up to 3 times. Sets GAME_DIR; fails if no answer has the exe.
ask_game_dir() {
    echo "Searched Steam under Wine, CrossOver and Whisky, Mac Steam, Steam libraries, GOG folders, ~/Games and /Volumes."
    tries=0
    while [ "$tries" -lt 3 ]; do
        tries=$((tries + 1))
        printf 'Drag the %s folder (or %s) into this window and press Return: ' "$GAME_FOLDER" "$GAME_EXE"
        IFS= read -r line || return 1
        answer=$(clean_path "$line")
        if GAME_DIR=$(game_dir_from "$answer"); then return 0; fi
        echo "setup: $GAME_EXE not found in $answer" >&2
    done
    GAME_DIR=""
    return 1
}

locate_and_copy_game() {
    step "2/$STEPS Locating the game"
    given=${GAME_ARG:-$(env_setting GAME_DIR)}
    if [ -n "$given" ]; then
        GAME_DIR=$(game_dir_from "$given") || fail "$GAME_EXE not found in $given"
    else
        find_game_dir
        if [ -z "$GAME_DIR" ] && [ "$DRY_RUN" -eq 0 ] && [ -t 0 ]; then
            ask_game_dir || GAME_DIR=""
        fi
    fi
    if [ -z "$GAME_DIR" ]; then
        msg="$GAME_EXE not found. $STORE_HINT,
or set ${APP_ENV_PREFIX}_GAME_DIR to the folder that holds $GAME_EXE,
or run ./setup.sh \"<game folder>\"."
        [ "$DRY_RUN" -eq 1 ] || fail "$msg"
        echo "setup: $msg" >&2
        GAME_DIR='<game folder>'
    else
        echo "found $GAME_DIR/$GAME_EXE"
        ls "$GAME_DIR"/$DATA_GLOB >/dev/null 2>&1 || fail "no $DATA_GLOB files next to $GAME_EXE; the install is incomplete"
    fi

    step "3/$STEPS Copying and verifying the game binary"
    if [ -f "$ORIG" ] && cmp -s "$GAME_DIR/$GAME_EXE" "$ORIG"; then
        echo "$ORIG is up to date"
    else
        run mkdir -p "$(dirname "$ORIG")"
        run cp "$GAME_DIR/$GAME_EXE" "$ORIG"
    fi
    echo "+ shasum -a 256 $ORIG"
    [ "$DRY_RUN" -eq 1 ] && return
    SHA=$(shasum -a 256 "$ORIG" | awk '{print $1}')
    [ "$SHA" = "$BINARY_SHA256" ] || fail "the game binary does not match (sha256 $SHA).
$VERSION_HINT"
    echo "sha256 matches"
}

setup_venv() {
    step "4/$STEPS Python environment"
    if [ -x "$VENV/bin/python" ] && "$VENV/bin/python" -c 'import capstone' >/dev/null 2>&1; then
        echo ".venv is ready"
    else
        run "$PYTHON" -m venv "$VENV"
        run "$VENV/bin/python" -m pip install --quiet capstone
    fi
}

# Each tools/deps script skips the steps whose output already exists.
build_deps() {
    step "5/$STEPS Building the graphics libraries (the first run takes several minutes)"
    for d in dxvk vkd3d kosmickrisp; do
        run_locked "$WIN/tools/deps/build_$d.sh" "$DEPS/$d-work" "$DEPS/$d"
    done
}

translate() {
    step "6/$STEPS Translating the game code"
    if [ -f "$GEN/lift_report.json" ] && [ "$FORCE" -eq 0 ]; then
        echo "generated.nosync is complete; skipping (use --force to translate again)"
        return
    fi
    # The game repo's recon files hold the committed function analysis of this exact binary.
    run_locked "$VENV/bin/python" -I "$WIN/tools/pe/run_lift_pe.py" --engine "$ENGINE" \
        --analysis "$ROOT/$RECON_ANALYSIS" --switch "$ROOT/$RECON_SWITCH" \
        --bin "$ORIG" --out "$GEN" --prefix "$LIFT_PREFIX" --jobs "$NCPU"
}

build() {
    step "7/$STEPS Building the game and the app"
    run_locked make -C "$ROOT/build" -j"$NCPU" ENGINE="$ENGINE" GEN="$GEN" DEPS="$DEPS" \
        BREW_PREFIX="$BREW_PREFIX" GAME_DIR="$GAME_DIR" GAME_IMAGE="$ORIG" app
}

install_app() {
    step "8/$STEPS Installing"
    APP="$ROOT/build.nosync/$APP_NAME.app"
    [ "$DRY_RUN" -eq 1 ] || [ -d "$APP" ] || fail "build did not produce $APP"
    if [ "$INSTALL" -eq 1 ]; then
        DEST="$HOME/Applications/$APP_NAME.app"
        run mkdir -p "$HOME/Applications"
        run rm -rf "$DEST"
        # -X: leave out extended attributes. A build folder under ~/Documents
        # carries file-sync attributes that fail codesign --strict.
        run cp -RX "$APP" "$DEST"
        # One app only: macOS lists every app bundle it finds, so the build copy
        # would show up in the launcher as a second game.
        run rm -rf "$APP"
        APP="$DEST"
    else
        echo "built $APP (not installed)"
    fi
}

preflight
if [ "$CHECK_ONLY" -eq 1 ]; then
    [ "$MISSING" -eq 0 ] && echo "All requirements are met."
    exit 0
fi
NCPU=$(sysctl -n hw.ncpu)
locate_and_copy_game
setup_venv
build_deps
translate
build
install_app
# game.conf may define post_install, a last step run after the install (for
# example a shader cache warm-up); POST_INSTALL_OPTION turns it off.
if command -v post_install >/dev/null 2>&1; then
    if [ "$POST_INSTALL" -eq 1 ]; then
        post_install
    else
        echo "==> skipped the post-install step ($POST_INSTALL_OPTION)"
    fi
fi
echo
echo "Start it by double-clicking $APP, or run: open \"$APP\""
echo "Saves: $HOME/Library/Application Support/$APP_SAVE_NAME/"
echo "Logs: $HOME/Library/Logs/$APP_SAVE_NAME/"
