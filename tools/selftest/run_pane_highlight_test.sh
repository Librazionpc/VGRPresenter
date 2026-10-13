#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_pane_highlight_test.sh — does The Table pane's result list
# show the hits?
#
# The report this exists for (user, with the pane's screenshot): typing
# "compelling that" into the pane's own search box listed rows whose snippet had
# "that" highlighted and "compelling" plain — and rows where nothing was
# highlighted at all, because the matched words sat past the visible edge of the
# one-line row:
#
#   "i think the typo guy is working but the highlighter is not highlighting
#    compelling ... so i expect the highlighted text to show well like in the
#    centre not at the extreme so user can see the hits well"
#
# This drives the REAL app: opens The Table pane's search, TYPES the query into
# the pane's own box (the field's debounce runs the search, as a user's keystrokes
# do), and prints every row exactly as the pane renders it — the centred snippet
# with the wrapped words marked «like this», where the first hit sits in the row,
# and the markup the row Text itself carries. Two passes:
#   1. "compelling that" — the user's own query: the highlight must wrap BOTH
#      words and the hit cluster must sit in the middle of the one-line row;
#   2. "complling that" — the same query misspelled: the pane's box must show the
#      typo fixer's correction while results are on screen.
#
# VERDICT (last line, greppable):
#   PANEHL: PASS rows=<n> firstHitAt=<pct>% worstHitAt=<pct>% typoRows=<m>
#   PANEHL: FAIL launch | FAIL missing | FAIL unlit | FAIL clipped
#           | FAIL typosearch | FAIL nohint
#
# Usage:  bash tools/selftest/run_pane_highlight_test.sh [wait-seconds]  (default 100)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/panehl_test.log"
WAIT_SECS="${1:-100}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "PANEHL: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_PANEHL_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_PANEHL_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-PANEHL\] result" "$LOG" 2>/dev/null; then
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

echo "== scenario (rows as the pane renders them) =="
grep -a "\[SELFTEST-PANEHL\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8
echo "== pane screenshots =="
for shot in panehl_pass1.png panehl_pass2.png panehl_row0.png; do
    if [ -s "$BUILD/$shot" ]; then
        echo "$BUILD/$shot ($(stat -c %s "$BUILD/$shot") bytes)"
    else
        echo "$BUILD/$shot MISSING"
    fi
done

RESULT="$(grep -a "\[SELFTEST-PANEHL\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-PANEHL result line — the scenario never finished; see $LOG)"
    echo "PANEHL: FAIL missing"
    exit 3
fi

field() {   # field <name-pattern> [strip-characters-for-numbers]
    echo "$RESULT" | grep -oE "$1='?[^' ]*'?" | head -1 | sed -E "s/$1='?//;s/'?$//"
}
ROWS="$(field rows)"
UNLIT="$(field unhiglightedRows)"
FIRST="$(field firstHitAt | tr -dc '0-9')"
WORST="$(field worstHitAt | tr -dc '0-9')"
RENDERED="$(field renderedUnmarked)"
TYPO_ROWS="$(field typoRows)"
TYPO_RENDERED="$(field typoRenderedUnmarked)"
TYPO_HINT="$(field typoHint)"
TYPO_HINT_SHOWN="$(field typoHintShown)"

if [ -z "$ROWS" ] || [ "$ROWS" = "0" ]; then
    echo "PANEHL: FAIL missing — the pane's search returned no rows"
    exit 4
fi
if [ "${UNLIT:-0}" != "0" ]; then
    echo "PANEHL: FAIL unlit — $UNLIT row(s) show a typed word plainly, not wrapped"
    exit 5
fi
if [ -n "$RENDERED" ]; then   # empty = clean; "missing" = no row Text on screen
    echo "PANEHL: FAIL unlit — the rendered row's own text carries a typed word plainly ('$RENDERED')"
    exit 6
fi
if [ -z "$WORST" ] || [ "$WORST" -gt 55 ]; then
    echo "PANEHL: FAIL clipped — a row's first hit sits at ${WORST:-?}% of the visible text (want <= 55%)"
    exit 7
fi
# The typo fixer's hint must be in THIS box too (user call), and the misspelled
# query must still find the passage the fixed words name.
if [ -z "$TYPO_ROWS" ] || [ "$TYPO_ROWS" = "0" ]; then
    echo "PANEHL: FAIL typosearch — the misspelled query returned no rows in the pane"
    exit 8
fi
if ! echo "$TYPO_HINT" | grep -q 'compelling'; then
    echo "PANEHL: FAIL nohint — the pane showed no correction for the misspelling (hint='${TYPO_HINT:-}')"
    exit 9
fi
if [ "$TYPO_HINT_SHOWN" != "true" ]; then
    echo "PANEHL: FAIL nohint — the correction line is not on screen in the pane (shown='${TYPO_HINT_SHOWN:-}')"
    exit 10
fi
if [ -n "$TYPO_RENDERED" ]; then
    echo "PANEHL: FAIL unlit — the corrected row carries a typed word plainly ('$TYPO_RENDERED')"
    exit 10
fi

for shot in panehl_pass1.png panehl_pass2.png panehl_row0.png; do
    [ -s "$BUILD/$shot" ] || { echo "PANEHL: FAIL noshot — $BUILD/$shot was never written"; exit 11; }
done

# PIXELS, not properties: row 0's own Text, grabbed and counted. A highlight the
# user cannot see is a highlight that does not exist, whatever the markup says.
echo "== row 0's own pixels (#ffd54a = the highlight gold) =="
PIXEL_VERDICT="$(node "$HERE/gold_pixels.mjs" "$BUILD/panehl_row0.png" --min 20 2>&1 | tail -1)"
echo "$PIXEL_VERDICT"
case "$PIXEL_VERDICT" in
    GOLDPIX:\ PASS*) ;;
    *) echo "PANEHL: FAIL unlit — row 0 painted no highlight gold ($PIXEL_VERDICT)"; exit 12 ;;
esac

echo "PANEHL: PASS rows=${ROWS} firstHitAt=${FIRST}% worstHitAt=${WORST}% typoRows=${TYPO_ROWS} typoHint='${TYPO_HINT}' hintShown=${TYPO_HINT_SHOWN}"
exit 0
