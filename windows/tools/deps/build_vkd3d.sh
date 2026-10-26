#!/bin/bash
# Build vkd3d-shader with tools/vkd3d/patches/0001 (mul() as one dot per
# component), and install its libraries and the two public headers we use.
# It first builds widl from Wine and the Perl JSON module; vkd3d needs both.
# Usage: windows/tools/deps/build_vkd3d.sh WORK_DIR PREFIX
#   WORK_DIR  source and build trees (default build.nosync/deps/vkd3d-work)
#   PREFIX    install prefix (default build.nosync/deps/vkd3d); use it as VKD3D_PREFIX
# Skips every step whose output already exists.
. "$(dirname "$0")/common.sh"

VKD3D_URL=https://codeberg.org/vkd3d/vkd3d.git
VKD3D_COMMIT=fed0c3418f815b0bc07533f6fc6f4aa06e8712c3
WINE_URL=https://github.com/wine-mirror/wine.git
WINE_COMMIT=840a939ee526137639665ff5f587224d6a36403f
JSON_URL=https://cpan.metacpan.org/authors/id/I/IS/ISHIGAKI/JSON-4.11.tar.gz
JSON_SHA256=713bdbe724dbb915ed50265ffe47e079a511980cb2427aa19076788bb64c3182
PATCH=$DEPS_ROOT/tools/vkd3d/patches/0001-vkd3d-shader-hlsl-Emit-one-dot-per-component-in-mul.patch

WORK=$(abs_dir "${1:?usage: $0 WORK_DIR PREFIX}")
PREFIX=$(abs_dir "${2:?usage: $0 WORK_DIR PREFIX}")
SRC=$WORK/vkd3d
BUILD=$WORK/build
WIDL=$WORK/winebuild/tools/widl/widl

if [ -f "$PREFIX/lib/libvkd3d-shader.1.dylib" ] && [ -f "$PREFIX/include/vkd3d_shader.h" ]; then
    echo "vkd3d: already installed in $PREFIX"
    exit 0
fi
# Wine's configure needs flex, bison and a PE cross compiler (mingw-w64);
# vkd3d needs GNU libtool and bison 3 (macOS bison 2.3 fails on hlsl.y).
need_brew autoconf automake libtool bison flex mingw-w64 pkgconf vulkan-headers vulkan-loader

# 1. widl, the Wine IDL compiler. Wine's default arch list fails without an
#    aarch64 PE compiler, so configure for x86_64 and build only the tool.
if [ ! -x "$WIDL" ]; then
    git_shallow "$WORK/winesrc" "$WINE_URL" "$WINE_COMMIT"
    mkdir -p "$WORK/winebuild"
    (cd "$WORK/winebuild" &&
        PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH" \
        ../winesrc/configure --enable-tools --enable-archs=x86_64 --disable-tests \
            --without-x --without-freetype --without-gnutls &&
        PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH" make -j "$JOBS" tools/widl/widl)
fi

# 2. Perl JSON (vkd3d generates spirv_grammar.h with it), into WORK only.
export PERL5LIB=$WORK/perl5/lib/perl5
if [ ! -f "$PERL5LIB/JSON.pm" ]; then
    fetch_sha256 "$JSON_URL" "$WORK/JSON-4.11.tar.gz" "$JSON_SHA256"
    tar xzf "$WORK/JSON-4.11.tar.gz" -C "$WORK"
    (cd "$WORK/JSON-4.11" && /usr/bin/perl Makefile.PL INSTALL_BASE="$WORK/perl5" &&
        make && make install)
fi

# 3. vkd3d source at the pinned commit, with the patch.
git_shallow "$SRC" "$VKD3D_URL" "$VKD3D_COMMIT"
if ! git -C "$SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
    git -C "$SRC" apply "$PATCH"
fi

# 4. Configure. Do not pass --without-metal: it breaks configure.
VK_PATH="$BREW/opt/libtool/libexec/gnubin:$BREW/opt/bison/bin:$PATH"
[ -x "$SRC/configure" ] || (cd "$SRC" && PATH="$VK_PATH" ./autogen.sh)
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && PATH="$VK_PATH" CC=/usr/bin/clang OBJC=/usr/bin/clang OTOOL=/usr/bin/otool \
        CPPFLAGS="-I$BREW/include" LDFLAGS="-L$BREW/lib" WIDL="$WIDL" \
        "$SRC/configure" --prefix="$PREFIX" --disable-demos --disable-tests \
            --without-ncurses --without-opengl --without-xcb --without-spirv-tools)
fi

# 5. Build and install the libraries and the headers the runtime includes.
PATH="$VK_PATH" make -C "$BUILD" -j "$JOBS"
PATH="$VK_PATH" make -C "$BUILD" install-libLTLIBRARIES
rpath_ids "$PREFIX/lib"
mkdir -p "$PREFIX/include"
install -m 644 "$SRC/include/vkd3d_shader.h" "$SRC/include/vkd3d_types.h" "$PREFIX/include/"
echo "vkd3d: installed in $PREFIX"
