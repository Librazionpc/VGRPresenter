#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_seed_test.sh — does the dock's search box hold the open
# sermon, and do the digits typed after it move the pane?
#
# The report this exists for (user, with screenshots of The Table):
#
#   "let the search bar by default hold 55-0117 - How The Angel … so i can
#    easily just enter the paragraph … it should fill the search with it so that
#    i add 56 at the back … when i enter the 56 it did not move"
#
# This drives the REAL app the way the operator does: opens The Table, opens a
# sermon (55-0117 when the library has it), FOCUSES the dock's search box — the
# focus is what fills it — and TYPES "56" with real key events into it. It
# prints the box's own state at every step (text, hint, seed, focus) and the
# pane's selection after the digits, so a step that does not happen is its own
# line rather than an inference from the final position.
#
# VERDICT (last line, greppable):
#   SEED: PASS paragraph=56
#   SEED: FAIL launch | FAIL noresult | FAIL noseed | FAIL nofill | FAIL stayed
#
# Usage:  bash tools/selftest/run_seed_test.sh [wait-seconds]  (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/seed_test.log"
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
    echo "SEED: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG" "$BUILD"/seed_*.png
echo "== launching with VGR_SELFTEST=1 VGR_SEED_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SEED_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-SEED\] return boxText" "$LOG" 2>/dev/null; then
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
grep -a "\[SELFTEST-SEED\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8
echo "== screenshots =="
for shot in seed_before.png seed_after.png seed_box.png seed_scripture_box.png; do
    if [ -s "$BUILD/$shot" ]; then
        echo "$BUILD/$shot ($(stat -c %s "$BUILD/$shot") bytes)"
    else
        echo "$BUILD/$shot MISSING"
    fi
done

RESULT="$(grep -a "\[SELFTEST-SEED\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-SEED result line — the scenario never finished; see $LOG)"
    echo "SEED: FAIL noresult"
    exit 3
fi

SEEDED="$(grep -a "\[SELFTEST-SEED\] seeded" "$LOG" 2>/dev/null | tail -1)"
FOCUSED="$(grep -a "\[SELFTEST-SEED\] focused=" "$LOG" 2>/dev/null | tail -1)"

# The box must be holding the sermon BEFORE anyone types: the hint names it, and
# focusing fills the field with the same line (that is the whole "hold it for me"
# ask — a hint that only paints is not a query the caret can append to).
if ! echo "$SEEDED" | grep -q "hint='..*- "; then
    echo "SEED: FAIL noseed — the box's hint does not name a sermon (${SEEDED:-no line})"
    exit 4
fi
if ! echo "$FOCUSED" | grep -qE "boxText='..*- .* ' "; then
    echo "SEED: FAIL nofill — focusing the box did not fill it with the citation line (${FOCUSED:-no line})"
    exit 5
fi

# A SINGLE CLICK MUST NOT STAGE and a DOUBLE CLICK must (user call: "double click
# is what should push it to monitor preview, single click is preview pane").
CLICK="$(grep -a "\[SELFTEST-SEED\] click double" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$CLICK" ]; then
    echo "SEED: FAIL noclick — the click phase never ran (see $LOG)"
    exit 7
fi
if ! echo "$CLICK" | grep -q "verdict=ok"; then
    echo "SEED: FAIL click — single click staged something, or the double click staged nothing ($CLICK)"
    exit 8
fi

if ! echo "$RESULT" | grep -q "verdict=moved"; then
    echo "SEED: FAIL stayed — typing 56 left the pane at $RESULT"
    exit 6
fi

# SCRIPTURE: the same box, seeded with the open book and chapter, takes the VERSE.
SCRIPTURE="$(grep -a "\[SELFTEST-SEED\] scripture result boxText" "$LOG" 2>/dev/null | tail -1)"
SCRIPTURE_SEEDED="$(grep -a "\[SELFTEST-SEED\] scripture seeded" "$LOG" 2>/dev/null | tail -1)"
if ! echo "$SCRIPTURE_SEEDED" | grep -qE "hint='[^']+[0-9]+: '"; then
    echo "SEED: FAIL noscriptureseed — the Scripture box does not hold book + chapter (${SCRIPTURE_SEEDED:-no line})"
    exit 9
fi
if ! echo "$SCRIPTURE" | grep -q "atVerse=8"; then
    echo "SEED: FAIL scripture — typing 8 did not move Scripture to verse 8 ($SCRIPTURE)"
    exit 10
fi

# THE KEYBOARD'S WALK + RETURN on a bare code: one Down highlights the first result
# and its paragraph preview shows; Return leaves that result's citation line in the box.
WALK="$(grep -a "\[SELFTEST-SEED\] walk " "$LOG" 2>/dev/null | tail -1)"
RETURN="$(grep -a "\[SELFTEST-SEED\] return boxText" "$LOG" 2>/dev/null | tail -1)"
if ! echo "$WALK" | grep -qE "cursor=0 .*peekIndex=0 peek='.+'" ; then
    echo "SEED: FAIL walk — Down did not highlight the first result with its preview ($WALK)"
    exit 11
fi
if ! echo "$RETURN" | grep -qE "boxText='56-[0-9]{4} - .+ '"; then
    echo "SEED: FAIL return — Return did not leave the result's citation line in the box ($RETURN)"
    exit 12
fi

PARA="$(echo "$RESULT" | grep -oE "atParagraph=[0-9]+" | head -1 | cut -d= -f2)"
echo "SEED: PASS paragraph=$PARA verse=8 walk=1 return=1"
exit 0
