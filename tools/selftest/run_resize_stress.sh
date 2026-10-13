#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_resize_stress.sh — does the app survive being resized hard?
#
# The regression guard for "the app always crashes when I resize it repeatedly".
# A hand drag can only produce so many WM_SIZE messages a second, and the crash
# needs repetition, so this drives the window's REAL native resize path: it finds
# the app's HWND and calls MoveWindow a few hundred times back to back,
# alternating small and large geometries so both the minimum and the maximum size
# clamps are exercised on every pass. Nothing here touches QML directly — if the
# process is alive at the end, the same code path a user drags survived.
#
# WHY A NATIVE DRIVE AND NOT A QML SCENARIO: WindowResizeHandles.qml resizes via
# QWindow::startSystemResize, so the geometry arrives from the OS (WM_SIZE). A
# QML `window.width = n` loop exercises the same layout re-derivation but not the
# platform handshake, which is where a resize crash usually lives.
#
# VERDICT (last line, greppable):
#   RESIZE_STRESS: PASS            — the process survived every resize
#   RESIZE_STRESS: FAIL crashed    — it died mid-sweep (see the iteration number)
#   RESIZE_STRESS: FAIL launch     — it never came up (see build/resize_stress.log)
#
# Usage:  bash tools/selftest/run_resize_stress.sh [iterations] [sleep-ms]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/resize_stress.log"
ITERATIONS="${1:-400}"
SLEEP_MS="${2:-10}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "RESIZE_STRESS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching (stdout+stderr -> $LOG) =="
(
    cd "$BUILD" || exit 1
    PATH="$MINGW_BIN:$QT_BIN:$PATH" "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# Give the engine + QML a moment to finish booting: resizing a half-built scene
# tests something the user never does.
BOOT_WAIT=0
while [ "$BOOT_WAIT" -lt 25 ]; do
    if grep -qa "QML debugging\|\[Layout\]\|Kernel ready" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        echo "RESIZE_STRESS: FAIL launch — the app exited during boot (see $LOG)"
        exit 2
    fi
    sleep 1
    BOOT_WAIT=$((BOOT_WAIT + 1))
done
sleep 4

echo "== driving $ITERATIONS native resizes =="
# RESIZE_FIXED_W/H hold the geometry constant, which separates "this size kills
# it" from "resizing repeatedly kills it" when a crash needs diagnosing.
FIXED_ARGS=""
if [ -n "${RESIZE_FIXED_W:-}" ] && [ -n "${RESIZE_FIXED_H:-}" ]; then
    FIXED_ARGS="-FixedW $RESIZE_FIXED_W -FixedH $RESIZE_FIXED_H"
    echo "   (fixed geometry ${RESIZE_FIXED_W}x${RESIZE_FIXED_H})"
fi
powershell -NoProfile -ExecutionPolicy Bypass -File "$BUILD/_resize_stress.ps1" \
    -Iterations "$ITERATIONS" -SleepMs "$SLEEP_MS" $FIXED_ARGS
STRESS_RC=$?
echo "stress exit=$STRESS_RC"

if kill -0 "$RUN_PID" 2>/dev/null; then
    echo "== still running after the sweep =="
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
else
    echo "== the app is GONE (it crashed or exited on its own) =="
    echo "-- last lines of the app log --"
    tail -20 "$LOG" 2>/dev/null
fi
wait "$RUN_PID" 2>/dev/null

if [ "$STRESS_RC" -eq 0 ]; then
    echo "RESIZE_STRESS: PASS"
    exit 0
fi
echo "RESIZE_STRESS: FAIL crashed"
exit 1
