#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_line_step_test.sh — does one reading step move ONE line,
# does the WALL follow the page, and does any frame skip?
#
# The report behind this: reading a multi-page pick with Alt+Down, the words
# scrolled smoothly for the first page and a bit and then BLINKED / jumped at a
# boundary. Property traces cannot see "the picture re-based"; these numbers can.
#
# THE STEP IS THE WALL'S › IN SCROLL MODE NOW — Alt+Down / Alt+Up were removed and
# the wall's Slide/Scroll switch (MonitorWall) decides what its arrows do. The
# scenario switches Scroll ON through that button before it stages the pick, and
# says which way it ran (`[SELFTEST-READING] ... mode=scroll`); every press line
# carries the mode too. A run in Slide mode is not a scroll measurement, so the
# runner refuses it instead of judging numbers that describe something else.
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
BUILD="${SELFTEST_BUILD_DIR:-$ROOT/build}"
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
# ONLY THIS BUILD'S INSTANCES, MATCHED BY THE EXECUTABLE'S OWN PATH. The machine
# may be running the operator's app from build/ while this runs from
# build-agent/ (SELFTEST_BUILD_DIR): killing by NAME took that instance down with
# it. Every process here is matched by its executable's path instead.
BUILD_WIN="$(cygpath -w "$BUILD" 2>/dev/null || printf '%s' "$BUILD")"
app_pids() {
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | ForEach-Object { \$_.Id }" \
        2>/dev/null | tr -d '\r'
}
echo "== stopping this build's instances ($BUILD_WIN) =="
for _pid in $(app_pids); do
    powershell -NoProfile -Command "Stop-Process -Id $_pid -Force" >/dev/null 2>&1
done
sleep 2
# ONE INSTANCE, ONE LOG. A survivor writes to the SAME log and runs its own copy
# of the scenario's timers, so the file becomes several interleaved walks —
# measured: the runner read 184 tile lines from a log the app was still appending
# to, half of them from an older instance that had gone LIVE, and judged the mix
# ("shows=-1 text=\"Probe 10\"" on a walk whose tiles all read their page).
# Refusing to run beats judging a mix.
if [ -n "$(app_pids)" ]; then
    echo "LINE_STEP: FAIL stale-instance — an instance of THIS build survived the stop;" \
         "it would append to this run's log. Close it and re-run."
    exit 5
fi

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_LINESTEP_TEST=1 (log -> $LOG) =="
# THE APP IS THE DIRECT CHILD, so $! below is ITS pid. The old form launched a
# subshell and captured the subshell's pid, which is why the stop at the end left
# the app running (see the instance check above).
cd "$BUILD" || exit 2
VGR_SELFTEST=1 VGR_LINESTEP_TEST=1 \
    PATH="$MINGW_BIN:$QT_BIN:$PATH" \
    "./$APP" > "$LOG" 2>&1 &
RUN_PID=$!
cd "$ROOT" || exit 2

TICK=0
# DID THE APP EXIT ON ITS OWN? A crash mid-walk leaves a short press table that
# reads exactly like a stepper which stopped moving — and it is not one: measured
# here, a run that "stalled at press 5" was the app dying with heap corruption
# (Windows Application Error 1000, 0xc0000374 in ntdll). The verdict below says
# so by name, with the scenario's own line count, instead of blaming the step.
EARLY_EXIT=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-LINESTEP\] done" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        EARLY_EXIT=1
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
kill "$RUN_PID" 2>/dev/null
wait "$RUN_PID" 2>/dev/null
# AND CONFIRM IT IS GONE: the log is only trustworthy if nothing can append to it
# after this line (see the launch above).
for _ in 1 2 3 4 5; do
    [ -z "$(app_pids)" ] && break
    sleep 1
done
if [ -n "$(app_pids)" ]; then
    echo "LINE_STEP: FAIL stale-instance — the app did not exit; its own walk would join this log"
    exit 6
fi

if [ "$EARLY_EXIT" = "1" ] && ! grep -qa "\[SELFTEST-LINESTEP\] done" "$LOG" 2>/dev/null; then
    echo "LINE_STEP: FAIL crashed — the app exited by itself after" \
         "$(grep -ac 'SELFTEST-LINESTEP] press=.* moved=' "$LOG" 2>/dev/null) press(es)," \
         "before the scenario finished. Check Windows' Application Error log" \
         "(Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000}) for" \
         "the faulting module: this is a crash, not a stepper that refused to move."
    exit 7
fi

LINES="$(grep -a "\[SELFTEST-LINESTEP\] press=.* moved=" "$LOG" 2>/dev/null)"
WALL="$(mktemp)"
grep -a "\[SELFTEST-LINESTEP\] press=.* tile=\|\[SELFTEST-LINESTEP\] press=.* wall=" "$LOG" 2>/dev/null > "$WALL"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-LINESTEP press lines — the driver never armed; see $LOG)"
    echo "LINE_STEP: FAIL missing"
    rm -f "$WALL"
    exit 3
fi

# SCROLL MODE IS THE PRECONDITION (see the header): the presses are
# LiveOutputService.lineStep, which is what the wall's › does in Scroll mode and
# what only Scroll mode attaches a passage for. Judged from the scenario's own
# line, so a switch that silently stayed in Slide mode fails here by name.
MODE_LINE="$(grep -a "\[SELFTEST-READING\] scenario=linestep " "$LOG" 2>/dev/null | tail -1)"
if ! printf '%s\n' "$MODE_LINE" | grep -qa "mode=scroll"; then
    echo "(no Scroll-mode line from the scenario: '${MODE_LINE:-none}')"
    grep -a "\[SELFTEST-READING\]\|SELFTEST-LINESTEP\] FAIL not-scroll" "$LOG" 2>/dev/null | head -5
    echo "LINE_STEP: FAIL nomode — the walk did not run in Scroll mode, so its presses were slide changes"
    rm -f "$WALL"
    exit 4
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
    # 0) the mode cannot change under the walk: a press that reads mode=slide is a
    #    slide change wearing a reading step's numbers.
    press_mode="$(get mode)"
    if [ -n "$press_mode" ] && [ "$press_mode" != "scroll" ]; then
        echo "  FAIL press=$press: the walk ran in '$press_mode' mode — the wall's › changes slides there, not lines"
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
echo "LINE_STEP: PASS $MOVED press(es) ($MOVED_BACK back up) in Scroll mode, one line a press, one passage, wall on the page, no skipped frame"
