#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_presplit_test.sh — is the paragraph list ALREADY divided?
#
# The regression guard for the reported bug: "the list is still not spliting it
# wait till i click on it". A sermon's long paragraph used to read as one long
# line until it was clicked, because the rows were built from the PICK's own
# preview — so the division the operator needed landed on the interface thread
# at the exact moment they were presenting. The chapter is now cut in the
# background before it is shown, and the rows come from that cut.
#
# The scenario opens a sermon with nothing clicked, EMPTIES the pick (the one
# state in which a pick cannot be what divided the rows) and reads the verse
# list's own model. Then it clicks a continuation row for real and checks the
# piece it names is the piece the pane's preview moves to.
#
# The cut arrives in TWO PIECES now (SlideBuilder's head/remainder block): the head with
# the open, so the list is divided the moment it appears, and the rest of the passage a
# beat later, so a paragraph BELOW the fold is divided too. The scenario measures both and
# the runner reports both (openSplitRows / finalSplitRows / settledMs).
#
# VERDICT (last line, greppable):
#   PRESPLIT: PASS                        — divided before the click, the rest of the cut
#                                           landed, and the click agrees
#   PRESPLIT: FAIL not-split              — the list showed no continuations
#   PRESPLIT: FAIL cut-incomplete         — the list lost division after the open
#   PRESPLIT: FAIL click-disagrees        — the row and the click named different pieces
#   PRESPLIT: FAIL launch                 — the app never came up (see the log)
#   PRESPLIT: FAIL missing                — the scenario never reported (see the log)
#
# Usage:  bash tools/selftest/run_presplit_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/presplit_test.log"
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
    echo "PRESPLIT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_PRESPLIT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_PRESPLIT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself once it has clicked its row.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-PRESPLIT\] result" "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
grep -a "\[SELFTEST-PRESPLIT\]" "$LOG" 2>/dev/null || true
# The cuts the services logged — where the two halves show up: the head with the open,
# the remainder right behind it, and the neighbours read with their own heads cut. They
# live in the ENGINE log (EngineBridge::write), not in the app's own stdout.
echo "== the cuts the load logged (head, then the remainder, then the neighbours) =="
grep -a "SlideWarm" "$BUILD/logs/engine.log" 2>/dev/null | tail -6 || true

LINE="$(grep -a "\[SELFTEST-PRESPLIT\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$LINE" ]; then
    echo "(no SELFTEST-PRESPLIT result line — the scenario never finished; see $LOG)"
    echo "PRESPLIT: FAIL launch"
    exit 3
fi

SPLIT_ROWS="$(echo "$LINE" | sed -n 's/.*splitRows=\([0-9]*\).*/\1/p')"
OPEN_SPLIT="$(echo "$LINE" | sed -n 's/.*openSplitRows=\([0-9]*\).*/\1/p')"
FINAL_SPLIT="$(echo "$LINE" | sed -n 's/.*finalSplitRows=\([0-9]*\).*/\1/p')"
OPEN_MS="$(echo "$LINE" | sed -n 's/.*openMs=\([0-9]*\).*/\1/p')"
SETTLED_MS="$(echo "$LINE" | sed -n 's/.*settledMs=\([0-9]*\).*/\1/p')"
AGREEMENT="$(echo "$LINE" | sed -n 's/.*agreement=\([a-z]*\).*/\1/p')"

if [ -z "$SPLIT_ROWS" ] || [ "$SPLIT_ROWS" -le 0 ]; then
    echo "PRESPLIT: FAIL not-split"
    exit 1
fi
if [ "$AGREEMENT" != "true" ]; then
    echo "PRESPLIT: FAIL click-disagrees"
    exit 1
fi
# THE HEAD IS HALF THE STORY (SlideBuilder's head/remainder block): the open hands over
# the rows the list can paint with the head's cut, and the rest of the passage is cut right
# after and handed over again. So the run reports all three numbers — rows divided when the
# sermon appeared (openSplitRows, the reported bug's own case), rows divided once the rest
# of the cut landed (finalSplitRows), and how long that took (settledMs) — and fails if the
# list LOST division between the two, which is the one way the second half could go wrong.
if [ -z "$FINAL_SPLIT" ] || [ "$FINAL_SPLIT" -lt "$SPLIT_ROWS" ]; then
    echo "PRESPLIT: FAIL cut-incomplete (open=$OPEN_SPLIT settled=$FINAL_SPLIT judged=$SPLIT_ROWS)"
    exit 1
fi
echo "PRESPLIT: PASS splitRows=$SPLIT_ROWS openSplitRows=$OPEN_SPLIT finalSplitRows=$FINAL_SPLIT openMs=$OPEN_MS settledMs=$SETTLED_MS"
exit 0
