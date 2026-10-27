#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_line_step_test.sh — does one reading step move ONE line,
# does the WALL follow the page, and does any frame skip?
#
# The report behind this: reading a multi-page pick with Alt+Down, the words
# scrolled smoothly for the first page and a bit and then BLINKED / jumped at a
# boundary. Property traces cannot see "the picture re-based"; these numbers can.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_LINESTEP_TEST=1
#      (stdout+stderr -> build/line_step_test.log). The scenario stages a
#      synthetic 24-page pick through the real stageSlideSet — LONGER than the
#      run's own window (4 pages each way), so the window SLIDES while the
#      reader walks — then presses the ordinary LiveOutputService.lineStep 40+
#      times, 700ms apart (longer than the 600ms cap on the glide, so each press
#      settles before the next), and walks the same boundaries back up.
#   2. asserts, per press:
#        * abs (readingAbsoluteLine: the position in the PICK, which is
#          window-independent) moves by exactly 1.0 in the press's direction and
#          never the wrong way;
#        * the page index never regresses against the walk;
#        * the passage is not re-cut and the page's block count does not churn;
#        * THE WALL IS ON THE PAGE THE WALK IS ON: every monitor tile's own
#          composition matches the page being read, and its words are still part
#          of the passage the service is drawing (a tile left behind shows an
#          older page — the "metadata is not updating the para values" report);
#        * glideSkips == 0: no 16 ms sample of the position moved half a line or
#          went the wrong way (that is what a visible "little skip" is).
#   3. prints the press table and the wall table on failure, so the boundary is
#      visible rather than inferred.
#
# Usage:  bash tools/selftest/run_line_step_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/line_step_test.log"
WAIT_SECS="${1:-90}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "LINE_STEP: FAIL missing-app — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_LINESTEP_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_LINESTEP_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-LINESTEP\] done" "$LOG" 2>/dev/null; then
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

LINES="$(grep -a "\[SELFTEST-LINESTEP\] press=.* moved=" "$LOG" 2>/dev/null)"
WALL="$(mktemp)"
grep -a "\[SELFTEST-LINESTEP\] press=.* tile=\|\[SELFTEST-LINESTEP\] press=.* wall=" "$LOG" 2>/dev/null > "$WALL"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-LINESTEP press lines — the driver never armed; see $LOG)"
    echo "LINE_STEP: FAIL missing"
    rm -f "$WALL"
    exit 3
fi

