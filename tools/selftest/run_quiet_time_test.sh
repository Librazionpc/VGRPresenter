#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_quiet_time_test.sh — does a Quiet Time pick SHOW ON THE
# OUTPUTS once GO LIVE takes it?
#
# The report this answers: "go live is still clearing it" — the Quiet Time slide
# was on the wall, and the Lower Third tile went blank on the press. Two separate
# things can be wrong there and they need different fixes:
#   * the PICTURE an output composes (a category template that does not resolve
#     for that output's style), and
#   * the PIXELS the app actually drew on that output's tile.
# A composed map can be right while the tile is empty (a tile draws "staged, else
# on-air", and hands the slide to the engine's frame when the design needs one),
# so this measures both — and the pixels come from the APP'S OWN GRAB,
# SelfTest.grab, the same one run_scroll_frames.sh photographs tiles with.
#
# WHAT IT DOES
#   1. launches with VGR_SELFTEST=1 VGR_QUIETTIME_TEST=1 (log -> build/quiet_time_test.log)
#   2. the engine-side scenario (LiveOutputService::runQuietTimeSelfTestOnce)
#      stages a Quiet Time pick — title 'Verse 1', reference line, showCategory
#      'Quiet Time' — and presses GO LIVE through the same entry point the button
#      uses, then asks EVERY enabled output for the picture it would draw, both
#      from the pick and from the ON-AIR copy a tile falls back to after the press
#   3. the QML side (Main.qml, SelfTestQuietTime) photographs each monitor tile
#      with SelfTest.grab -> build/qt_tile_<i>.png and prints that tile's facts
#      (slide/blocks/engineComposed/framePriority/frameClear/cleared/enabled)
#   4. quiet_time_pixels.mjs counts the bright (text) pixels in each picture
#
# VERDICT (last line, greppable):
#   QUIETTIME: PASS outputs=N shown=N wall=N tiles=N bright=N
#   QUIETTIME: FAIL notrun      — the scenario produced no verdict line
#   QUIETTIME: FAIL notlive     — GO LIVE never reached the engine
#   QUIETTIME: FAIL category    — the on-air copy lost showCategory
#   QUIETTIME: FAIL wall        — an output composes an EMPTY picture from the
#                                 on-air copy while the pick composes fine
#   QUIETTIME: FAIL blank       — an output composes nothing even for the pick
#   QUIETTIME: FAIL title       — an output keeps blocks but loses title/design
#   QUIETTIME: FAIL tile        — the wall's own PIXELS for an ENABLED output are
#                                 blank, or its picture is missing (the QTPIX:
#                                 line beside it says which)
#
# Usage:  bash tools/selftest/run_quiet_time_test.sh [wait-seconds]   (default 90)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/quiet_time_test.log"
WAIT_SECS="${1:-90}"
ANALYZER="$HERE/quiet_time_pixels.mjs"
MIN_BRIGHT=40

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "QUIETTIME: FAIL notrun — $BUILD/$APP not found (build first)"
    exit 2
fi
if [ ! -f "$ANALYZER" ]; then
    echo "QUIETTIME: FAIL notrun — $ANALYZER is missing"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG" "$BUILD"/qt_tile_*.png
echo "== launching with VGR_SELFTEST=1 VGR_QUIETTIME_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_QUIETTIME_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    kill -0 "$RUN_PID" 2>/dev/null || break     # the scenario quits itself once it has grabbed
    sleep 1
    TICK=$((TICK + 1))
done
kill "$RUN_PID" 2>/dev/null
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-QUIETTIME\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "RangeError|Cannot read|TypeError|qml:[0-9]+:" "$LOG" 2>/dev/null | head -8

VERDICT_LINE="$(grep -a "\[SELFTEST-QUIETTIME\] verdict" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$VERDICT_LINE" ]; then
    echo "(no verdict line — the scenario never reached its checks; see $LOG)"
    echo "QUIETTIME: FAIL notrun"
    exit 3
fi

field() {
    printf '%s\n' "$VERDICT_LINE" | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2
}
OUTPUTS="$(field outputs)"
SHOWN="$(field shown)"
BLANK="$(field blank)"
DRAINED="$(field drained)"
WALLBAD="$(field wallBad)"
LIVE="$(field live)"
CATEGORY="$(printf '%s\n' "$VERDICT_LINE" | sed -n "s/.*onAirCategory='\(.*\)'$/\1/p")"

