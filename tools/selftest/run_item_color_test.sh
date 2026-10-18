#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_item_color_test.sh — does a colour applied in the picker
# reach the item ON THE CANVAS, for a shape as well as for text?
#
# The report this exists for: "the opacity in the colour picker shows well but
# it never applies to the shape in the edit canvas — the text item does apply
# it". Both kinds go through the SAME BackgroundColorModal, so the difference is
# either in the apply or in what the canvas reads back, and reading the code
# cannot tell the two apart. This scenario measures it: it adds a shape and a
# text item, opens the picker the row's own "Change" chip opens, sets Opacity to
# 30%, presses Save (the same accept() the footer button calls) and prints:
#
#   * what the ITEM now holds (style.backgroundColor / meta.color), and
#   * what the ITEM'S DELEGATE is drawing (DraggableCanvasText's own
#     styleBackgroundColor) — the delegate is the thing the operator sees, and
#     "the model changed, the canvas did not" is exactly the bug class here.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_ITEMCOLOR_TEST=1
#      (stdout+stderr -> build/item_color_test.log)
#   2. the Main.qml scenario (SelfTestItemColor) drives the real path — a new
#      show, a canvas item of each kind, the picker opened through
#      EditScreen.openBackgroundPicker (the handler the Change chip calls), an
#      opacity, Save — then reads both items back and quits itself
#   3. the [SELFTEST-ITEMCOLOR] lines are printed, then the verdict
#
# VERDICT (last line, greppable):
#   ITEMCOLOR: APPLIED      — the shape's fill faded on the ITEM and on its
#                             DELEGATE (and the text item's colour obeyed the
#                             same picker in the same run)
#   ITEMCOLOR: FAIL missing — the scenario never reported (the driver never
#                             armed; see the log)
#   ITEMCOLOR: FAIL item    — the item itself never got the faded colour: the
#                             apply is dropping it
#   ITEMCOLOR: FAIL canvas  — the ITEM holds the faded colour and the DELEGATE
#                             still draws the old one: the canvas is not being
#                             told (the reported "it never applies to the shape")
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
WAIT_SECS="${1:-90}"
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

rm -f "$LOG"
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
        sleep 2
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
if printf '%s' "$VERDICT" | grep -qa "verdict=APPLIED"; then
    echo "ITEMCOLOR: APPLIED"
    exit 0
fi
SUMMARY="$(printf '%s\n' "$LINES" | grep -a "summary shapeItemFaded=" | tail -1)"
if [ -z "$VERDICT" ] || [ -z "$SUMMARY" ]; then
    # Armed, but a step never reported (an exception in the scenario, or the app
    # died mid-run). Saying "canvas" here would blame the wrong half.
    echo "ITEMCOLOR: FAIL missing — the scenario armed but never reached its verdict (see $LOG)"
    exit 3
fi
if printf '%s' "$SUMMARY" | grep -qa "shapeItemFaded=false"; then
    echo "ITEMCOLOR: FAIL item — the item never received the faded colour (the apply drops it)"
    exit 4
fi
echo "ITEMCOLOR: FAIL canvas — the item faded, the canvas delegate did not"
exit 5
