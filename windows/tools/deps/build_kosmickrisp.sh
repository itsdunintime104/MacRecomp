#!/bin/bash
# Build KosmicKrisp (the Mesa Vulkan driver on Metal) from the Mesa 26.2.5
# release with the three patches in tools/package/patches, and install it.
# Usage: windows/tools/deps/build_kosmickrisp.sh WORK_DIR PREFIX
#   WORK_DIR  tarball, source, venv and build tree (default build.nosync/deps/kosmickrisp-work)
#   PREFIX    install prefix (default build.nosync/deps/kosmickrisp); use it as KK_PREFIX
# Skips every step whose output already exists. The build is long (LLVM).
. "$(dirname "$0")/common.sh"

MESA_VERSION=26.2.5
MESA_URL=https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz
MESA_SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
LLVM_MAJOR=23
PATCHES=(drawable-release renderpass-release wait-for-drawable)
PIP_PKGS=(mako==1.4.3 pyyaml==6.0.3 packaging==26.3)

WORK=$(abs_dir "${1:?usage: $0 WORK_DIR PREFIX}")
PREFIX=$(abs_dir "${2:?usage: $0 WORK_DIR PREFIX}")
SRC=$WORK/mesa-$MESA_VERSION
BUILD=$WORK/build
ICD=$PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json

if [ -f "$PREFIX/lib/libvulkan_kosmickrisp.dylib" ] && [ -f "$ICD" ]; then
    echo "kosmickrisp: already installed in $PREFIX"
    exit 0
fi
need_brew meson ninja pkgconf bison glslang llvm spirv-llvm-translator spirv-tools zstd python@3
LLVM_CONFIG=$BREW/opt/llvm/bin/llvm-config
[ "$("$LLVM_CONFIG" --version | cut -d. -f1)" = "$LLVM_MAJOR" ] ||
    die "need LLVM $LLVM_MAJOR (Homebrew llvm is $("$LLVM_CONFIG" --version))"

# 1. Source tarball, checked, then the patches in order.
fetch_sha256 "$MESA_URL" "$WORK/mesa-$MESA_VERSION.tar.xz" "$MESA_SHA256"
if [ ! -f "$WORK/.patched" ]; then
    rm -rf "$SRC"
    tar xf "$WORK/mesa-$MESA_VERSION.tar.xz" -C "$WORK"
    for p in "${PATCHES[@]}"; do
        patch -d "$SRC" -p1 --forward --quiet < "$DEPS_ROOT/tools/package/patches/kosmickrisp-$p.patch"
    done
    touch "$WORK/.patched"
fi

# 2. Python modules Mesa's code generators need.
if [ ! -x "$WORK/venv/bin/python" ]; then
    "$BREW/bin/python3" -m venv "$WORK/venv"
    "$WORK/venv/bin/pip" install -q "${PIP_PKGS[@]}"
fi

# 3. Configure, build, install. The -isystem path: Command Line Tools
#    clang++ does not find <cstdint> without it, and the SPIRV-Tools check fails.
export PATH="$WORK/venv/bin:$BREW/opt/bison/bin:$PATH"
export PKG_CONFIG_PATH="$BREW/opt/spirv-llvm-translator/lib/pkgconfig:$BREW/opt/spirv-tools/lib/pkgconfig:$BREW/lib/pkgconfig"
if [ ! -f "$BUILD/build.ninja" ]; then
    printf "[binaries]\nllvm-config = '%s'\n" "$LLVM_CONFIG" > "$WORK/native.ini"
    meson setup "$BUILD" "$SRC" --native-file "$WORK/native.ini" --prefix="$PREFIX" \
        --buildtype=release -Db_ndebug=true \
        -Dcpp_args="-isystem $SDK/usr/include/c++/v1" \
        -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dllvm=enabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
        -Dplatforms=macos -Dvideo-codecs= -Dvulkan-layers= -Dtools= -Dbuild-tests=false
fi
ninja -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD" --quiet
rpath_ids "$PREFIX/lib"
# Mesa writes the driver's absolute install path into the manifest; name it
# relative to the manifest (the Vulkan loader resolves it from the manifest's
# folder), so the prefix can move.
sed -i '' 's|"library_path": *"[^"]*/|"library_path": "../../../lib/|' "$ICD"
echo "kosmickrisp: installed in $PREFIX"
