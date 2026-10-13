#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_exact_test.sh — does the search padlock mean what it says?
#
# The feature: a padlock at the far end of the app-wide search field ("i want to
# add exact word ... exact word search"). Locked, every typed word must appear
# VERBATIM as a whole word — no completion ("friend" stops finding "friends"),
# no inner substring, no typo correction.
#
# This drives the REAL app: opens the dialog, types "frien" (not a word), reads
# the rows, CLICKS the lock (a real synthetic click at the item's centre) and
# reads them again; then types "friend" (a real word) with the lock still on.
# Two probes, because one word can only prove half the feature. Judged:
#   * "frien" loose returns rows AND lock returns strictly fewer (the lock filters), and
#   * "friend" locked returns at least one sermon row,
#   * EVERY sermon row that locked search returns carries the word as a WHOLE word.
#
# VERDICT (last line, greppable):
#   EXACT: PASS locked=<n> looseFrien=<n> lockedFrien=<n> violations=0
#   EXACT: FAIL launch | FAIL missing | FAIL violations=<n> | FAIL empty | FAIL nofilter
#
# Usage:  bash tools/selftest/run_exact_test.sh [wait-seconds]   (default 110)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/exact_test.log"
WAIT_SECS="${1:-110}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "EXACT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_EXACT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_EXACT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself once it has read the locked rows back.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-EXACT\] result" "$LOG" 2>/dev/null; then
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
grep -a "\[SELFTEST-EXACT\]" "$LOG" 2>/dev/null
echo "== item geometry (the driver's own resolve of the click sink) =="
grep -a "itemCenter" "$LOG" 2>/dev/null | head -3
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-EXACT\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-EXACT result line — the scenario never finished; see $LOG)"
    echo "EXACT: FAIL missing"
    exit 3
fi

# Plain grep/cut parsing: no sed group escapes to lose in transit.
LOCKED_FLAG="$(echo "$RESULT" | grep -o 'locked=[a-z]*' | head -1 | cut -d= -f2)"
LOCKED="$(echo "$RESULT" | grep -o 'rows=[0-9]*' | head -1 | cut -d= -f2)"
VIOL="$(echo "$RESULT" | grep -o 'violations=[0-9]*' | head -1 | cut -d= -f2)"
LOCKED_FRIEN="$(echo "$RESULT" | grep -o 'lockedProbe=[0-9-]*' | head -1 | cut -d= -f2)"
# Which path engaged the padlock: the real OS click, or the control's own handler
# (a synthetic click can miss — the driver's own "window-foreground luck" note).
TOGGLE_BY="$(grep -a "\[SELFTEST-EXACT\] toggle" "$LOG" 2>/dev/null | tail -1 \
            | awk '{print $3}' | cut -d= -f2 | tr -d '\r')"
LOOSE_FRIEN="$(grep -a "\[SELFTEST-EXACT\] unlocked" "$LOG" | tail -1 | grep -o 'rows=[0-9]*' | head -1 | cut -d= -f2)"

if [ "$LOCKED_FLAG" != "true" ]; then
    echo "EXACT: FAIL lock — the click did not turn the padlock on ($RESULT)"
    exit 4
fi
if [ -n "$VIOL" ] && [ "$VIOL" != "0" ]; then
    echo "EXACT: FAIL violations=$VIOL — a locked row does not carry the word whole"
    exit 5
fi
if [ -z "$LOCKED" ] || [ "$LOCKED" = "0" ]; then
    echo "EXACT: FAIL empty — the locked search returned no sermon row at all"
    exit 6
fi
# The discriminator: "frien" is not a word, so the loose search's rows came from
# completion/substring matching. Locked, there must be strictly fewer.
if [ -z "$LOOSE_FRIEN" ] || [ "$LOOSE_FRIEN" = "0" ] \
        || [ -z "$LOCKED_FRIEN" ] || [ "$LOCKED_FRIEN" -ge "$LOOSE_FRIEN" ]; then
    echo "EXACT: FAIL nofilter — the lock did not filter the fuzzy rows (loose=$LOOSE_FRIEN locked=$LOCKED_FRIEN)"
    exit 7
