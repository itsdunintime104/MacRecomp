# Shared setup for tools/deps/build_*.sh. Source it; do not run it.
# Sets: DEPS_ROOT (the engine's windows/ folder), BREW, JOBS, SDK, CXXFIX, and a clean arm64 PATH.
set -euo pipefail

DEPS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BREW=${BREW_PREFIX:-/opt/homebrew}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

[ "$(uname -m)" = arm64 ] || { echo "error: these builds need an arm64 Mac" >&2; exit 1; }
# Only arm64 tools: an x86_64 tool early in PATH (for example a MacPorts
# otool or perl, or a /usr/local gcc) makes xcrun and configure fail.
export PATH="$BREW/bin:/usr/bin:/bin:/usr/sbin:/sbin"
SDK=$(xcrun --show-sdk-path)
# Command Line Tools 27.0 shipped an almost empty usr/include/c++/v1 that
# hides the SDK libc++ headers (notes/dxvk_triangle.md). Point the compiler at
# the SDK copy only then: on a working toolchain meson drops the -isystem as a
# default include path but keeps -nostdinc++, and nothing then finds <array>.
CXXFIX=
echo '#include <array>' | c++ -x c++ -std=c++17 -fsyntax-only - 2>/dev/null ||
    CXXFIX="-nostdinc++ -isystem $SDK/usr/include/c++/v1"

die() { echo "error: $*" >&2; exit 1; }

# need_brew PKG...: stop with the list of missing Homebrew packages.
need_brew() {
    local p missing=()
    for p in "$@"; do [ -e "$BREW/opt/$p" ] || missing+=("$p"); done
    [ ${#missing[@]} -eq 0 ] || die "missing Homebrew packages: ${missing[*]} (brew install ${missing[*]})"
}

# abs_dir DIR: create DIR and print its absolute path.
abs_dir() { mkdir -p "$1" && (cd "$1" && pwd); }

# git_shallow DIR URL COMMIT: fetch only COMMIT into DIR (skip if DIR exists).
git_shallow() {
    local dir=$1 url=$2 commit=$3
    [ -d "$dir/.git" ] && return 0
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" checkout -q --detach FETCH_HEAD
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || die "$dir is not at $commit"
}

# fetch_sha256 URL FILE SHA256: download URL to FILE once and check its hash.
fetch_sha256() {
    local url=$1 file=$2 sum=$3
    if [ ! -f "$file" ]; then
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | shasum -a 256 -c - >/dev/null || die "sha256 mismatch for $file"
}

# rpath_ids LIBDIR: give each dylib in LIBDIR the install name @rpath/<file>,
# point its references to libraries of the same install at @rpath/<file>, add
# @loader_path as an rpath for those, and re-sign it. libtool and meson record
# the absolute install path, so a prefix that is copied or moved (or was reached
# through a temporary link) leaves names that point nowhere. The game links with
# -rpath <prefix>/lib (build/Makefile), and make_app.sh bundles by @rpath.
rpath_ids() {
    local dir=$1 f old d args sib
    for f in "$dir"/*.dylib; do
        [ -L "$f" ] && continue
        old=$(/usr/bin/otool -D "$f" | tail -n +2)
        args=(-id "@rpath/${f##*/}")
        sib=0
        for d in $(/usr/bin/otool -L "$f" | tail -n +2 | awk '{print $1}'); do
            [ "$d" != "$old" ] && [ "${d%/*}" = "${old%/*}" ] || continue
            args+=(-change "$d" "@rpath/${d##*/}")
            sib=1
        done
        if [ "$sib" -eq 1 ] && ! /usr/bin/otool -l "$f" | awk '$1 == "path" {print $2}' | grep -qx '@loader_path'; then
            args+=(-add_rpath @loader_path)
        fi
        /usr/bin/install_name_tool "${args[@]}" "$f"
        codesign --force -s - "$f"
    done
}
