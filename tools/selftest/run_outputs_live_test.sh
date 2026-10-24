#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_outputs_live_test.sh — does EVERY output row report its own
# runtime, and does a blackout show up on all of them?
#
# The report it answers: "anything the notification does for one output should be
# implemented for all — it seems like the notification is only instantiated in
# one output, whereas it should be for all: fps change, blackout, whatever."
#
# The row's live line used to exist only for the kinds that own a sender (NDI,
# SDI): a HDMI row said "Inactive" and nothing else while an NDI row reported
# frames, so the one output with a sender looked like the only one the app was
# watching. The engine measures the same two facts for every kind
# (RenderEngine::OutputFeedAges: frames delivered + the age of the last present),
# LiveOutputService publishes them per row (outputRuntime), and the row prints
# one line for its own kind. This drives the app the way the operator does:
#
#   1. launches with VGR_SELFTEST=1 VGR_OUTSLIVE_TEST=1, puts ONE real slide on
#      air, opens Settings · Outputs;
#   2. reads every row's statusText — the value the line under the row binds to,
#      off the delegate item — WHILE LIVE: each row must open with its own kind
#      and none may still be saying "go live to send" (that is the off-air text);
#   3. presses the wall's Blackout control with a REAL click (SelfTest.clickItem,
#      the same control the operator uses), waits for the carrier AND the 1 Hz
#      feed probe, and reads every row again: each one must now say "blackout up"
#      — the stage-wide fact has to reach every kind of row, not just NDI's;
#   4. presses it again and reads once more: no row may still claim a blackout;
#   5. puts the POINTER on a tile's runtime dot (SelfTest.move) and reads the tip
#      it raised: the dot is colour-only, so the frame count has to be reachable
#      by hover or it is not reachable at all (a tip that never appears reads as
#      an empty string and fails);
#   6. does the same for the MONITOR WALL's tiles in every phase — each tile
#      carries its state as a DOT (no text: the row owns the words), so the
#      tile's own runtimeState is read off the tile as rendered and must be
#      "live" while live, "blackout" while blacked out, and "live" again after
#      — the dialog and the wall cannot disagree.
#
# VERDICT (last line, greppable):
#   OUTSLIVE: PASS rows=N live=N blackout=N restored=N
#   OUTSLIVE: FAIL launch | FAIL notrun | FAIL missing  (no output row at all)
#           | FAIL stale     (a row never reported, or still showed off-air text)
#           | FAIL blackout  (a row did not say the blackout)
#           | FAIL restore   (a row still said blackout after it ended)
#           | FAIL tiles     (a wall tile missing, stale, or disagreeing)
#           | FAIL hover     (the dot raised no tip, or one without a frame count)
#
# Usage:  bash tools/selftest/run_outputs_live_test.sh [wait-seconds]   (default 90)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/outputs_live_test.log"
WAIT_SECS="${1:-90}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "OUTSLIVE: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_OUTSLIVE_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_OUTSLIVE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-OUTSLIVE\] restored result" "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-OUTSLIVE\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

result_of() {   # result_of <phase> <field>
    grep -a "\[SELFTEST-OUTSLIVE\] $1 result" "$LOG" 2>/dev/null | tail -1 \
        | grep -o "$2=[^ ]*" | head -1 | cut -d= -f2
}

LIVE_LINE="$(grep -a "\[SELFTEST-OUTSLIVE\] live result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$LIVE_LINE" ]; then
    echo "(no live-phase result line — the scenario never reached Settings · Outputs while live; see $LOG)"
    echo "OUTSLIVE: FAIL notrun"
    exit 3
fi

ROWS="$(result_of live rows)"
LIVE_REPORTING="$(result_of live reporting)"
LIVE_OFFAIR="$(result_of live offair)"
LIVE_BLACKOUT="$(result_of live blackout)"

if [ -z "$ROWS" ] || [ "$ROWS" -lt 1 ]; then
    echo "OUTSLIVE: FAIL missing — Settings · Outputs listed no row: $LIVE_LINE"
    exit 4
fi
# Every row, every kind. `reporting` counts rows whose line opens with their own
# kind, so a row still riding the old NDI/SDI-only line (or an empty one) fails.
if [ "${LIVE_REPORTING:-0}" != "$ROWS" ]; then
    echo "OUTSLIVE: FAIL stale — $ROWS row(s) but only ${LIVE_REPORTING:-0} reported a runtime line of their own: $LIVE_LINE"
    exit 5
fi
# While live, no row may still be showing the OFF-AIR text: that would mean the
# row never learned the session started (the runtime map not reaching it).
if [ "${LIVE_OFFAIR:-0}" != "0" ]; then
    echo "OUTSLIVE: FAIL stale — ${LIVE_OFFAIR} row(s) still said \"go live to send\" during a live session: $LIVE_LINE"
    exit 6
fi
if [ "${LIVE_BLACKOUT:-0}" != "0" ]; then
    echo "OUTSLIVE: FAIL blackout — ${LIVE_BLACKOUT} row(s) claimed a blackout before one was pressed: $LIVE_LINE"
    exit 7
fi

