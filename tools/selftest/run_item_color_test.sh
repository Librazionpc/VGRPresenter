#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_item_color_test.sh — does a colour/opacity picked in the
# colour picker reach the SHAPE on the canvas, the way it already does for a
# text item?
#
# THE REPORT THIS EXISTS FOR: "the opacity in the colour picker shows well but
# it never applies to the shape in the edit canvas — the text item does". Both
# kinds go through the SAME BackgroundColorModal, so the difference is either in
# the apply or in what the canvas reads back, and reading the code cannot tell
# the two apart. This scenario drives the real path and reports THREE witnesses
# per item:
#
#   * the ITEM's own value (style.backgroundColor / meta.color),
#   * its DELEGATE's own read (DraggableCanvasText.styleBackgroundColor),
#   * the PIXELS the delegate actually paints — each solid apply is grabbed to a
#     PNG and sampled here, because "the property is right, the screen is not"
#     is precisely the bug class every property trace has missed.
#
# Both shape render paths are measured: a RECTANGLE (the delegate's own fill
# box) and a CIRCLE (drawsOwnBody — EditScreen's shapeContent paints the body
# from the same style). The text item's colour runs in the same launch as the
# known-good comparison.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_ITEMCOLOR_TEST=1
#      (stdout+stderr -> build/item_color_test.log)
#   2. the Main.qml scenario (SelfTestItemColor) adds a rectangle shape, a
#      circle shape and a text item, opens the picker the row's own Change chip
#      opens (EditScreen.openBackgroundPicker), applies WHITE at 100% and then at
#      30%, reads every item + delegate back BY KEY, grabs each shape's delegate
#      to build/itemcolor_*_solid.png, and quits itself
#   3. the [SELFTEST-ITEMCOLOR] lines are printed, the two solid grabs are
#      sampled for the applied colour, then the verdict
#
# VERDICT (last line, greppable):
#   ITEMCOLOR: APPLIED     — both shapes hold the picked colour AND paint it,
#                            both fade to the opacity slider's alpha, and the
#                            text item obeyed the same picker in the same run
#   ITEMCOLOR: FAIL missing— the scenario never reported / never reached its
#                            verdict (see the log)
#   ITEMCOLOR: FAIL item   — an ITEM never received the colour: the apply drops
#                            it (a missing value counts as missing, never as a
#                            pass — the previous version of this scenario read a
#                            null item as alpha -1 and called that "faded")
#   ITEMCOLOR: FAIL canvas — the item holds the colour and the delegate's own
#                            read agrees, but the PIXELS are unchanged: the
#                            shape is not being painted
#   ITEMCOLOR: FAIL text   — the known-good control (a text item's colour)
#                            stopped working too, so this is not shape-specific
#
# Usage:  bash tools/selftest/run_item_color_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/item_color_test.log"
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
    echo "ITEMCOLOR: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG" "$BUILD"/itemcolor_*.png
echo "== launching with VGR_SELFTEST=1 VGR_ITEMCOLOR_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_ITEMCOLOR_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits itself after its last report; poll for that rather than
# always sleeping the whole budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-ITEMCOLOR\] verdict=" "$LOG" 2>/dev/null; then
        sleep 3
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
kill "$RUN_PID" 2>/dev/null
wait "$RUN_PID" 2>/dev/null

echo "== item colour =="
LINES="$(grep -a "\[SELFTEST-ITEMCOLOR\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-ITEMCOLOR lines — the driver never armed; see $LOG)"
    echo "ITEMCOLOR: FAIL missing"
    exit 3
fi
printf '%s\n' "$LINES"

VERDICT="$(printf '%s\n' "$LINES" | grep -a "verdict=" | tail -1)"
SUMMARY="$(printf '%s\n' "$LINES" | grep -a "summary rectPropsOk=" | tail -1)"
if [ -z "$VERDICT" ] || [ -z "$SUMMARY" ]; then
    echo "ITEMCOLOR: FAIL missing — the scenario armed but never reached its verdict (see $LOG)"
    exit 3
fi

# ---- the pixel half: does the SHAPE actually paint the picked colour? -------
# White at 100% is a colour no style in this app carries, so white pixels in the
# grab can only come from the apply. A missing grab is a failed measurement, not
# a pass.
sample() {
    local png="$1" min="$2"
    if [ ! -f "$png" ]; then
        echo "GOLDPIX: FAIL read — $png not written (the grab never landed)"
        return
    fi
    node "$HERE/gold_pixels.mjs" "$png" --color "#ffffff" --tol 24 --min "$min" 2>&1 | tail -1
}
RECT_PX="$(sample "$BUILD/itemcolor_rect_solid.png" 500)"
CIRCLE_PX="$(sample "$BUILD/itemcolor_circle_solid.png" 300)"
echo "== pixels =="
echo "rect   $RECT_PX"
echo "circle $CIRCLE_PX"

rect_paints() { printf '%s' "$1" | grep -qa "GOLDPIX: PASS"; }
says() { printf '%s' "$2" | grep -qa "$1"; }

# The text control: the same picker, the known-good item.
if ! says "text before=" "$LINES"; then
    echo "ITEMCOLOR: FAIL missing — the text control never reported"
    exit 3
fi
TEXT_LINE="$(printf '%s\n' "$LINES" | grep -a "text before=" | tail -1)"
if printf '%s' "$TEXT_LINE" | grep -qa "after=#ffffff"; then
    TEXT_OK=1
else
    TEXT_OK=0
fi

if ! says "cycleOk=true" "$SUMMARY"; then
    echo "ITEMCOLOR: FAIL select — a click on the shape still selects the text item above it"
    exit 7
fi
if ! says "rectPropsOk=true" "$SUMMARY" || ! says "circlePropsOk=true" "$SUMMARY"; then
    echo "ITEMCOLOR: FAIL item — the item never received the colour/opacity (the apply drops it)"
    exit 4
fi
if [ "$TEXT_OK" -ne 1 ]; then
    echo "ITEMCOLOR: FAIL text — the text item's own colour stopped working too (not shape-specific)"
    exit 5
fi
if ! rect_paints "$RECT_PX" || ! rect_paints "$CIRCLE_PX"; then
    echo "ITEMCOLOR: FAIL canvas — the item holds the colour and the delegate reads it, but the shape paints nothing"
    exit 6
fi
echo "ITEMCOLOR: APPLIED"
exit 0
