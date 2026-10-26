#!/bin/bash
# Build DXVK Native (d3d9 only, SDL2 window system) with the four macOS
# patches in tools/dxvk_test/patches, and install it.
# Usage: windows/tools/deps/build_dxvk.sh WORK_DIR PREFIX
#   WORK_DIR  source and build trees (default build.nosync/deps/dxvk-work)
#   PREFIX    install prefix (default build.nosync/deps/dxvk); use it as DXVK_PREFIX
# Skips every step whose output already exists.
. "$(dirname "$0")/common.sh"

DXVK_URL=https://github.com/doitsujin/dxvk.git
DXVK_COMMIT=02e2028c3fe954c8913b1f39bce2f581739f0c41
PATCHES=$DEPS_ROOT/tools/dxvk_test/patches

WORK=$(abs_dir "${1:?usage: $0 WORK_DIR PREFIX}")
PREFIX=$(abs_dir "${2:?usage: $0 WORK_DIR PREFIX}")
SRC=$WORK/dxvk
BUILD=$WORK/build-sdl2

if [ -f "$PREFIX/lib/libdxvk_d3d9.0.dylib" ]; then
    echo "dxvk: already installed in $PREFIX"
    exit 0
fi
need_brew meson ninja pkgconf glslang sdl2

# Full clone: DXVK takes its version string from git describe.
if [ ! -d "$SRC/.git" ]; then
    git clone -q "$DXVK_URL" "$SRC"
    git -C "$SRC" checkout -q --detach "$DXVK_COMMIT"
    git -C "$SRC" submodule -q update --init --recursive
fi

if [ ! -f "$WORK/.patched" ]; then
    [ "$(git -C "$SRC" rev-parse HEAD)" = "$DXVK_COMMIT" ] || die "$SRC is not at $DXVK_COMMIT"
    # A fixed committer and date give the same commit ids, and so the same
    # version string, on every machine.
    git -C "$SRC" -c user.name=hp-deps -c user.email=hp-deps@invalid \
        am -q --keep-non-patch --committer-date-is-author-date "$PATCHES"/0*.patch
    touch "$WORK/.patched"
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    CXXFLAGS="$CXXFIX" \
    meson setup "$BUILD" "$SRC" --buildtype=release --prefix="$PREFIX" \
        -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
        -Denable_d3d9=true -Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled \
        -Dnative_macos=true
fi
ninja -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD" --quiet
rpath_ids "$PREFIX/lib"
echo "dxvk: installed in $PREFIX"
