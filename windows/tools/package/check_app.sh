#!/bin/sh
# Check that an app built by make_app.sh is self-contained. It never starts the game.
# Usage: check_app.sh <app>
#  1. Every Mach-O in the app loads only /usr/lib, /System and @rpath,
#     @executable_path or @loader_path names, and has only such rpaths.
#  2. Each bundled Vulkan ICD manifest names its library relative to itself,
#     and that file exists.
#  3. codesign --verify --deep passes.
#  4. The launcher sets VK_DRIVER_FILES (to a manifest inside the app),
#     DXVK_WSI_DRIVER=SDL2 and DXVK_LOG_PATH, and no DYLD_* variable.
#  5. DXVK opens SDL2 by its leaf name (dxvk src/wsi/sdl2/wsi_platform_sdl2.cpp):
#     a probe library with DXVK's rpath, in a copy of Frameworks, opens
#     libSDL2-2.0.0.dylib by leaf name with no DYLD_* variable set, and gets the
#     bundled copy.
# Prints one line per check and exits 1 if any fails.
set -eu
[ $# -eq 1 ] && [ -d "$1/Contents" ] || { echo "check_app: usage: check_app.sh <app>" >&2; exit 2; }
APP=$(cd "$1" && pwd)
C=$APP/Contents
OTOOL=/usr/bin/otool
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok: $*"; }
allowed() { case "$1" in /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) return 0 ;; esac; return 1; }

# 1. Load commands of every Mach-O file. The app name has spaces: split on newlines.
n=0
before=$FAILS
NL='
'
OLDIFS=$IFS IFS=$NL
for f in $(find "$C/MacOS" "$C/Frameworks" -type f); do
    IFS=$OLDIFS
    file -b "$f" | grep -q Mach-O || continue
    n=$((n + 1))
    for d in $($OTOOL -L "$f" | tail -n +2 | awk '{print $1}'); do
        allowed "$d" || fail "${f#"$C"/} loads $d"
    done
    for r in $($OTOOL -l "$f" | awk '$1 == "path" {print $2}'); do
        case "$r" in @executable_path*|@loader_path*) ;; *) fail "${f#"$C"/} has rpath $r" ;; esac
    done
done
IFS=$OLDIFS
[ "$n" -gt 1 ] || fail "found $n Mach-O files"
[ "$FAILS" -eq "$before" ] && ok "load commands of $n Mach-O files"

# 2. ICD manifests.
for j in "$C"/Resources/vulkan/icd.d/*.json; do
    [ -f "$j" ] || { fail "no ICD manifest"; break; }
    lib=$(sed -n 's/.*"library_path": *"\([^"]*\)".*/\1/p' "$j")
    case "$lib" in
        /*|"") fail "${j##*/} library_path is '$lib'" ;;
        *) if [ -f "$(dirname "$j")/$lib" ]; then ok "${j##*/} -> $lib"; else fail "${j##*/} -> $lib (missing)"; fi ;;
    esac
done

# 3. Signature.
if out=$(codesign --verify --deep "$APP" 2>&1); then ok "codesign --verify --deep"; else fail "codesign: $out"; fi

# 4. Launcher.
EXE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$C/Info.plist")
L=$C/MacOS/$EXE
if [ -x "$L" ]; then
    if grep -q DYLD_ "$L"; then fail "launcher $EXE mentions DYLD_"; else ok "launcher $EXE sets no DYLD_ variable"; fi
    icd=$(sed -n 's|^export VK_DRIVER_FILES="\$CONTENTS/\(.*\)"$|\1|p' "$L")
    if [ -n "$icd" ] && [ -f "$C/$icd" ]; then ok "launcher VK_DRIVER_FILES=$icd"; else fail "launcher VK_DRIVER_FILES '$icd'"; fi
    grep -qx 'export DXVK_WSI_DRIVER=SDL2' "$L" || fail "launcher does not set DXVK_WSI_DRIVER=SDL2"
    grep -q '^export DXVK_LOG_PATH=' "$L" || fail "launcher does not set DXVK_LOG_PATH"
else
    fail "launcher $L missing"
fi

# 5. SDL2 by leaf name, the way DXVK opens it.
DXVK=$C/Frameworks/libdxvk_d3d9.0.dylib
$OTOOL -l "$DXVK" | awk '$1 == "path" {print $2}' | grep -qx '@loader_path' ||
    fail "libdxvk_d3d9 has no @loader_path rpath"
T=$(mktemp -d "${TMPDIR:-/tmp}/check_app.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/x.app/Contents/MacOS"
ditto "$C/Frameworks" "$T/x.app/Contents/Frameworks"
cat > "$T/probe.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int probe(void) {
    Dl_info info;
    void *h = dlopen("libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    if (!dladdr(dlsym(h, "SDL_Init"), &info)) { printf("dladdr failed\n"); return 1; }
    printf("%s\n", info.dli_fname);
    return 0;
}
EOF
printf 'int probe(void);\nint main(void) { return probe(); }\n' > "$T/main.c"
FWT=$T/x.app/Contents/Frameworks
clang -arch arm64 -dynamiclib -install_name @rpath/libprobe.dylib -Wl,-rpath,@loader_path \
    -o "$FWT/libprobe.dylib" "$T/probe.c"
clang -arch arm64 -o "$T/x.app/Contents/MacOS/probe" "$T/main.c" -L"$FWT" -lprobe \
    -Wl,-rpath,@executable_path/../Frameworks
got=$(cd / && env -i HOME="$HOME" PATH=/usr/bin:/bin "$T/x.app/Contents/MacOS/probe") || true
case "$got" in
    "$FWT/libSDL2-2.0.0.dylib"|"$(realpath "$FWT")/libSDL2-2.0.0.dylib") ok "SDL2 by leaf name -> bundled copy" ;;
    *) fail "SDL2 by leaf name -> '$got'" ;;
esac

[ "$FAILS" -eq 0 ] && echo "check_app: all checks passed" && exit 0
echo "check_app: $FAILS failed"
exit 1
