#!/bin/sh
# Build the D3D9 smoke test and the C link probe against DXVK Native in $DEPS/dxvk
# (windows/tools/deps/build_dxvk.sh builds it), then run the smoke test on each driver.
# Usage: build.sh [test|run|all]   (default: all)
# DEPS is RECOMP_DEPS_DIR (or HPRECOMP_DEPS_DIR), as in windows/tests/run_all.sh;
# the build goes to $TMPDIR/dxvk_test.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DEPS=${RECOMP_DEPS_DIR:-${HPRECOMP_DEPS_DIR:?set RECOMP_DEPS_DIR to the graphics deps folder}}
PREFIX=$DEPS/dxvk                 # the DXVK install the game links against
WORK=${TMPDIR:-/tmp}/dxvk_test
OUT=$WORK/test
SDK=$(/usr/bin/xcrun --show-sdk-path)
# The Command Line Tools ship an almost empty usr/include/c++/v1, which shadows the SDK's libc++ headers.
CXXFIX="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
KK=$DEPS/kosmickrisp/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
MVK=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
# Copy of the MoltenVK manifest with is_portability_driver=false, so the loader offers MoltenVK
# to an instance that does not set VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR (DXVK does not).
MVK_NP=$WORK/icd/MoltenVK_icd_noportability.json

step=${1:-all}

if [ "$step" = test ] || [ "$step" = all ]; then
  mkdir -p "$OUT"
  /usr/bin/clang++ -std=c++17 -O1 -g $CXXFIX -Wall -Wno-unused-function \
    -I"$PREFIX/include/dxvk" -I/opt/homebrew/include/SDL2 \
    "$HERE/d3d9_triangle.cpp" -o "$OUT/d3d9_triangle" \
    -L"$PREFIX/lib" -ldxvk_d3d9 -Wl,-rpath,"$PREFIX/lib" -L/opt/homebrew/lib -lSDL2
  /usr/bin/clang -std=c11 -Wall -I"$PREFIX/include/dxvk" -I/opt/homebrew/include/SDL2 \
    "$HERE/link_probe.c" -o "$OUT/link_probe" \
    -L"$PREFIX/lib" -ldxvk_d3d9 -Wl,-rpath,"$PREFIX/lib" -L/opt/homebrew/lib -lSDL2
fi

if [ "$step" = run ] || [ "$step" = all ]; then
  LOGS=${TMPDIR:-/tmp}/hp-dxvk-logs
  mkdir -p "${MVK_NP%/*}"
  sed -e 's/"is_portability_driver" *: *true/"is_portability_driver" : false/' \
      -e 's|"library_path": *"[^"]*"|"library_path": "/opt/homebrew/lib/libMoltenVK.dylib"|' "$MVK" > "$MVK_NP"
  for drv in "$KK" "$MVK" "$MVK_NP"; do
    echo "=== VK_DRIVER_FILES=$drv"
    log=$LOGS/$(basename "$drv" .json); mkdir -p "$log"
    rc=0
    VK_DRIVER_FILES=$drv DXVK_WSI_DRIVER=SDL2 DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib \
      DXVK_LOG_LEVEL=debug DXVK_LOG_PATH=$log VK_LOADER_DEBUG=error \
      "$OUT/d3d9_triangle" > "$log/stdout.txt" 2>&1 || rc=$?
    grep -vE '^(info:|debug:|\[mvk-info\])' "$log/stdout.txt"
    grep -hE '^(err|warn):' "$log"/*.log 2>/dev/null | grep -v SetupFPU || true
    echo "exit=$rc"
  done
fi
