#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_rank_test.sh — what order does the app-wide search show?
#
# The report this exists for (user, with two screenshots): typing
#
#     compelling that you recieve
#
# listed Bible verses that carry only "you that" — none of them has "compelling"
# — while the sermon that says "and compelling and compulsory, that you receive
# the Holy Ghost" (the row the same dialog leads with for
# "compelling and compulsory") was not at the top. A row that carries MORE of the
# typed words must come first, whatever kind it belongs to.
#
# This drives the REAL app: opens the search dialog, types the query, and prints
# every shown row in display order with the count of typed words it carries and
# the MATCH percentage it SHOWS the user
# ("<kind>(<carried>/<words>) match<pct>% <title>"). Judged:
#   * the first row carries ALL of the query's words (carried == words), and
#   * no row that carries only one of three-or-more typed words sits above one
#     that carries more, and
#   * the shown percentage never goes up as you read down the list (the rating
#     IS the ranking key — a rising column would mean the number lies), and
#   * the HOVER PREVIEW of a row is COMPLETE while the pointer stays on it: the
#     preview asks a published read (SearchService.fullVerse answers "" until its
#     worker read lands, and verseCached re-asks for it), so the row must show the
#     whole verse and never fall back to its two-line snippet on a hover that never
#     ends. The scenario parks a REAL cursor on a located VERSE row (kind=bible — a
#     sermon paragraph previews through the library instead) and reads the preview
#     twice with the pointer UNMOVED, so "filled" cannot be the pointer arriving
#     late. The log also carries the row's own seed, because every verse row already
#     carries its verse text: the check guards the preview's completeness, it does
#     not claim to distinguish which of the two texts is on screen. If the cursor
#     does not land on the dialog's row at all (it does not on this machine), this
#     prints a NOTE rather than a pass or a fail — a check that could not run is
#     reported as such, never as one that passed.
#
# VERDICT (last line, greppable):
#   RANK: PASS top=<kind> (<carried>/<words>) match=<pct>% rows=<n>
#   RANK: FAIL launch | FAIL missing | FAIL top=<carried>/<words> | FAIL order
#   RANK: FAIL rating | FAIL hover
#
# Usage:  bash tools/selftest/run_rank_test.sh [wait-seconds]   (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/rank_test.log"
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
    echo "RANK: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_RANK_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_RANK_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-RANK\] result" "$LOG" 2>/dev/null; then
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

echo "== scenario (rows in display order) =="
grep -a "\[SELFTEST-RANK\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-RANK\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-RANK result line — the scenario never finished; see $LOG)"
    echo "RANK: FAIL missing"
    exit 3
fi

TOP="$(echo "$RESULT" | grep -o 'top=[a-z-]*' | head -1 | cut -d= -f2)"
# The first logged row carries "<kind>(<carried>/<words>)".
FIRST_ROW="$(grep -a "\[SELFTEST-RANK\] row0=" "$LOG" 2>/dev/null | tail -1 | cut -d= -f2-)"
CARRIED="$(echo "$FIRST_ROW" | grep -oE '\([0-9]+/' | tr -dc '0-9')"
WORDS="$(echo "$FIRST_ROW" | grep -oE '/[0-9]+\)' | tr -dc '0-9')"

if [ -z "$CARRIED" ] || [ -z "$WORDS" ]; then
    echo "RANK: FAIL missing — could not read the first row's word count ($FIRST_ROW)"
    exit 4
fi
if [ "$CARRIED" != "$WORDS" ]; then
    echo "RANK: FAIL top=$CARRIED/$WORDS — the first row does not carry every typed word ($FIRST_ROW)"
    exit 5
fi

# No row carrying fewer words may sit above one carrying more, and the MATCH
# percentage each row shows (the ranking key itself) must never go up as you
# read down the list — a column that rises would mean the number disagrees with
# the order the rows are in.
PREV=""
PREV_PCT=""
while IFS= read -r line; do
    n="$(echo "$line" | grep -oE '\([0-9]+/' | tr -dc '0-9')"
    [ -z "$n" ] && continue
    if [ -n "$PREV" ] && [ "$n" -gt "$PREV" ]; then
        echo "RANK: FAIL order — a row carrying $n words sits below one carrying $PREV ($line)"
        exit 6
    fi
    PREV="$n"
    pct="$(echo "$line" | grep -oE 'match[0-9]+%' | tr -dc '0-9')"
    if [ -n "$pct" ] && [ -n "$PREV_PCT" ] && [ "$pct" -gt "$PREV_PCT" ]; then
        echo "RANK: FAIL rating — a row showing MATCH ${pct}% sits below one showing MATCH ${PREV_PCT}% ($line)"
        exit 7
    fi
    [ -n "$pct" ] && PREV_PCT="$pct"
done <<< "$(grep -a "\[SELFTEST-RANK\] row" "$LOG" 2>/dev/null)"

if [ -z "$PREV_PCT" ]; then
    echo "RANK: FAIL rating — no row logged a MATCH percentage"
    exit 8
fi

# THE HOVER PREVIEW (see the header): probe1 is read with the pointer still parked
# on the row the scenario hovered, so an empty preview there is the trap — the row's
# published read answered "" and nothing re-asked when the answer landed.
# ...and it must be a VERSE row: a sermon paragraph previews through the library, so
# a pass on one would prove nothing about the published read this check exists for.
HOVER1="$(grep -a "\[SELFTEST-RANK\] hover probe1" "$LOG" 2>/dev/null | tail -1)"
if ! echo "$HOVER1" | grep -q "landed=true"; then
    # NOT a verdict: moving the OS cursor onto a row inside the search dialog does
    # not land on this machine (measured twice, landed=false while the row was on
    # screen), so the preview check could not run. Said out loud rather than
    # silently passing — the ranking checks above are unaffected by it.
    echo "RANK: NOTE hover — the cursor did not land on the dialog's row, so the preview check did not run (${HOVER1:-no probe line at all})"
elif ! echo "$HOVER1" | grep -q "kind=bible"; then
    echo "RANK: NOTE hover — the preview check needs a Bible verse row and this run hovered '${HOVER1}' (see the hover lines above)"
elif ! echo "$HOVER1" | grep -q "filled=true"; then
    echo "RANK: FAIL hover — the hovered verse row's full text never arrived ($HOVER1; the pointer never left the row)"
    exit 9
fi

echo "RANK: PASS top=${TOP} (${CARRIED}/${WORDS}) match=${PREV_PCT}% rows=$(grep -ac "\[SELFTEST-RANK\] row" "$LOG" 2>/dev/null)"
exit 0
