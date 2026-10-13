#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_pill_test.sh — does one L/R button close only its own side?
#
# The report: "as a bug if i click on the L or R it closes all". The Media pane's
# Inputs section is the BUSES — the audio-input and video-source rosters (and
# their cards' pill pairs) live on the Settings - Audio & Video board — so the
# card this guards is the bus card: clicking its L used to mute the WHOLE bus, so
# one click turned both sides dark.
#
# This drives the REAL card: it opens Media · Inputs, clicks the bus card's L pill
# (a real synthetic click at the pill's centre; the pill's own toggled() is the
# fallback when the click misses, and WHICH path ran is printed), then reads BOTH
# sides back out of the model. Judged:
#   * the clicked side is closed (0), and
#   * the OTHER side is still open (not 0).
# The scenario restores the trims it changed and removes any row it added.
#
# VERDICT (last line, greppable):
#   PILL: PASS bus=closed-own-side by=click,toggled() ...
#   PILL: FAIL launch | FAIL missing | FAIL bus
#
# Usage:  bash tools/selftest/run_pill_test.sh [wait-seconds]   (default 90)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/pill_test.log"
WAIT_SECS="${1:-90}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "PILL: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_PILL_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_PILL_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario opens the pane, clicks both pills and quits the app itself.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-PILL\] result" "$LOG" 2>/dev/null; then
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
grep -a "\[SELFTEST-PILL\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-PILL\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-PILL result line — the scenario never finished; see $LOG)"
    echo "PILL: FAIL missing"
    exit 3
fi
if echo "$RESULT" | grep -q "pane=missing"; then
    echo "PILL: FAIL launch — the Media pane was never found ($RESULT)"
    exit 4
fi

BUS="$(echo "$RESULT" | grep -o 'bus=[A-Z]*' | head -1 | cut -d= -f2)"
# Which path engaged the pill (a synthetic click can miss — the driver's own
# "window-foreground luck" note).
BUS_BY="$(grep -a "\[SELFTEST-PILL\] bus" "$LOG" 2>/dev/null | tail -1 \
          | grep -o 'by=[^ ]*' | head -1 | cut -d= -f2)"

if [ "$BUS" != "PASS" ]; then
    echo "PILL: FAIL bus — one click on the bus card's L closed more than L ($RESULT)"
    exit 5
fi
echo "PILL: PASS bus=closed-own-side busBy=${BUS_BY:-?}"
exit 0