if [ -z "${OUTPUTS:-}" ] || [ "$OUTPUTS" -lt 1 ]; then
    echo "QUIETTIME: FAIL notrun — the wall listed no enabled output: $VERDICT_LINE"
    exit 4
fi
if [ "${LIVE:-0}" != "1" ]; then
    echo "QUIETTIME: FAIL notlive — GO LIVE never reached the engine, so no output had the pick: $VERDICT_LINE"
    exit 5
fi
if [ -n "$CATEGORY" ] && [ "$CATEGORY" != "Quiet Time" ]; then
    echo "QUIETTIME: FAIL category — the on-air copy carries '$CATEGORY', not 'Quiet Time': $VERDICT_LINE"
    exit 6
fi

# --- THE WALL'S OWN PIXELS (the app's grab, not a receiver's) ----------------
# Judged per ENABLED output: a REC/STREAM row has no picture of its own, so its
# tile is photographed and reported but cannot fail the run.
echo "== the wall's own tile pictures (enabled outputs must carry the design) =="
TILES=0
TILE_BRIGHT=0
ENABLED_SEEN=0
TILE_BAD=""
while IFS= read -r gline; do
    [ -n "$gline" ] || continue
    idx="$(printf '%s\n' "$gline" | sed -n 's/.*tile=\([0-9]*\).*/\1/p')"
    enabled="$(printf '%s\n' "$gline" | grep -o 'enabled=[a-z]*' | cut -d= -f2)"
    png="$BUILD/qt_tile_${idx}.png"
    if [ ! -f "$png" ]; then
        [ "$enabled" = "true" ] && { [ -n "$TILE_BAD" ] || TILE_BAD="missing picture for enabled tile $idx"; }
        continue
    fi
    TILES=$((TILES + 1))
    line="$(node "$ANALYZER" "$png" --min "$MIN_BRIGHT" 2>&1 | tail -1)"
    echo "  $line  (enabled=$enabled)"
    [ "$enabled" = "true" ] || continue
    ENABLED_SEEN=$((ENABLED_SEEN + 1))
    case "$line" in
        "QTPIX: PASS"*)
            TILE_BRIGHT=$((TILE_BRIGHT + $(printf '%s\n' "$line" | sed -n 's/.*bright=\([0-9]*\).*/\1/p')))
            ;;
        *)
            [ -n "$TILE_BAD" ] || TILE_BAD="$line"
            ;;
    esac
done <<< "$(grep -a '\[SELFTEST-QUIETTIME\] grabbed' "$LOG" 2>/dev/null)"

if [ "$TILES" -eq 0 ]; then
    echo "QUIETTIME: FAIL tile — the wall was never photographed (no build/qt_tile_*.png; see the grabbed lines above)"
    exit 13
fi
if [ "$ENABLED_SEEN" -ne "$OUTPUTS" ]; then
    echo "QUIETTIME: FAIL tile — photographed ${ENABLED_SEEN} enabled tile(s), the engine counted $OUTPUTS enabled output(s)"
    exit 14
fi
if [ -n "$TILE_BAD" ]; then
    echo "QUIETTIME: FAIL tile — the wall did not draw the design on an enabled output: $TILE_BAD"
    exit 15
fi

if [ "${WALLBAD:-0}" != "0" ]; then
    echo "QUIETTIME: FAIL wall — ${WALLBAD} of $OUTPUTS output(s) compose an EMPTY picture from the on-air copy while the pick composes fine: $VERDICT_LINE"
    exit 12
fi
if [ "${BLANK:-0}" != "0" ]; then
    echo "QUIETTIME: FAIL blank — ${BLANK} of $OUTPUTS output(s) compose nothing for the pick: $VERDICT_LINE"
    exit 7
fi
if [ "${SHOWN:-0}" != "$OUTPUTS" ]; then
    echo "QUIETTIME: FAIL title — only ${SHOWN:-0} of $OUTPUTS output(s) drew the pick (${DRAINED:-0} kept blocks but lost the title/design): $VERDICT_LINE"
    exit 8
fi

echo "QUIETTIME: PASS outputs=$OUTPUTS shown=$SHOWN wall=$((OUTPUTS - ${WALLBAD:-0})) tiles=$TILES bright=$TILE_BRIGHT"
exit 0
