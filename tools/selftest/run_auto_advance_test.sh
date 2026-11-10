#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_auto_advance_test.sh — does the WALL'S TIMER move the slide?
#
# The report: "the timer isn't moving the slide". The timer is one shot per slide,
# armed with that slide's own NEXT TIMER, and the step it takes is the SAME one the
# Next button and its hotkeys take — a line in Scroll mode, the next slide in Slide.
# It is recorded against the SHOW, not against the slide title it was armed on (that
# comparison went false on the first step and disarmed it after one slide).
#
# WHAT IT DOES
#   1. launches with VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 (log -> build/auto_advance_test.log)
#   2. the scenario (LiveOutputService::runAutoAdvanceSelfTestOnce) builds a 3-slide
#      show with a 2 s NEXT TIMER on each slide, goes live through the loaded-show
#      path, and presses the wall's timer through the same entry point the button uses
#   3. every step logs "LiveOutputService: auto-advance step index=N title='…'"
#
# VERDICT (last line, greppable):
#   AUTOADVANCE: PASS steps=N indexes=0>1>2
#   AUTOADVANCE: FAIL notrun  — the scenario never armed (see build/auto_advance_test.log)
#   AUTOADVANCE: FAIL noarm   — the timer was pressed but is not advancing
#   AUTOADVANCE: FAIL stuck   — fewer than 2 steps in the window: it did not move the slide
#   AUTOADVANCE: FAIL order   — steps happened but the on-air index did not advance in order
#
# Usage:  bash tools/selftest/run_auto_advance_test.sh [wait-seconds]   (default 20)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/auto_advance_test.log"
WAIT_SECS="${1:-20}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "AUTOADVANCE: FAIL notrun — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 \
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
grep -a "\[SELFTEST-AUTOADVANCE\]" "$LOG" 2>/dev/null
echo "== steps =="
grep -a "auto-advance" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "RangeError|Cannot read|TypeError|qml:[0-9]+:" "$LOG" 2>/dev/null | head -8

ARMED="$(grep -a "\[SELFTEST-AUTOADVANCE\] armed" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$ARMED" ]; then
    echo "(no armed line — the scenario never reached the timer; see $LOG)"
    echo "AUTOADVANCE: FAIL notrun"
    exit 3
fi
case "$ARMED" in
    *"advancing=1"*) ;;
    *)
        echo "AUTOADVANCE: FAIL noarm — the timer was pressed but is not advancing: $ARMED"
        exit 4
        ;;
esac

STEPS="$(grep -a -c "auto-advance step" "$LOG" 2>/dev/null)"
STEPS="${STEPS:-0}"
INDEXES="$(grep -a "auto-advance step" "$LOG" 2>/dev/null \
    | sed -n 's/.*index=\([0-9]*\).*/\1/p' | paste -sd'>' -)"

if [ "$STEPS" -lt 2 ]; then
    echo "AUTOADVANCE: FAIL stuck — ${STEPS} step(s) in ${WAIT_SECS}s: the timer did not move the slide (indexes='${INDEXES}')"
    exit 5
fi

# In order: each logged index is one more than the one before it.
ORDER="$(grep -a "auto-advance step" "$LOG" 2>/dev/null \
    | sed -n 's/.*index=\([0-9]*\).*/\1/p' \
    | awk 'NR==1 { prev=$1; next } { if ($1 != prev + 1) bad=1; prev=$1 } END { print bad ? "no" : "yes" }')"
if [ "$ORDER" != "yes" ]; then
    echo "AUTOADVANCE: FAIL order — the steps did not advance the on-air index one at a time (indexes='${INDEXES}')"
    exit 6
fi

echo "AUTOADVANCE: PASS steps=$STEPS indexes=$INDEXES"
exit 0
