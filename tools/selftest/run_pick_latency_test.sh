#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_pick_latency_test.sh — how long does a verse pick take to
# reach the monitor preview?
#
# The report this exists for: "when i click on scripture it takes too long
# before it shows in monitor preview unlike the table". That is a claim about
# milliseconds, and the only answer worth having is the app's own clock — a
# stopwatch on the operator's desk cannot tell the pick's own cost apart from
# the compose behind it.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_PICKLAT_TEST=1
#      (stdout+stderr -> build/pick_latency_test.log)
#   2. shows the Scripture tab exactly as the tab click does
#      (showScreen.showInLibrary), waits for the pane's chapter to fill
#   3. takes the chapter's SECOND verse through the pane's OWN staging gesture
#      (stageOutputPart — the body of a row's double-click) and prints:
#        * syncMs  — how long that call blocked the interface thread, i.e. the
#                    wait the operator sees before anything can be redrawn;
#        * tileMs  — when the monitor tile's composition first HELD the verse,
#                    polled every 8 ms from the same t0. A poll cannot run while
#                    the interface thread is inside the pick, so tileMs >= syncMs
#                    always; the GAP between them is the compose/binding cost
#                    behind the pick rather than the pick itself.
#
# The second verse on purpose: the first is what a freshly opened tab already
# has staged, and a pick that changes nothing measures nothing.
#
# VERDICT (last line, greppable):
#   PICKLAT: PASS syncMs=<n> tileMs=<n>
#   PICKLAT: FAIL launch | FAIL missing | FAIL nopick | FAIL notile
#
# Usage:  bash tools/selftest/run_pick_latency_test.sh [wait-seconds]
#         (default 60; the scenario arms at 22 s + 8 s after launch)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/pick_latency_test.log"
WAIT_SECS="${1:-60}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh" 2>/dev/null || true

if [ ! -f "$BUILD/$APP" ]; then
    echo "PICKLAT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"

echo "== launching with VGR_SELFTEST=1 VGR_PICKLAT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_PICKLAT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-PICKLAT\] done" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
kill "$RUN_PID" 2>/dev/null
wait "$RUN_PID" 2>/dev/null

echo "== the pick, measured =="
LINES="$(grep -a "\[SELFTEST-PICKLAT\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-PICKLAT lines — the driver never armed; see $LOG)"
    echo "PICKLAT: FAIL missing"
    exit 3
fi
printf '%s\n' "$LINES"

echo "== QML errors (should be none) =="
grep -aE "QML .*error|RangeError|Cannot read|TypeError|is not defined" "$LOG" 2>/dev/null | head -8

FIELD() {   # FIELD <name> — one number out of the pick line
    echo "$LINES" | grep -a "kind=scripture" |
        grep -oE "$1=[0-9]+" | head -1 | sed -E "s/$1=//"
}

SYNC_MS="$(FIELD syncMs)"
TILE_LINE="$(echo "$LINES" | grep -a "tileMs=" | head -1)"
TILE_MS="$(echo "$TILE_LINE" | grep -oE "tileMs=[0-9]+" | head -1 | sed -E "s/tileMs=//")"

if [ -z "$SYNC_MS" ]; then
    echo "PICKLAT: FAIL nopick — the scenario never reached its pick (see $LOG)"
    exit 4
fi
if [ -z "$TILE_MS" ]; then
    echo "PICKLAT: FAIL notile — the pick ran (syncMs=$SYNC_MS) but no tile ever held the verse"
    exit 5
fi

echo "PICKLAT: PASS syncMs=$SYNC_MS tileMs=$TILE_MS"
exit 0
