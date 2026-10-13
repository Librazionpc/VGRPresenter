#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_cursor_scale_test.sh — is the cursor still SHOWN once the
# app is scaled?
#
# The report this exists for (user): "for the app rescaling the cursor doesnt
# show as well".
#
# Every layer of this app is drawn at its 1440x900 design size and scaled by
# window.uiScale (qml/Main.qml). The Main.qml scenario (SelfTestCursorScale)
# opens Settings > Styles, parks the REAL cursor on the "+ Add style" button
# (a plain-cursorShape MouseArea, the commonest control in the app) at
# uiScale 1 and again with the window 20% over the design size, and reports the
# shape the OS was showing at each park — `SelfTest.cursorAt()`, which moves the
# pointer and reads Win32 GetCursorInfo in the same tick. That in-process read
# is deliberate: a separate process polling the live cursor races the human at
# the keyboard (first cut of this script did exactly that and read the user's
# own pointer position, not ours).
#
# A pointing hand is expected at BOTH parks. If the hand only shows unscaled,
# per-MouseArea cursorShape is not surviving the scaled ancestor, and the
# app-wide cursor layer has to carry it (see AppCursorCatcher.qml).
#
# VERDICT (last line, greppable):
#   CURSORSCALE: PASS at1=HAND atScale=HAND
#   CURSORSCALE: FAIL launch | FAIL missing | FAIL scaled at1=.. atScale=..
#
# Usage:  bash tools/selftest/run_cursor_scale_test.sh [wait-seconds]  (default 150)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/cursor_scale_test.log"
WAIT_SECS="${1:-150}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "CURSORSCALE: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_CURSORSCALE_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_CURSORSCALE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-CURSORSCALE\] result" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-CURSORSCALE\]" "$LOG" 2>/dev/null
echo "== what the OS was showing, per park (SelfTest.cursorAt) =="
grep -a "SelfTestDriver: cursorAt" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -6

PARK_A="$(grep -a "\[SELFTEST-CURSORSCALE\] phase=A" "$LOG" 2>/dev/null | tail -1)"
PARK_B="$(grep -a "\[SELFTEST-CURSORSCALE\] phase=B" "$LOG" 2>/dev/null | tail -1)"
PARK_C="$(grep -a "\[SELFTEST-CURSORSCALE\] phase=C" "$LOG" 2>/dev/null | tail -1)"
AT1="$(echo "$PARK_A" | sed -nE 's/.*osCursor=([A-Za-z-]+).*/\1/p')"
ATSCALE="$(echo "$PARK_B" | sed -nE 's/.*osCursor=([A-Za-z-]+).*/\1/p')"
FRAME_EDGE="$(echo "$PARK_C" | sed -nE 's/.*edge@[0-9,]+=([A-Za-z-]+).*/\1/p')"
FRAME_CORNER="$(echo "$PARK_C" | sed -nE 's/.*corner@[0-9,]+=([A-Za-z-]+).*/\1/p')"
WINDOWED="$(echo "$PARK_C" | sed -nE 's/.*windowed=([A-Za-z-]+).*/\1/p')"
[ -n "$AT1" ] || AT1="not-read"
[ -n "$ATSCALE" ] || ATSCALE="not-read"
[ -n "$FRAME_EDGE" ] || FRAME_EDGE="not-read"
[ -n "$FRAME_CORNER" ] || FRAME_CORNER="not-read"

if [ -z "$PARK_A" ] || [ -z "$PARK_B" ] || [ -z "$PARK_C" ]; then
    echo "CURSORSCALE: FAIL missing — the scenario did not finish its parks (see $LOG)"
    exit 3
fi

# 1. The pointing hand on a plain-cursorShape button, at both scales.
if [ "$AT1" != "HAND" ] || [ "$ATSCALE" != "HAND" ]; then
    echo "CURSORSCALE: FAIL scaled at1=$AT1 atScale=$ATSCALE (the hand must show at both)"
    exit 4
fi
# 2. The window's own frame: a size arrow on the edge and on the corner. Only
#    meaningful while the window is resizable (an inert grip on a maximized
#    window legitimately shows nothing).
if [ "$WINDOWED" = "true" ] && { [ "$FRAME_EDGE" != "SIZEHOR" ] || [ "$FRAME_CORNER" != "SIZENWSE" ]; }; then
    echo "CURSORSCALE: FAIL frame edge=$FRAME_EDGE corner=$FRAME_CORNER (expected SIZEHOR / SIZENWSE to resize the app)"
    exit 5
fi
echo "CURSORSCALE: PASS at1=$AT1 atScale=$ATSCALE windowed=$WINDOWED frameEdge=$FRAME_EDGE frameCorner=$FRAME_CORNER"
exit 0
