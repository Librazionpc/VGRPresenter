#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_canvas_cursor_test.sh — are the adjustment points ON the
# canvas, and do they show the right cursor?
#
# The reports this exists for (user, with a screenshot of a text item framed to
# the canvas edge):
#
#   "the adjustment point of edit are not in the edit canvas"
#   "the cursor doesn't change when moving or resize but it does when in text
#    mode ... i meant arrow point beams that show when resize the item"
#   "for the app rescaling the cursor doesn't show as well"
#
# mCanvas clips (clip: true), and the resize dots straddle the item's edge by
# convention — half in, half out. On an item framed to the canvas edge that put
# half of every dot (and the whole 16px edge strip) outside the clipped area: the
# points were not drawn on the canvas at all, so there was nothing to hover and no
# size cursor to show. The cursor itself had a second problem: an item's content
# area (the I-beam) and the dot over it both push on every pointer move, so push
# order alone decided the winner and the I-beam won on every corner.
#
# This drives the REAL app: opens a show, adds a text item, frames it to the
# canvas edge, then walks the OS cursor onto every adjustment point with real
# cursor moves and reads what the cursor layer actually renders. Judged:
#   * every dot's rectangle lies inside the canvas rect (nothing clipped away),
#   * every dot claims ITS size arrow (corner = diagonal, edge = horizontal or
#     vertical), the body claims the I-beam, and a press claims the grab,
#   * and the whole sweep is repeated after the WINDOW is resized (every layer
#     re-scales with it).
#
# VERDICT (last line, greppable):
#   CANVAS: PASS interior=<verdict> | rescale=<verdict>
#   CANVAS: FAIL launch | FAIL missing | FAIL dots=<inside>/<n>
#           | FAIL shapes | FAIL rescale
#
# Usage:  bash tools/selftest/run_canvas_cursor_test.sh [wait-seconds]  (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/canvas_cursor_test.log"
WAIT_SECS="${1:-120}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "CANVAS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_CANVAS_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_CANVAS_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-CANVAS\] result" "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
# Bounded wait: quit() tears the window down while Qt's DXGI vsync service and
# the Win32 event dispatcher are still shutting down, and that shutdown has
# been seen to take longer than the scenario itself — an unbounded `wait` then
# blocks the REPORTING half of this script forever, and the run looks like "no
# output at all" while the log holds a full verdict.
WAITED=0
while kill -0 "$RUN_PID" 2>/dev/null && [ "$WAITED" -lt 30 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
done
kill -0 "$RUN_PID" 2>/dev/null && powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
wait "$RUN_PID" 2>/dev/null

echo "== scenario (every adjustment point, then the same after a window resize) =="
grep -a "\[SELFTEST-CANVAS\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-CANVAS\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-CANVAS result line — the scenario never finished; see $LOG)"
    echo "CANVAS: FAIL missing"
    exit 3
fi

INTERIOR="$(echo "$RESULT" | sed -E 's/.*interior: ([^|]*)\|.*/\1/' | sed -E 's/ +$//')"
RESCALE="$(echo "$RESULT" | sed -E 's/.*rescale: //' | sed -E 's/ +$//')"
INSIDE="$(echo "$INTERIOR" | grep -oE 'dots=[0-9]+/[0-9]+' | cut -d= -f2)"
DOTS="$(echo "$INSIDE" | cut -d/ -f2)"
INSIDE_N="$(echo "$INSIDE" | cut -d/ -f1)"

[ -n "$DOTS" ] && [ "$DOTS" != "0" ] || { echo "CANVAS: FAIL missing — no adjustment points were found"; exit 4; }
[ "$INSIDE_N" = "$DOTS" ] || { echo "CANVAS: FAIL dots=${INSIDE_N}/${DOTS} — a dot is outside the canvas (clipped)"; exit 5; }
echo "$INTERIOR" | grep -qE 'shapes=[0-9]+/[0-9]+' || { echo "CANVAS: FAIL shapes — no cursor shapes were read ('$INTERIOR')"; exit 6; }
OK="$(echo "$INTERIOR" | grep -oE 'shapes=[0-9]+' | cut -d= -f2)"
TOTAL="$(echo "$INTERIOR" | grep -oE 'shapes=[0-9]+/[0-9]+' | cut -d/ -f2)"
[ "$OK" = "$TOTAL" ] || { echo "CANVAS: FAIL shapes=${OK}/${TOTAL} — a handle did not show its own cursor ('$INTERIOR')"; exit 7; }
echo "$INTERIOR" | grep -q 'body=ok' || { echo "CANVAS: FAIL shapes — the body did not show the text I-beam ('$INTERIOR')"; exit 8; }
echo "$INTERIOR" | grep -q 'press=ok' || { echo "CANVAS: FAIL shapes — a press did not take the grab cursor ('$INTERIOR')"; exit 9; }
echo "$RESCALE" | grep -qE 'dots=[0-9]+/[0-9]+' || { echo "CANVAS: FAIL rescale — the resized pass never reported ('$RESCALE')"; exit 10; }
R_INSIDE="$(echo "$RESCALE" | grep -oE 'dots=[0-9]+/[0-9]+' | cut -d= -f2)"
[ "$(echo "$R_INSIDE" | cut -d/ -f1)" = "$(echo "$R_INSIDE" | cut -d/ -f2)" ] \
    || { echo "CANVAS: FAIL rescale — a dot left the canvas after the window resize ('$RESCALE')"; exit 11; }
R_OK="$(echo "$RESCALE" | grep -oE 'shapes=[0-9]+' | cut -d= -f2)"
R_TOTAL="$(echo "$RESCALE" | grep -oE 'shapes=[0-9]+/[0-9]+' | cut -d/ -f2)"
[ "$R_OK" = "$R_TOTAL" ] || { echo "CANVAS: FAIL rescale — a cursor went missing after the window resize ('$RESCALE')"; exit 12; }

if [ -s "$BUILD/canvas_handles.png" ]; then
    echo "canvas screenshot: $BUILD/canvas_handles.png ($(stat -c %s "$BUILD/canvas_handles.png") bytes)"
else
    echo "canvas screenshot: MISSING"
fi

echo "CANVAS: PASS interior=$INTERIOR | rescale=$RESCALE"
exit 0
