#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_show_timer_test.sh — does a SHOW advance by its own timer?
#
# The reports: "the timer icon is not responding", "the slides did not move",
# and "loading a show from the list stalls".
#
# WHAT IT DOES (qml/Main.qml, VGR_SHOWTIMER_TEST)
#   1. opens the named show through the same path a list click takes (centerItem)
#   2. logs how long the show took to load (loadMs)
#   3. writes a 2 s NEXT TIMER to every slide, re-reads it
#   4. stageSlideSet + goLive, presses the wall timer (logs takeMs for the stall)
#   5. samples onAirIndex for 10 s, grabs showtimer_center_*.png / showtimer_wall_*.png
#
# VERDICT (last line, greppable):
#   SHOWTIMER: PASS advances=N firstAdvanceMs=… loadMs=… takeMs=…
#   SHOWTIMER: FAIL noshow   — the show was not found in the library
#   SHOWTIMER: FAIL notimer  — the re-read show carried no NEXT TIMER
#   SHOWTIMER: FAIL stuck    — the timer was pressed but no slide moved
#   SHOWTIMER: FAIL notrun   — the scenario never produced a result line
#
# Usage:  bash tools/selftest/run_show_timer_test.sh ["Show name"] [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/show_timer_test.log"
SHOW_NAME="${1:-25-0308}"
WAIT_SECS="${2:-45}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "SHOWTIMER: FAIL notrun — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_SHOWTIMER_TEST=1 name='$SHOW_NAME' (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SHOWTIMER_TEST=1 VGR_SHOWTIMER_NAME="$SHOW_NAME" \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    kill -0 "$RUN_PID" 2>/dev/null || break
    sleep 1
    TICK=$((TICK + 1))
done
kill "$RUN_PID" 2>/dev/null
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-SHOWTIMER\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "RangeError|Cannot read|TypeError|qml:[0-9]+:" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-SHOWTIMER\] result=" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no result line — the scenario did not finish; see $LOG)"
    echo "SHOWTIMER: FAIL notrun"
    exit 3
fi
if echo "$RESULT" | grep -q "FAIL"; then
    if grep -aq "FAIL noshow" "$LOG"; then
        echo "SHOWTIMER: FAIL noshow — '$SHOW_NAME' was not in the library"
        exit 4
    fi
    if grep -aq "anyTimer=false" "$LOG"; then
        echo "SHOWTIMER: FAIL notimer — the re-read show carried no NEXT TIMER"
        exit 5
    fi
    echo "SHOWTIMER: FAIL stuck — the timer was pressed but no slide moved: $RESULT"
    exit 6
fi
echo "SHOWTIMER: PASS ${RESULT#*result=PASS }"
exit 0