fi
# The SENTENCE probe: a multi-word query with the lock on must come back with
# paragraphs that carry the words TOGETHER (and none that merely sprinkle them):
# user call "prioritize the sentence first ... no individual words, the next
# available word combination".
SENT_LINE="$(grep -a "\[SELFTEST-EXACT\] sentence" "$LOG" 2>/dev/null | tail -1)"
SENT_LOCKED="$(echo "$SENT_LINE" | grep -o 'locked=[a-z]*' | head -1 | cut -d= -f2)"
SENT_ROWS="$(echo "$SENT_LINE" | grep -o 'rows=[0-9]*' | head -1 | cut -d= -f2)"
SENT_VIOL="$(echo "$SENT_LINE" | grep -o 'violations=[0-9]*' | head -1 | cut -d= -f2)"
if [ "$SENT_LOCKED" != "true" ] || [ -z "$SENT_ROWS" ] || [ "$SENT_ROWS" = "0" ]; then
    echo "EXACT: FAIL sentence — the locked multi-word search returned nothing ($SENT_LINE)"
    exit 10
fi
if [ "$SENT_VIOL" != "0" ]; then
    echo "EXACT: FAIL sentence — $SENT_VIOL locked rows do not carry the words together"
    exit 11
fi

# The TYPO under the lock: the gold correction line must still show (user call:
# "i was supposing to see the yellow auto correct show under") — "compeling"
# must be offered its fix even while the padlock is on.
HINT_LINE="$(grep -a "\[SELFTEST-EXACT\] hint" "$LOG" 2>/dev/null | tail -1)"
if ! echo "$HINT_LINE" | grep -qi "compelling"; then
    echo "EXACT: FAIL hint — the locked search did not offer the correction ($HINT_LINE)"
    exit 12
fi

# WHICH KIND LEADS: when a sermon row carries the query as a verbatim phrase, the
# Sermons group must not sit under a kind that only has the words scattered (user
# report: "this exist in the table but it showing bible at the top").
ORDER_LINE="$(grep -a "\[SELFTEST-EXACT\] order" "$LOG" 2>/dev/null | tail -1)"
ORDER_FIRST="$(echo "$ORDER_LINE" | grep -o 'first=[a-z-]*' | head -1 | cut -d= -f2)"
ORDER_PHRASE="$(echo "$ORDER_LINE" | grep -o 'tablePhrase=[a-z]*' | head -1 | cut -d= -f2)"
if [ "$ORDER_PHRASE" = "true" ] && [ "$ORDER_FIRST" != "table" ]; then
    echo "EXACT: FAIL order — a verbatim sermon phrase did not lead ($ORDER_LINE)"
    exit 13
fi

# The pane's pill (the OTHER search box: The Table's "Search in the sermons"):
# the same lock, reached through ReferencePane -> adapter -> TheTableService.
PANE_LINE="$(grep -a "\[SELFTEST-EXACT\] pane" "$LOG" 2>/dev/null | tail -1)"
PANE_LOCKED="$(echo "$PANE_LINE" | grep -o 'locked=[a-z]*' | head -1 | cut -d= -f2)"
PANE_LOOSE="$(echo "$PANE_LINE" | grep -o 'loose=[0-9-]*' | head -1 | cut -d= -f2)"
PANE_EXACT="$(echo "$PANE_LINE" | grep -o 'exactRows=[0-9]*' | head -1 | cut -d= -f2)"
if [ "$PANE_LOCKED" != "true" ] || [ -z "$PANE_LOOSE" ] || [ -z "$PANE_EXACT" ]; then
    echo "EXACT: FAIL pane — the pane's pill search was not exercised ($PANE_LINE)"
    exit 8
fi
if [ "$PANE_LOOSE" = "0" ] || [ "$PANE_EXACT" -ge "$PANE_LOOSE" ]; then
    echo "EXACT: FAIL pane — the pill's lock did not filter (loose=$PANE_LOOSE exact=$PANE_EXACT)"
    exit 9
fi
echo "EXACT: PASS locked=${LOCKED} looseFrien=${LOOSE_FRIEN} lockedFrien=${LOCKED_FRIEN} violations=0 sentenceRows=${SENT_ROWS} sentenceViol=0 hint=ok orderfirst=${ORDER_FIRST:-?} paneLoose=${PANE_LOOSE} paneLocked=${PANE_EXACT} toggleBy=${TOGGLE_BY:-?}"
exit 0