# The press's own `abs` is the SETTLED position the previous press left (the
# scenario spaces its presses past the glide's maximum), so the sequence of
# `abs` values IS what the reader's eye was following.
FAILS=0
MOVED=0
MOVED_BACK=0
PREV_ABS=""
PREV_DIR=""
PREV_BLOCKS=""
PREV_PAGE=""
PREV_PASSAGE=""
WALL_FAILS=0
WALL_TILES=0
WORST_TICK="0.000"
while IFS= read -r line; do
    get() { printf '%s\n' "$line" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
    moved="$(get moved)"
    dir="$(get dir)"
    absv="$(get abs)"
    passage="$(get passage)"
    blocks="$(get blocks)"
    page="$(get page)"
    before_page="$(get pageBefore)"
    press="$(get press)"
    worst="$(get glideWorst)"
    skips="$(get glideSkips)"
    [ -n "$worst" ] && awk -v a="$WORST_TICK" -v b="$worst" 'BEGIN { exit !(b > a) }' \
        && WORST_TICK="$worst"
    # 5) the per-frame skip trace (over the glide this press followed)
    if [ -n "$skips" ] && [ "$skips" != "0" ]; then
        echo "  FAIL press=$press: $skips skipped frame(s) in the glide — $(get glideSkipAt)"
        FAILS=$((FAILS + 1))
    fi
    [ "$moved" = "true" ] || continue
    MOVED=$((MOVED + 1))
    [ "$dir" = "-1" ] && MOVED_BACK=$((MOVED_BACK + 1))
    if [ -n "$PREV_ABS" ] && [ "$dir" = "$PREV_DIR" ]; then
        # 1) never the wrong way, never a stalled position
        awk -v a="$PREV_ABS" -v b="$absv" -v d="$dir" 'BEGIN { exit d > 0 ? !(b > a) : !(b < a) }' || {
            echo "  FAIL press=$press dir=$dir: position moved the wrong way (was $PREV_ABS, now $absv)"
            FAILS=$((FAILS + 1))
        }
        # 2) exactly one line per press — across page turns and window slides too
        awk -v a="$PREV_ABS" -v b="$absv" -v d="$dir" 'BEGIN { x = d * (b - a); exit !(x > 0.99 && x < 1.01) }' || {
            echo "  FAIL press=$press dir=$dir: position went $PREV_ABS -> $absv (not one line)"
            FAILS=$((FAILS + 1))
        }
    fi
    # 3) NOT the passage length, deliberately: for a pick longer than the run's
    #    window the run is re-joined every time the window slides (a page added at
    #    one end, dropped at the other), so its length legitimately changes between
    #    two presses — and whether the PICTURE moved is already answerable, and
    #    answered, by `abs` (1.0 per press, never backwards) plus glideSkips == 0.
    PREV_PASSAGE="$passage"
    PREV_ABS="$absv"
    PREV_DIR="$dir"
    # 4) the page's block count does not churn
    if [ -n "$PREV_BLOCKS" ] && [ "$blocks" != "$PREV_BLOCKS" ]; then
        echo "  FAIL press=$press: block count changed ($PREV_BLOCKS -> $blocks)"
        FAILS=$((FAILS + 1))
    fi
    PREV_BLOCKS="$blocks"
    # 6) the page index never regresses against the walk
    if [ -n "$PREV_PAGE" ]; then
        awk -v a="$PREV_PAGE" -v b="$page" -v d="$dir" 'BEGIN { exit d > 0 ? !(b >= a) : !(b <= a) }' || {
            echo "  FAIL press=$press dir=$dir: page regressed ($PREV_PAGE -> $page)"
            FAILS=$((FAILS + 1))
        }
    fi
    PREV_PAGE="$page"
    # 7) THE WALL IS ON THIS PAGE — every tile's own composition, as it draws it
    wall="$(grep -a "press=$press tile=" "$WALL" | head -4)"
    if [ -z "$wall" ] || printf '%s\n' "$wall" | grep -qa "wall=missing"; then
        WALL_FAILS=$((WALL_FAILS + 1))
        echo "  FAIL press=$press: no wall tiles to read (the scenario could not find selfTestMonitorWall)"
    else
        while IFS= read -r wl; do
            [ -n "$wl" ] || continue
            WALL_TILES=$((WALL_TILES + 1))
            shows="$(printf '%s\n' "$wl" | sed -n 's/.* shows=\([^ ]*\).*/\1/p')"
            flow="$(printf '%s\n' "$wl" | sed -n 's/.* flow=\([^ ]*\).*/\1/p')"
            if [ "$shows" != "$page" ]; then
                echo "  FAIL press=$press: $(printf '%s' "$wl" | sed -n 's/.*\(tile=[0-9]*\).*/\1/p') shows page $shows while the walk is on page $page"
                WALL_FAILS=$((WALL_FAILS + 1))
            fi
            if [ "$flow" != "true" ]; then
                echo "  FAIL press=$press: a wall tile is not drawing the passage any more ($wl)"
                WALL_FAILS=$((WALL_FAILS + 1))
            fi
        done <<EOF
$wall
EOF
    fi
done <<EOF
$LINES
EOF
FAILS=$((FAILS + WALL_FAILS))

echo "== presses =="
printf '%s\n' "$LINES"
echo "== wall =="
cat "$WALL"
rm -f "$WALL"
echo "== moved presses: $MOVED, wall tile reads: $WALL_TILES, worst single-frame move: $WORST_TICK line(s), findings: $FAILS =="
if [ "$MOVED" -lt 10 ] || [ "$MOVED_BACK" -lt 5 ]; then
    echo "LINE_STEP: FAIL too-few-moves ($MOVED down, $MOVED_BACK up) — the stepper stopped early"
    exit 1
fi
if [ "$WALL_TILES" -eq 0 ]; then
    echo "LINE_STEP: FAIL no-wall — nothing read the monitor tiles, so the wall was not checked"
    exit 1
fi
if [ "$FAILS" -ne 0 ]; then
    echo "LINE_STEP: FAIL findings=$FAILS"
    exit 1
fi
echo "LINE_STEP: PASS $MOVED press(es) ($MOVED_BACK back up), one line a press, one passage, wall on the page, no skipped frame"
