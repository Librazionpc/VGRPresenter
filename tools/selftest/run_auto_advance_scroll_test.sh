#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_auto_advance_scroll_test.sh — in SCROLL mode, does the
# wall's timer walk ONE LINE at a time?
#
# The timer takes the same step the Next button and its hotkeys take: in Scroll
# that is LiveOutputService.lineStep(1), one line, handing off at the page's end.
# This puts a passage ON AIR with Scroll switched on, presses the wall's timer,
# and judges each step by the pick's own absolute line (readingAbsoluteLine, which
# is window-independent and therefore comparable across a page turn):
#   * the step happened in Scroll (scroll=1) — a scroll=0 step is a slide change
#   * the position moved by exactly ONE line (0.99 .. 1.01), never zero, never two
#   * at least two lines were walked inside the window
#
# WHAT IT DOES
#   1. launches with VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 VGR_AUTOADVANCE_SCROLL=1
#      (log -> build/auto_advance_scroll_test.log)
#   2. the scenario (LiveOutputService::runAutoAdvanceSelfTestOnce, scroll branch)
#      switches Scroll on, stages a 12-page passage whose pages carry a 2 s NEXT
#      TIMER, goes live, and presses the wall's timer through the same entry point
#      the button uses
#   3. every step logs "auto-advance step ... scroll=1 lines=<delta> serial=<n>"
#
# VERDICT (last line, greppable):
#   AUTOSCROLL: PASS steps=N lines=<a,b,...>
#   AUTOSCROLL: FAIL notrun — the scenario never armed (see the log)
#   AUTOSCROLL: FAIL noarm  — the timer was pressed but is not advancing in Scroll
#   AUTOSCROLL: FAIL stuck  — fewer than 2 steps in the window
#   AUTOSCROLL: FAIL slid   — a step ran in Slide mode (scroll=0): it changed slide
#   AUTOSCROLL: FAIL jump   — a step moved by something other than one line
#
# Usage:  bash tools/selftest/run_auto_advance_scroll_test.sh [wait-seconds]   (default 22)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/auto_advance_scroll_test.log"
WAIT_SECS="${1:-22}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "AUTOSCROLL: FAIL notrun — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 VGR_AUTOADVANCE_SCROLL=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_AUTOADVANCE_TEST=1 VGR_AUTOADVANCE_SCROLL=1 \
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
    echo "AUTOSCROLL: FAIL notrun"
    exit 3
fi
case "$ARMED" in
    *"scroll=1"*) ;;
    *)
        echo "AUTOSCROLL: FAIL noarm — the scenario did not reach Scroll mode: $ARMED"
        exit 4
        ;;
esac
case "$ARMED" in
    *"advancing=1"*) ;;
    *)
        echo "AUTOSCROLL: FAIL noarm — the timer was pressed but is not advancing: $ARMED"
        exit 5
        ;;
esac

STEPS="$(grep -a -c "auto-advance step" "$LOG" 2>/dev/null)"
STEPS="${STEPS:-0}"
if [ "$STEPS" -lt 2 ]; then
    echo "AUTOSCROLL: FAIL stuck — ${STEPS} step(s) in ${WAIT_SECS}s: the timer did not walk the passage"
    exit 6
fi

# per step: "<scroll> <lines>" — the mode it ran in, and how far the position moved.
ROWS="$(grep -a "auto-advance step" "$LOG" 2>/dev/null \
    | sed -n 's/.*scroll=\([0-9]*\).*lines=\([-0-9.]*\).*/\1 \2/p')"
if [ -z "$ROWS" ]; then
    echo "AUTOSCROLL: FAIL notrun — the step lines carry no scroll=/lines= fields (see the steps above)"
    exit 7
fi
SLID="$(printf '%s\n' "$ROWS" | awk '$1 != 1 { bad++ } END { print bad + 0 }')"
if [ "$SLID" -ne 0 ]; then
    echo "AUTOSCROLL: FAIL slid — ${SLID} step(s) ran in Slide mode (scroll=0): the timer changed slide, not line"
    exit 8
fi
# THE FIRST ROW IS THE BASELINE, not a measurement: the position at the FIRST step is where the pick
# started, so its delta vs. "nothing" is 0. Every step after it must have moved exactly one line (the
# walk glides up to 600 ms, so each delta is read settled, at the start of the next 2 s tick).
JUDGED="$(printf '%s\n' "$ROWS" | awk 'NR > 1 { n++ } END { print n + 0 }')"
if [ "$JUDGED" -lt 2 ]; then
    echo "AUTOSCROLL: FAIL stuck — only ${JUDGED} judged step(s): not enough ticks to measure a walk"
    exit 10
fi
JUMPED="$(printf '%s\n' "$ROWS" | awk 'NR > 1 && ($2 < 0.99 || $2 > 1.01) { bad++ } END { print bad + 0 }')"
DELTAS="$(printf '%s\n' "$ROWS" | awk '{ print $2 }' | paste -sd, -)"
if [ "$JUMPED" -ne 0 ]; then
    echo "AUTOSCROLL: FAIL jump — ${JUMPED} step(s) did not move exactly one line (lines=${DELTAS})"
    exit 9
fi

echo "AUTOSCROLL: PASS steps=$STEPS lines=$DELTAS"
exit 0
