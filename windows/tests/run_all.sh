#!/bin/bash
# Build and run every Windows runtime test with its own flags; one PASS/FAIL
# line per test, then a summary. Exits non-zero on any failure.
# Usage: windows/tests/run_all.sh [--out DIR] [--no-gpu] [--only NAME] [--tests FILE] [--python PATH]
#   --out DIR      build and log dir (default: $TMPDIR/pe_run_all)
#   --no-gpu       skip the tests that need a GPU (d3dx_misc, d3d9_e2e)
#   --only NAME    run one test, for example win32_core or analyze_pe
#   --tests FILE   a game repo's test list (bash): it may define more def_<name>
#                  functions and add their names to C_TESTS, GPU_TESTS and
#                  PY_TESTS (windows/README.md, "Game tests")
#   --python PATH  Python with pytest and capstone (default: .venv of the game
#                  repo that holds this engine, else the engine's .venv)
# Warnings: the test's own files build with -Wall; the count is the warnings
# in windows/runtime/src files only (other warnings stay in the build log).
# A test binary that exits 77 counts as SKIP (game data it needs is absent).
# DXVK, vkd3d-shader and KosmicKrisp come from RECOMP_DEPS_DIR (HPRECOMP_DEPS_DIR
# is accepted too; default: build.nosync/deps of the game repo that holds this engine).
set -u

W=$(cd "$(dirname "$0")/.." && pwd)
ENGINE=$(cd "$W/.." && pwd)
OUT=${TMPDIR:-/tmp}/pe_run_all
GPU=1
ONLY=
TESTS_FILE=
PY=
while [ $# -gt 0 ]; do
    case $1 in
        --out) OUT=$2; shift 2 ;;
        --no-gpu) GPU=0; shift ;;
        --only) ONLY=$2; shift 2 ;;
        --tests) TESTS_FILE=$2; shift 2 ;;
        --python) PY=$2; shift 2 ;;
        *) echo "usage: $0 [--out DIR] [--no-gpu] [--only NAME] [--tests FILE] [--python PATH]" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)
