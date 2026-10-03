#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_insets_test.sh — catch Settings-card layout drift.
#
# The automated guard for "every card on Settings > General shares the same
# 20px internal padding". It renders the screen for real (VGR_SELFTEST=1 arms
# the scenario Timers in qml/Main.qml), grabs each card to a PNG, and measures
# the rendered insets with tools/selftest/measure_insets.py.
#
# WHAT IT DOES
#   1. removes any stale insets_*.png from the build dir
#   2. launches the app with VGR_SELFTEST=1 (stderr -> build/insets_test.log)
#   3. the scenario opens Settings > General, grabs each card, and quits
#   4. runs the checker over the grabbed cards
#
# VERDICT (last line, greppable):
#   INSETS_TEST: PASS          — every card's left/top/right inset agrees
#   INSETS_TEST: FAIL drift    — a card drifted from the others (see checker)
#   INSETS_TEST: FAIL missing  — the scenario never produced screenshots
#
# Usage:  bash tools/selftest/run_insets_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/insets_test.log"
WAIT_SECS="${1:-45}"

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "INSETS_TEST: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== clearing stale screenshots =="
rm -f "$BUILD"/insets_*.png

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 (stderr -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_INSETS_TEST=1 \
        PATH="C:/msys64/ucrt64/bin:C:/Qt/6.11.1/mingw_64/bin:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-INSETS\] done" "$LOG" 2>/dev/null; then
        sleep 2   # let the last grab's saveToFile() flush
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
grep -a "SELFTEST-INSETS" "$LOG" 2>/dev/null || echo "(no SELFTEST-INSETS lines — the driver never armed)"

SHOTS=( "$BUILD"/insets_*.png )
if [ ! -e "${SHOTS[0]}" ]; then
    echo "INSETS_TEST: FAIL missing — no screenshots produced (see $LOG)"
    exit 3
fi

echo "== measuring $((${#SHOTS[@]})) card(s) =="
py "$HERE/measure_insets.py" "${SHOTS[@]}"
CHECK=$?

if [ "$CHECK" -eq 0 ]; then
    echo "INSETS_TEST: PASS"
    exit 0
fi
echo "INSETS_TEST: FAIL drift"
exit 4