BLACK_LINE="$(grep -a "\[SELFTEST-OUTSLIVE\] blackout result" "$LOG" 2>/dev/null | tail -1)"
BLACKED="$(result_of blackout blackout)"
if [ -z "$BLACK_LINE" ]; then
    echo "(no blackout-phase result line — the control was never pressed; see $LOG)"
    echo "OUTSLIVE: FAIL notrun"
    exit 8
fi
if [ "${BLACKED:-0}" != "$ROWS" ]; then
    echo "OUTSLIVE: FAIL blackout — the blackout reached ${BLACKED:-0} of $ROWS row(s); every kind has to say it: $BLACK_LINE"
    exit 9
fi

RESTORED_LINE="$(grep -a "\[SELFTEST-OUTSLIVE\] restored result" "$LOG" 2>/dev/null | tail -1)"
RESTORED_BLACK="$(result_of restored blackout)"
if [ -z "$RESTORED_LINE" ]; then
    echo "(no restored-phase result line — the scenario stopped early; see $LOG)"
    echo "OUTSLIVE: FAIL notrun"
    exit 10
fi
if [ "${RESTORED_BLACK:-0}" != "0" ]; then
    echo "OUTSLIVE: FAIL restore — ${RESTORED_BLACK} row(s) still reported a blackout after it ended: $RESTORED_LINE"
    exit 11
fi

# --- THE DOT'S HOVER DETAIL -------------------------------------------------
# The dot is colour-only on the picture, so the numbers live in its hover tip.
# An empty tip means the hover never reached the dot (or the tip was never
# published), which would leave the operator with a colour and no way to read it.
HOVER_TIP="$(grep -a "\[SELFTEST-OUTSLIVE\] hover tip=" "$LOG" 2>/dev/null | tail -1 \
    | sed -n 's/.*hover tip="\(.*\)"$/\1/p')"
if [ -z "$HOVER_TIP" ]; then
    echo "OUTSLIVE: FAIL hover — the pointer was put on a tile's runtime dot and no tip was raised"
    echo "OUTSLIVE: FAIL hover (the dot is colour-only: without its tip the frame count is unreachable)"
    exit 15
fi
case "$HOVER_TIP" in
    *frames*) ;;
    *)
        echo "OUTSLIVE: FAIL hover — the tip does not say how many frames the output received:"
        echo "  tip=\"$HOVER_TIP\""
        exit 15
        ;;
esac

# --- THE WALL'S TILES -------------------------------------------------------
# The same facts on the wall itself: "0:3 frames · 373 ms ago | 1:blackout".
# tile_verdict <phase> <must-contain|-> <must-not-contain> prints "bad/total"
# and returns 0 when every entry passes (a missing phase line is a failure too —
# that is a tile that was not found, which the scenario prints as <missing>).
tile_verdict() {
    local phase="$1" must="$2" mustnot="$3"
    local line payload entry n=0 bad=0
    line="$(grep -a "\[SELFTEST-OUTSLIVE\] $phase tiles=" "$LOG" 2>/dev/null | tail -1)"
    payload="$(printf '%s\n' "$line" | sed -n 's/.*tiles="\(.*\)"$/\1/p')"
    if [ -z "$payload" ]; then
        echo "0/0"
        return 1
    fi
    local IFS='|'
    for entry in $payload; do
        n=$((n + 1))
        case "$entry" in
            *"<missing>"*) bad=$((bad + 1)); continue ;;
        esac
        if [ "$must" != "-" ]; then
            printf '%s' "$entry" | grep -qF "$must" || bad=$((bad + 1))
        fi
        if [ "$mustnot" != "-" ] && printf '%s' "$entry" | grep -qF "$mustnot"; then
            bad=$((bad + 1))
        fi
    done
    echo "$bad/$n"
    [ "$bad" -eq 0 ] && [ "$n" -gt 0 ]
}

if ! LIVE_TILES_SET="$(tile_verdict live live -)"; then
    echo "OUTSLIVE: FAIL tiles — the wall's tiles were not showing a live state while the session was live ($LIVE_TILES_SET bad/total):"
    grep -a "\[SELFTEST-OUTSLIVE\] live tiles=" "$LOG" | tail -1
    exit 12
fi
if ! BLACK_TILES_SET="$(tile_verdict blackout blackout -)"; then
    echo "OUTSLIVE: FAIL tiles — a wall tile did not show the blackout ($BLACK_TILES_SET bad/total):"
    grep -a "\[SELFTEST-OUTSLIVE\] blackout tiles=" "$LOG" | tail -1
    exit 13
fi
if ! RESTORED_TILES_SET="$(tile_verdict restored - blackout)"; then
    echo "OUTSLIVE: FAIL tiles — a wall tile was missing or still showed blackout after it ended ($RESTORED_TILES_SET bad/total):"
    grep -a "\[SELFTEST-OUTSLIVE\] restored tiles=" "$LOG" | tail -1
    exit 14
fi
# "bad/total" from the helper; on success the total is the number of tiles that
# were checked in ALL THREE phases.
TILE_TOTAL="${LIVE_TILES_SET##*/}"
echo "hover: $HOVER_TIP"
echo "tiles: live=$LIVE_TILES_SET blackout=$BLACK_TILES_SET restored=$RESTORED_TILES_SET (bad/total each)"
echo "OUTSLIVE: PASS rows=$ROWS live=$LIVE_REPORTING blackout=$BLACKED restored=$ROWS tiles=$TILE_TOTAL"
exit 0