if [ -n "$TESTS_FILE" ]; then
    TESTS_FILE=$(cd "$(dirname "$TESTS_FILE")" && pwd)/${TESTS_FILE##*/} || exit 2
    TESTS_DIR=$(dirname "$TESTS_FILE")
fi
cd "$W" || exit 2

if [ -z "$PY" ]; then
    PY=$ENGINE/../.venv/bin/python
    [ -x "$PY" ] || PY=$ENGINE/.venv/bin/python
fi
DEPS=${RECOMP_DEPS_DIR:-${HPRECOMP_DEPS_DIR:-$ENGINE/../build.nosync/deps}}
S=$ENGINE/runtime/src
R=runtime/src

COMMON=(-std=gnu17 -O1 -arch arm64 -Wall -DRECOMP_GLUE_NO_MAIN
    -I "$ENGINE/tools/xrecomp/include" -I "$ENGINE/runtime/include" -I runtime/include)
SDL_I=(-I /opt/homebrew/include -I /opt/homebrew/include/SDL2)
SDL_L=(-L/opt/homebrew/lib -lSDL2 -framework CoreGraphics -framework CoreFoundation -lobjc)
FAUDIO_I=(-I /opt/homebrew/opt/faudio/include)
FAUDIO_L=(-L/opt/homebrew/opt/faudio/lib -lFAudio)
VKD3D_I=(-I "$DEPS/vkd3d/include")
VKD3D_L=(-L"$DEPS/vkd3d/lib" -lvkd3d-shader -Wl,-rpath,"$DEPS/vkd3d/lib")
DXVK_I=(-I "$DEPS/dxvk/include/dxvk")
DXVK_L=(-L"$DEPS/dxvk/lib" -ldxvk_d3d9 -Wl,-rpath,"$DEPS/dxvk/lib")
GPU_ENV=(VK_DRIVER_FILES=$DEPS/kosmickrisp/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
    DXVK_WSI_DRIVER=SDL2 DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib DXVK_LOG_LEVEL=none)

# Each def_<name> sets FLAGS (compiler flags and sources), LIBS and ENVS.
def_win32_core() {
    FLAGS=("${COMMON[@]}" tests/test_win32_core.c $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_com_guest() {
    FLAGS=("${COMMON[@]}" tests/test_com_guest.c $R/com_guest.c $R/frame_trace.c $R/win32_com.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_frame_trace() {
    FLAGS=("${COMMON[@]}" tests/test_frame_trace.c $R/frame_trace.c
        $S/guest_mem.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_win32_mem() {
    FLAGS=("${COMMON[@]}" tests/test_win32_mem.c $R/win32_mem.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_win32_sync() {
    FLAGS=("${COMMON[@]}" tests/test_win32_sync.c $R/win32_sync.c
        $S/guest_mem.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c $S/guest_heap.c $S/dlmalloc.c)
    LIBS=(-lpthread); ENVS=()
}
def_win32_net() {
    FLAGS=("${COMMON[@]}" tests/test_win32_net.c $R/win32_net.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_win32_fs() {
    FLAGS=("${COMMON[@]}" tests/test_win32_fs.c $R/win32_fs.c $S/guest_mem.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_win32_ui() {
    FLAGS=("${COMMON[@]}" "${SDL_I[@]}" tests/test_win32_ui.c $R/win32_ui.c $S/hud_core.c
        $S/guest_mem.c $S/host_funcs.c $S/recomp_glue.c $S/guest_call.c)
    LIBS=("${SDL_L[@]}"); ENVS=(SDL_VIDEODRIVER=dummy RECOMP_NO_ALERT_UI=1)
}
def_win32_input() {
    FLAGS=("${COMMON[@]}" "${SDL_I[@]}" tests/test_win32_input.c $R/win32_input.c $R/input_script.c
        $S/guest_mem.c $S/host_funcs.c $S/guest_call.c $S/recomp_glue.c)
    LIBS=("${SDL_L[@]}"); ENVS=(SDL_VIDEODRIVER=dummy)
}
def_win32_winmm() {
    FLAGS=("${COMMON[@]}" tests/test_win32_winmm.c $R/win32_winmm.c $R/win32_sync.c
        $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(-lpthread); ENVS=()
}
def_win32_bink() {
    FLAGS=("${COMMON[@]}" tests/test_win32_bink.c $R/win32_bink.c $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/guest_call.c $S/host_funcs.c $S/recomp_glue.c)
    LIBS=(); ENVS=()
}
def_win32_audio() {
    FLAGS=("${COMMON[@]}" "${FAUDIO_I[@]}" tests/test_win32_audio.c $R/win32_audio.c
        $S/guest_mem.c $S/recomp_glue.c)
    LIBS=("${FAUDIO_L[@]}"); ENVS=()
}
def_win32_xaudio2() {
    FLAGS=("${COMMON[@]}" "${FAUDIO_I[@]}" tests/test_win32_xaudio2.c $R/win32_xaudio2.c
        $R/win32_sync.c $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c
        $S/guest_call.c $S/recomp_glue.c)
    LIBS=("${FAUDIO_L[@]}" -lpthread); ENVS=(SDL_AUDIO_DRIVER=dummy)
}
def_win32_dsound() {
    FLAGS=("${COMMON[@]}" "${FAUDIO_I[@]}" tests/test_win32_dsound.c $R/win32_dsound.c $R/com_guest.c
        $R/frame_trace.c $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/guest_call.c
        $S/recomp_glue.c)
    LIBS=("${FAUDIO_L[@]}" -lpthread); ENVS=(SDL_AUDIO_DRIVER=dummy)
}
def_d3d9_hud() {
    FLAGS=("${COMMON[@]}" "${DXVK_I[@]}" "${SDL_I[@]}" tests/test_d3d9_hud.c $R/d3d9_hud.c $S/hud_core.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/recomp_glue.c $S/guest_call.c)
    LIBS=("${SDL_L[@]}"); ENVS=()
}
def_d3d9_guest() {
    FLAGS=("${COMMON[@]}" "${DXVK_I[@]}" "${VKD3D_I[@]}" "${SDL_I[@]}" tests/test_d3d9_guest.c
        -DGAME_SCREEN_WIDTH_VA=0x00E00000u -DGAME_SCREEN_HEIGHT_VA=0x00E00004u
        $R/gen_d3d9_guest.c $R/d3d9_guest_manual.c $R/d3d9_hud.c $S/hud_core.c $R/d3dx_shader.c $R/d3dx_misc.c
        $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/recomp_glue.c $S/guest_call.c)
    LIBS=("${VKD3D_L[@]}" "${SDL_L[@]}"); ENVS=()
}
def_d3dx_shader() {
    FLAGS=("${COMMON[@]}" "${DXVK_I[@]}" "${VKD3D_I[@]}" tests/test_d3dx_shader.c $R/d3dx_shader.c
        $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/guest_call.c $S/recomp_glue.c)
    LIBS=("${VKD3D_L[@]}"); ENVS=()
}
def_d3dx_misc() {
    FLAGS=("${COMMON[@]}" "${DXVK_I[@]}" "${SDL_I[@]}" "${VKD3D_I[@]}" tests/test_d3dx_misc.c
        $R/d3dx_misc.c $R/d3dx_shader.c $R/gen_d3d9_guest.c $R/d3d9_guest_manual.c $R/d3d9_hud.c $S/hud_core.c $R/com_guest.c $R/frame_trace.c
        $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/recomp_glue.c $S/guest_call.c)
    LIBS=("${DXVK_L[@]}" "${SDL_L[@]}" "${VKD3D_L[@]}"); ENVS=("${GPU_ENV[@]}")
}
def_d3d9_e2e() {
    FLAGS=(-std=gnu11 -O1 -arch arm64 -Wall -fwrapv -fno-strict-aliasing -ffp-contract=off
        -DRECOMP_TSO=1 -DRECOMP_GLUE_NO_MAIN
        -I "$ENGINE/tools/xrecomp/include" -I "$ENGINE/runtime/include" -I runtime/include
        "${DXVK_I[@]}" "${SDL_I[@]}" "${VKD3D_I[@]}" tests/test_d3d9_e2e.c
        $R/win32_ui.c $R/gen_d3d9_guest.c $R/d3d9_guest_manual.c $R/d3d9_hud.c $S/hud_core.c $R/d3d9_backend.c $R/com_guest.c $R/frame_trace.c
        $R/d3dx_misc.c $R/d3dx_shader.c
        $R/win32_core.c $R/win32_unsupported.c
        $S/guest_mem.c $S/guest_heap.c $S/dlmalloc.c $S/host_funcs.c $S/recomp_glue.c $S/guest_call.c)
    LIBS=("${DXVK_L[@]}" "${SDL_L[@]}" "${VKD3D_L[@]}"); ENVS=("${GPU_ENV[@]}")
}

def_analyze_pe() {
    PYTEST=tests/test_analyze_pe.py
}

C_TESTS="frame_trace win32_core win32_mem win32_sync win32_fs win32_ui win32_input win32_net win32_winmm win32_bink win32_audio win32_xaudio2 win32_dsound com_guest d3d9_hud d3d9_guest d3dx_shader d3dx_misc d3d9_e2e"
GPU_TESTS=" d3dx_misc d3d9_e2e "
PY_TESTS="analyze_pe"
# shellcheck source=/dev/null
[ -n "$TESTS_FILE" ] && { . "$TESTS_FILE" || exit 2; }
ALL="$C_TESTS $PY_TESTS"
if [ -n "$ONLY" ]; then
    case " $ALL " in *" $ONLY "*) ;; *) echo "unknown test: $ONLY (one of: $ALL)" >&2; exit 2 ;; esac
fi

CRASH_DIR=$HOME/Library/Logs/DiagnosticReports
ls "$CRASH_DIR" 2>/dev/null | sort > "$OUT/crash_before.txt"

PASSED=0; FAILED=0; SKIPPED=0; FAILS=
echo "engine: $ENGINE"
echo "out:    $OUT"

record_fail() {  # name, message
    echo "FAIL $1: $2"
    FAILED=$((FAILED + 1)); FAILS="$FAILS $1"
}

run_logged() {  # name, log, command...
    local name=$1 log=$2 t0 t1 st
    shift 2
    t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    perl -e 'alarm 120; exec @ARGV or die "exec: $!\n"' "$@" > "$log" 2>&1
    st=$?
    t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    SECS=$(perl -e "printf '%.1f', $t1 - $t0")
    return $st
}

for name in $ALL; do
    [ -n "$ONLY" ] && [ "$name" != "$ONLY" ] && continue
    if [ "$GPU" = 0 ] && [[ "$GPU_TESTS" == *" $name "* ]]; then
        echo "SKIP $name (--no-gpu)"; SKIPPED=$((SKIPPED + 1)); continue
    fi
    build_log=$OUT/$name.build.log
    run_log=$OUT/$name.run.log
    PYTEST=
    "def_$name"
    if [ -n "$PYTEST" ]; then
        : > "$build_log"
        if run_logged "$name" "$run_log" "$PY" -m pytest -q -p no:cacheprovider "$PYTEST"; then
            echo "PASS $name (${SECS}s)"; PASSED=$((PASSED + 1))
        else
            record_fail "$name" "$(tail -n 1 "$run_log")"
        fi
        continue
    fi
    bin=$OUT/test_$name
    rm -f "$bin"
    if ! clang "${FLAGS[@]}" -o "$bin" ${LIBS[@]+"${LIBS[@]}"} > "$build_log" 2>&1; then
        record_fail "$name" "build: $(grep -m 1 -E 'error' "$build_log" || tail -n 1 "$build_log")"
        continue
    fi
    warn=$(grep -cE '^runtime/src/[^:]+:[0-9]+:[0-9]+: warning:' "$build_log")
    if run_logged "$name" "$run_log" env ${ENVS[@]+"${ENVS[@]}"} "$bin"; then
        echo "PASS $name (${SECS}s) warnings=$warn"; PASSED=$((PASSED + 1))
    else
        st=$?
        last=$(tail -n 1 "$run_log")
        if [ $st -eq 77 ]; then
            echo "SKIP $name: $last"; SKIPPED=$((SKIPPED + 1)); continue
        fi
        [ $st -eq 142 ] && last="timeout after 120 s; $last"
        record_fail "$name" "exit $st: $last"
    fi
done

ls "$CRASH_DIR" 2>/dev/null | sort > "$OUT/crash_after.txt"
NEW_CRASHES=$(comm -13 "$OUT/crash_before.txt" "$OUT/crash_after.txt")
NCRASH=0
[ -n "$NEW_CRASHES" ] && NCRASH=$(printf '%s\n' "$NEW_CRASHES" | wc -l | tr -d ' ')

echo "summary: $PASSED passed, $FAILED failed, $SKIPPED skipped, $NCRASH new crash reports"
[ -n "$FAILS" ] && echo "failed:$FAILS"
[ -n "$NEW_CRASHES" ] && printf '  crash: %s\n' $NEW_CRASHES
echo "logs: $OUT"
[ "$FAILED" -eq 0 ] && [ "$NCRASH" -eq 0 ]
