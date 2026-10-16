#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_pane_rows_test.sh — does The Table pane's result list build
# a VIEWPORT, or the whole model at once?
#
# The ask this exists for (user, right after the paragraph list was virtualized):
#
#   "Make the reference pane's search-results list virtualized as well, since a
#    two-character query can still build about a thousand rows at once."
#
# A row count cannot answer that. A long list is fine; BUILDING it in one go is
# what freezes the pointer, so the scenario measures the two things only a
# windowed list does, for each of the three lists the pane owns — the tab
# search's citation matches, the hover peek's sermon paragraphs, and the pill
# search's hits:
#
#   * with the query's rows on screen, `built` stays a viewport's worth
#     (built < rows), and
#   * scrolling to the END materializes the LAST row (lastRow=true) while the
#     total ever built stays below the row count (scrolledBuilt < rows) — a
#     windowed list must reach its tail, or the end of a 641-match query would be
#     unreachable.
#
# It drives the REAL app: the two-character query ("th") is TYPED into the tab
# bar's box (the provider runs per keystroke, as under a user's fingers), the
# hover is the pane's own peekSuggestion() on its LONGEST matching sermon, and
# the pill's field is set the way the pane-highlight scenario sets it.
#
# Library-independent: each list's numbers are compared to each other, so a small
# library still decides the question — but a list too short for the question to
# mean anything (fewer than 60 rows) is reported as SKIP rather than passed, and
# the citation list is the one the user's two characters actually drive, so it
# must be big enough to judge.
#
# VERDICT (last line, greppable):
#   PANEROWS: PASS matches=<rows>/<built>/<scrolledBuilt>/<lastRow> ...
#   PANEROWS: FAIL launch | FAIL missing | FAIL whole | FAIL tail | FAIL lastrow
#             | FAIL small | FAIL qml
#
# Usage:  bash tools/selftest/run_pane_rows_test.sh [wait-seconds]  (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/panerows_test.log"
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
    echo "PANEROWS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_PANEROWS_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_PANEROWS_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-PANEROWS\] result" "$LOG" 2>/dev/null; then
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

echo "== scenario (per list: rows / built / scrolledBuilt / lastRow) =="
grep -a "\[SELFTEST-PANEROWS\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
QML_ERRORS="$(grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8)"
echo "${QML_ERRORS:-none}"
echo "== pane screenshot =="
if [ -s "$BUILD/panerows_pane.png" ]; then
    echo "$BUILD/panerows_pane.png ($(stat -c %s "$BUILD/panerows_pane.png") bytes)"
else
    echo "$BUILD/panerows_pane.png MISSING"
fi

RESULT="$(grep -a "\[SELFTEST-PANEROWS\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-PANEROWS result line — the scenario never finished; see $LOG)"
    echo "PANEROWS: FAIL missing"
    exit 3
fi

# The line format is fixed by paneRowsLog(): "<which> <phase> rows=N built=N
# scrolledBuilt=N lastRow=<bool>". " built=" only ever matches the built field
# (the other one is " scrolledBuilt="), and " rows=" only ever the row count.
value() { echo "$1" | sed -E "s/.* $2=([^ ]*).*/\1/"; }

FAILED=0
JUDGED=0
MATCH_JUDGED=0
SKIPPED=""
SUMMARY=""
for which in match peek hit; do
    LIST="$(grep -a "\[SELFTEST-PANEROWS\] $which kept" "$LOG" 2>/dev/null | tail -1)"
    # The tail half of the measurement is its own line (the scroll has to land
    # before the rows it brought in exist), so the two are read together: rows +
    # built as they stood with the query's rows on screen, scrolledBuilt + lastRow
    # from after the scroll to the list's end.
    TAIL="$(grep -a "\[SELFTEST-PANEROWS\] $which scrolled" "$LOG" 2>/dev/null | tail -1)"
    if [ -z "$LIST" ] || [ -z "$TAIL" ]; then
        echo "PANEROWS: FAIL missing — no '$which' line: the scenario never got that list on screen (see $LOG)"
        exit 3
    fi
    ROWS="$(value "$LIST" rows)"
    BUILT="$(value "$LIST" built)"
    SCROLLED="$(value "$TAIL" scrolledBuilt)"
    LAST="$(value "$TAIL" lastRow)"
    SUMMARY="$SUMMARY $which=$ROWS/$BUILT/$SCROLLED/$LAST"
    if [ "${ROWS:-0}" -lt 60 ]; then
        SKIPPED="$SKIPPED $which(${ROWS})"
        echo "  $which: SKIP — ${ROWS} row(s): too few for \"is the whole model built\" to mean anything"
        continue
    fi
    OK=1
    if [ "${BUILT:-0}" -ge "$ROWS" ]; then
        echo "  $which: FAIL — built=$BUILT of rows=$ROWS: the list built the whole model"
        OK=0
    fi
    if [ "${SCROLLED:-0}" -ge "$ROWS" ]; then
        echo "  $which: FAIL — scrolledBuilt=$SCROLLED of rows=$ROWS: reaching the end built the whole model"
        OK=0
    fi
    if [ "$LAST" != "true" ]; then
        echo "  $which: FAIL — the LAST row (index $((ROWS - 1))) is not on screen after scrolling to the end"
        OK=0
    fi
    if [ "$OK" = "1" ]; then
        JUDGED=$((JUDGED + 1))
        [ "$which" = "match" ] && MATCH_JUDGED=1
        echo "  $which: OK rows=$ROWS built=$BUILT scrolledBuilt=$SCROLLED lastRow=$LAST"
    else
        FAILED=$((FAILED + 1))
    fi
done

if [ "$FAILED" != "0" ]; then
    echo "PANEROWS: FAIL whole — $FAILED list(s) built their model instead of a viewport$SUMMARY"
    exit 4
fi
# The citation list is the one the user's two characters drive ("th" matches
# hundreds of sermons in a real library): with it unjudgeable, nothing here has
# answered the question — an empty pane, not a small library, is the likely cause.
if [ "$MATCH_JUDGED" = "0" ]; then
    echo "PANEROWS: FAIL small — the tab search returned too few citation matches to judge$SUMMARY"
    exit 5
fi
if [ -n "$QML_ERRORS" ]; then
    echo "PANEROWS: FAIL qml — the run logged QML errors (see above)"
    exit 6
fi

echo "PANEROWS: PASS judged=$JUDGED skipped='${SKIPPED# }'$SUMMARY"
exit 0
