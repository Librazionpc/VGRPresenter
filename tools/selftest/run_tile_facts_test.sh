#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_tile_facts_test.sh — what is EACH monitor-wall tile
# actually drawing, for the same on-air slide?
#
# The report this exists for: "the same slide looks different on the two
# monitor tiles". The tile is the one surface an operator has for comparing
# outputs, so a difference there is either (a) two outputs that really differ,
# (b) one surface deriving a rule for itself, or (c) a stale composition — and
# from the picture alone those are indistinguishable. This scenario prints the
# facts each tile reads and grabs each tile's pixels, so the answer is measured
# rather than guessed.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_TILEFACTS_TEST=1
#      (stdout+stderr -> build/tile_facts_test.log)
#   2. for EVERY tile on the SHOW wall, in three states — nothing taken, a
#      still taken off air, and live — prints:
#        * the output's own row facts (style id, Fill) and the tile's derived
#          ones (effectiveFit/fillMode, hasFrame, framePriority, hasSlidePreview)
#        * the COMPOSITION the tile is drawing from: valid, styleColor,
#          background, stylePaintless, styleImagePainted, explicitTransparent,
#          frameTransparent, clearedByTake, block count
#        * which of the pane's layers is visible (pane colour, checkers,
#          design blocks, taken-media image, distributed frame, taken input)
#      and then the SAME composition composed FRESH at that instant, beside it.
#      A tile whose cached answer disagrees with the fresh one is drawing a
#      composition that has not seen what changed (the take is not a QML
#      dependency of the tile's binding).
#   3. grabs each tile to build/tilefacts_<phase>_<n>.png — the picture, not
#      just properties
#
# No verdict on purpose: this is the diagnostic that answers "why do they
# differ", and the answer is a comparison of two tiles, not a pass/fail of one
# rule. It DOES fail when the scenario produced no lines (the driver never
# armed / a phase never ran), because a silent run reads like an answer.
#
# Usage:  bash tools/selftest/run_tile_facts_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/tile_facts_test.log"
WAIT_SECS="${1:-60}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "TILE_FACTS: FAIL missing-app — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$BUILD"/tilefacts_*.png "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_TILEFACTS_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_TILEFACTS_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits itself after its last report; poll for that rather than
# always sleeping the full budget. The kill at the end is the backstop for a
# scenario that armed but never reached its final timer.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-TILES\] phase=live tile=0 fresh" "$LOG" 2>/dev/null; then
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

echo "== tile facts =="
LINES="$(grep -a "\[SELFTEST-TILES\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-TILES lines — the driver never armed; see $LOG)"
    echo "TILE_FACTS: FAIL missing"
    exit 3
fi
printf '%s\n' "$LINES"

echo "== screenshots =="
for png in "$BUILD"/tilefacts_*.png; do
    [ -f "$png" ] && echo "  ${png#"$BUILD"/}"
done

echo "TILE_FACTS: REPORTED"
