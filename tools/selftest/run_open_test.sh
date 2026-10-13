#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_open_test.sh — where does clicking a search result go?
#
# The user report: picking a sermon in the app-wide search "takes time to take me
# to open The Table". This drives that exact path with the self-test harness —
# open the dialog, type a sermon citation, pick the first result — and prints the
# measured cost of each step (dialog open, engine query, the tab/pane switch, the
# goTo, and the pane's chapter read), so a sluggish pick is measured, not guessed.
#
# The numbers come from two sources:
#   * [SELFTEST-OPEN] lines — the scenario's own console.log (build/open_test.log)
#   * [OpenTrace] lines     — EngineBridge.log from the production path
#                             (build/logs/engine.log)
#
# VERDICT (last line, greppable):
#   OPEN: PASS sync=<ms>            — the pick completed and reported
#   OPEN: FAIL launch               — the app never came up
#   OPEN: FAIL missing              — the scenario never reported
#
# Usage:  bash tools/selftest/run_open_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/open_test.log"
ELOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-70}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "OPEN: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_OPEN_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_OPEN_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself once it has read the pane back.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-OPEN\] state" "$LOG" 2>/dev/null; then
        sleep 1
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
grep -a "\[SELFTEST-OPEN\]" "$LOG" 2>/dev/null
echo "== the production path's own trace =="
grep -a "\[OpenTrace\]" "$ELOG" 2>/dev/null | tail -4

STATE="$(grep -a "\[SELFTEST-OPEN\] state" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$STATE" ]; then
    echo "(no SELFTEST-OPEN state line — the scenario never finished; see $LOG)"
    echo "OPEN: FAIL missing"
    exit 3
fi

SYNC="$(grep -a "\[OpenTrace\] openSearchResult" "$ELOG" 2>/dev/null | tail -1 \
        | sed -n 's/.*took \([0-9]*\)ms.*/\1/p')"
TOTAL="$(echo "$STATE" | sed -n 's/.*total=\([0-9]*\)ms.*/\1/p')"
echo "OPEN: PASS sync=${SYNC:-?}ms total=${TOTAL:-?}ms"
exit 0
