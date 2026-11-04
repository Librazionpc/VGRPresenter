#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_table_walk_test.sh — walk a REAL Table passage with the
# reading step and check the three surfaces a reader is watching.
#
# The report behind this: reading a passage with Alt+Down, the monitor wall kept
# showing an older page (its paragraph number and its words) while the preview
# had moved on, and the step skipped. The synthetic-pick harness
# (run_line_step_test.sh) pins the arithmetic; THIS one pins the real thing: the
# app's own app-wide search, the real passage it opens, and what the wall, the
# pane and the step actually do as the walk crosses paragraph boundaries.
#
# WHAT IT DOES
#   1. launches with VGR_SELFTEST=1 + VGR_TABLEWALK_TEST=1
#      (and VGR_TABLEWALK_QUERY=<passage>, default "47-1100X"; prefix with
#      "bible:" to require and open a Scripture result, and "live:" to put it on air)
#      stdout+stderr -> build/table_walk_test.log
#   2. the scenario opens the app-wide search, takes the first result (the real
#      "search a citation, open it" path), stages the passage through the pane's
#      own play, switches the wall to SCROLL mode through its own button, then
#      presses the ordinary reading step 120 times, 700ms apart — past the glide's
#      600ms cap, so each press starts from the settled position, and far enough
#      to cross SEVERAL paragraph boundaries (a real sermon's paragraphs run 10–25
#      lines each).
#
#      THE WALK IS MODE-GATED NOW. Alt+Down / Alt+Up are gone: the reading step is
#      the wall's ‹ ›, and what those do is the wall's Slide/Scroll switch
#      (MonitorWall). Only Scroll puts a passage on the slide at all, so a run that
#      walks in Slide mode would measure a slide change and every number below
#      would be meaningless. The runner therefore requires the scenario's own
#      `[SELFTEST-READING] mode=scroll` line and a `mode=scroll` on every press,
#      and judges the switch's contract at the end: Slide mode takes the passage
#      off the slide (`slideFlow=0`), Scroll mode puts it back (`slideFlow>0`).
#   3. asserts, per press:
#        * abs (the position in the pick) moves +1.0 and never goes backwards;
#        * the page index never regresses;
#        * EVERY wall tile is NOT STALE — its own composition, as it draws it,
#          equals one composed now from the same slide and style (that is the
#          "the metadata is not updating the para values" / "the left is still
#          showing while the right has finished" check, content-free);
#        * every wall tile is still drawing the live passage (flow=true);
#        * the PANE's own reference (its "…Fellowship N" paragraph) never goes
#          backwards, and it really does move — at least twice over the walk,
#          which is the "several paragraph boundaries" part of the report;
#        * AT EVERY SUCH BOUNDARY THE WALL MOVED WITH IT: a press that turns the
#          pane to a new paragraph must also change the tile text (the reported
#          "the left is still showing while the right has finished"), so a wall
#          left behind on the old page fails the run;
#        * glideSkips == 0: no 16 ms sample of the position moved a whole line or
#          went the wrong way;
#        * TWO OUTPUTS, ONE SET OF WORDS: every press reads, for EACH output, the
#          passage character its own top line starts at (`gchar`, in that output's
#          own style grid) and how long one of its lines is (`gspan`). No output's
#          gchar may go backwards, the two outputs may not be further apart than
#          one wrap (the longer of the two lines), and the two grids must really
#          differ (`glines`) — otherwise the comparison proves nothing. That is
#          the "the displays are not going at the same speed, the left is still
#          showing while the right has finished" report, measured instead of
#          eyeballed.
#
#        * EACH TILE'S SLICE MATCHES THE SHARED GRID: `drawn` is the lines in the
#          visible text slice, not the whole passage. Compare it with `expected`
#          (the corresponding slice's line count), while `glines` records the
#          whole-passage grid used by the step.
#
# The walk REWINDS to the pick's first line before pressing (lineStep(-1) until it
# refuses), so the boundary sequence does not depend on which paragraph this
# machine's pane had selected — and the backward path is exercised too.
#
# Usage:  bash tools/selftest/run_table_walk_test.sh [wait-seconds] [query]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${SELFTEST_BUILD_DIR:-$ROOT/build}"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/table_walk_test.log"
WAIT_SECS="${1:-120}"
QUERY="${2:-47-1100X}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "TABLE_WALK: FAIL missing-app — $BUILD/$APP not found (build first)"
    exit 2
fi

# ONLY THIS BUILD'S INSTANCES, MATCHED BY THE EXECUTABLE'S OWN PATH — see the
# same block in run_line_step_test.sh: killing by name takes the operator's own
# app down with it, and a survivor appends its own walk to this run's file.
BUILD_WIN="$(cygpath -w "$BUILD" 2>/dev/null || printf '%s' "$BUILD")"
# INTO A FILE, NOT A PIPE. Called from $(app_pids) this used to hang the runner for
# ever after a good walk: PowerShell allocates a console, its conhost inherits the
# command-substitution pipe, and it outlives PowerShell — so the read never sees EOF
# (measured: flat CPU, no child processes, 25 minutes after the walk had finished).
app_pids() {
    local out
    out="$(mktemp)"
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | ForEach-Object { \$_.Id }" \
        > "$out" 2>/dev/null
    tr -d '\r' < "$out"
    rm -f "$out"
}
BUILD_WIN="x"; RUN_PID=0; EARLY_EXIT=0; echo "== REPLAY: judging the existing log =="
if [ "$EARLY_EXIT" = "1" ] && ! grep -qa "\[SELFTEST-TABLEWALK\] done" "$LOG" 2>/dev/null; then
    echo "TABLE_WALK: FAIL crashed — the app exited by itself after" \
         "$(grep -ac 'SELFTEST-TABLEWALK] press=.* moved=' "$LOG" 2>/dev/null) press(es)," \
         "before the scenario finished. Check Windows' Application Error log" \
         "(Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000}) for" \
         "the faulting module: this is a crash, not a stepper that refused to move."
    exit 7
fi

LINES="$(grep -a "\[SELFTEST-TABLEWALK\] press=.* moved=" "$LOG" 2>/dev/null)"
TILES="$(mktemp)"
PANE="$(mktemp)"
grep -a "\[SELFTEST-TABLEWALK\] press=.* tile=" "$LOG" 2>/dev/null > "$TILES"
grep -a "\[SELFTEST-TABLEWALK\] press=.* pane=" "$LOG" 2>/dev/null > "$PANE"

# ---- FIELD EXTRACTION WITHOUT FORKING --------------------------------------
# This block used to be `printf | sed` per field AND a `grep` per press, inside a
# loop over every tile of every press: 605 tile lines x ~10 forks = ~6 000 process
# creations, and on this machine (MSYS tools under Windows) each one costs ~10ms,
# so the JUDGING alone took longer than the walk. Measured: 45 s of `bash -x` got
# as far as press 6 of 121 — it read as a hang ("the runner wedges after the walk")
# and the verdict never printed. The same numbers are now read in-process: one
# regex per field, and the per-press lookups are indexed once up front.
#
# field <name> <line> -> sets FIELD to the value of `name=<value>` on that line
# (a value runs to the next space, which is what every judged field is: numbers,
# booleans, or an enum like `s2`).
FIELD=""
field() {
    FIELD=""
    [[ $2 =~ (^|[[:space:]])$1=([^[:space:]]*) ]] && FIELD="${BASH_REMATCH[2]}"
    return 0
}
# press=<n> -> the tile lines / the pane line logged for that press, gathered in
# one pass each instead of one grep per press.
declare -A TILES_BY_PRESS=() PANE_BY_PRESS=()
index_by_press() {   # file, assoc-array name
    local file="$1" l key
    while IFS= read -r l; do
        [ -n "$l" ] || continue
        key=""
        [[ $l =~ press=([0-9]+) ]] && key="${BASH_REMATCH[1]}"
        [ -n "$key" ] || continue
        if [ "$2" = "TILES_BY_PRESS" ]; then
            TILES_BY_PRESS["$key"]="${TILES_BY_PRESS[$key]:-}${l}"$'\n'
        else
            PANE_BY_PRESS["$key"]="${PANE_BY_PRESS[$key]:-}${l}"$'\n'
        fi
    done < "$file"
}
index_by_press "$TILES" TILES_BY_PRESS
index_by_press "$PANE" PANE_BY_PRESS
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-TABLEWALK press lines — see $(basename "$LOG"))"
    grep -a "\[SELFTEST-TABLEWALK\]" "$LOG" 2>/dev/null | head -5
    echo "TABLE_WALK: FAIL missing"
    rm -f "$TILES" "$PANE"
    exit 3
fi

# SCROLL MODE IS THE WALK'S PRECONDITION (see the header): without it the
# scenario pressed the wall's › in Slide mode, which steps slides. The scenario
# says which way it ran, so this can be judged instead of assumed.
MODE_LINE="$(grep -a "\[SELFTEST-READING\] scenario=tablewalk " "$LOG" 2>/dev/null | tail -1)"
if ! printf '%s\n' "$MODE_LINE" | grep -qa "mode=scroll"; then
    echo "(no Scroll-mode line from the scenario: '${MODE_LINE:-none}')"
    grep -a "\[SELFTEST-READING\]\|SELFTEST-TABLEWALK\] FAIL not-scroll" "$LOG" 2>/dev/null | head -5
    echo "TABLE_WALK: FAIL nomode — the walk did not run in Scroll mode, so its steps were slide changes"
    rm -f "$TILES" "$PANE"
    exit 4
fi

# EVERY FAILURE LEAVES ITS OWN EVIDENCE. The first run of this check reported a
# single finding whose message scrolled out of the terminal before it could be
# read, and the passing runs after it overwrote the log — so the one observation
# that mattered was unreproducible by hand. A finding now appends the suspect
# press AND its neighbours (what the wall drew, what the pane said) to a file
# that survives later runs, so "it failed once" is always diagnosable.
FINDINGS="$BUILD/table_walk_findings.log"
rm -f "$FINDINGS"
dump_ctx() {
    local press="$1"; shift
    {
        echo "--- press=$press $*"
        grep -a "press=$press " "$LOG" 2>/dev/null
        grep -a "press=$((press - 1)) " "$LOG" 2>/dev/null
        grep -a "press=$((press + 1)) " "$LOG" 2>/dev/null
    } >> "$FINDINGS"
}

FAILS=0
GRID_MISMATCH=0
MOVED=0
PREV_ABS=""
PREV_PAGE=""
PREV_PARA=""
PARA_MOVES=0
PREV_TILETEXT=""
TILE_TEXT_MOVES=0
TILE_READS=0

# ---- TWO OUTPUTS, TWO STYLES, ONE SET OF WORDS ----------------------------
#
# The report this whole exercise started from is a comparison between the two
# outputs: "the displays or output are not going at the same speed, the left is
# still showing while the right has finished, and the metadata is not updating".
# A line number cannot be compared between two styles — "line 7" is a different
# sentence in a grid that wraps at a different type size — so the reading
# position is now a CHARACTER OFFSET into the passage, converted per surface.
# These three numbers are what makes that checkable from the outside:
#
#   * gpchar = the passage character the output's own top line starts at, in the
#     PICK's coordinate (its own grid through the same conversion the surface
#     draws with, plus the run's own origin). The run's window slides as the walk
#     goes — which moves the run-relative offset backwards through no fault of the
#     picture — so the pick-wide number is the only one that can be compared
#     across presses;
#   * gspan  = the longest line in that grid — one wrap, the most a difference
#     between two grids can legitimately be;
#   * glines = how many lines that style makes of the passage, the evidence that
#     the two outputs really are laying it out differently.
#
# Failing conditions: an output whose gpchar goes BACKWARDS (it is drawing an
# earlier line than the press before), and two outputs more than one wrap apart
# (they are not on the same words), which is the "not the same speed" report.
GTILE_CHAR=(); GTILE_LOCAL=(); GTILE_SPAN=(); GTILE_LINES=(); GTILE_STYLE=()
PREV_GCHAR0=""; PREV_GCHAR1=""; PREV_LOCAL0=""; PREV_LOCAL1=""
MAX_SPREAD=0; SPREAD_AT=""
GRID_PROOF=""; GRID_SAME=1; TWO_STYLE_PRESSES=0
gchar_check() {   # idx now prev press
    local idx="$1" now="$2" prev="$3" press="$4"
    [ -n "$prev" ] || return 0
    [ "$now" -ge "$prev" ] && return 0
    echo "  FAIL press=$press: output $idx went BACKWARDS in the passage ($prev -> $now chars) — it is drawing an earlier line than the press before"
    dump_ctx "$press" "style-char-backwards tile=$idx $prev->$now"
    FAILS=$((FAILS + 1))
}
# The same, for the run-relative number: it is ALLOWED to drop when the run's
# window slides, but only by exactly what the window dropped. Reported, never
# failed — the pick-wide number above is the one that must be monotonic, and a
# gap between the two would mean the window moved without the picture moving with
# it (the words would jump by a page).
RUN_REBASE_MOVES=0
while IFS= read -r line; do
    # no forks here — see the FIELD EXTRACTION note above; the per-press timings
    # are only this script's own `date` deltas, nothing to resolve.
    field press      "$line"; press="$FIELD"
    field moved      "$line"; moved="$FIELD"
    field abs        "$line"; absv="$FIELD"
    field page       "$line"; page="$FIELD"
    field glideSkips "$line"; skips="$FIELD"
    field mode       "$line"; mode="$FIELD"
    # THE WALK ITSELF MUST STAY IN SCROLL MODE. A press that reads mode=slide is a
    # slide change reported as a reading step — the mode switched under the walk
    # (the pane adopts a per-tab value), so the sequence is not one walk any more.
    if [ -n "$mode" ] && [ "$mode" != "scroll" ]; then
        echo "  FAIL press=$press: the walk ran in '$mode' mode — the wall's › changes slides there, not lines"
        dump_ctx "$press" "walk-mode $mode"
        FAILS=$((FAILS + 1))
    fi
    PARA_NOW=""
    TILE_TEXT=""
    if [ -n "$skips" ] && [ "$skips" != "0" ]; then
        echo "  FAIL press=$press: $skips skipped frame(s) in the glide"
        dump_ctx "$press" "skipped-frames"
        FAILS=$((FAILS + 1))
    fi
    # the pane's own reference paragraph (the value the report says never moved)
    pane="${PANE_BY_PRESS[$press]:-}"
    if [ -z "$pane" ] || [[ $pane == *pane=missing* ]]; then
        echo "  FAIL press=$press: no pane to read (selfTestTablePane not found)"
        dump_ctx "$press" "pane-missing"
        FAILS=$((FAILS + 1))
    else
        field para "$pane"; para="$FIELD"
        PARA_NOW="$para"
        if [ -n "$PREV_PARA" ] && [ "$para" != "-1" ] && [ "$PREV_PARA" != "-1" ]; then
            if [ "$para" -lt "$PREV_PARA" ]; then
                echo "  FAIL press=$press: the pane's paragraph went backwards ($PREV_PARA -> $para)"
                dump_ctx "$press" "paragraph-backwards $PREV_PARA->$para"
                FAILS=$((FAILS + 1))
            elif [ "$para" -gt "$PREV_PARA" ]; then
                PARA_MOVES=$((PARA_MOVES + 1))
            fi
        fi
        PREV_PARA="$para"
    fi
    # the wall: every tile's own composition against one composed now
    tiles="${TILES_BY_PRESS[$press]:-}"
    if [ -z "$tiles" ] || [[ $tiles == *wall=missing* ]]; then
        echo "  FAIL press=$press: no wall tiles read (selfTestMonitorWall not found)"
        dump_ctx "$press" "wall-missing"
        FAILS=$((FAILS + 1))
    else
        while IFS= read -r tl; do
            [ -n "$tl" ] || continue
            TILE_READS=$((TILE_READS + 1))
            # the one field with spaces in it: cut between its own quotes
            TILE_TEXT=""
            case "$tl" in
                *tileText=\"*\"*) _rest="${tl#*tileText=\"}"; TILE_TEXT="${_rest%%\"*}" ;;
            esac
            field stale "$tl"; stale="$FIELD"
            field flow  "$tl"; flow="$FIELD"
            if [ "$stale" != "false" ]; then
                echo "  FAIL press=$press: a wall tile is STALE — $tl"
                dump_ctx "$press" "stale-tile $tl"
                FAILS=$((FAILS + 1))
            fi
            if [ "$flow" != "true" ]; then
                echo "  FAIL press=$press: a wall tile stopped drawing the passage — $tl"
                dump_ctx "$press" "tile-off-passage $tl"
                FAILS=$((FAILS + 1))
            fi
            field tile "$tl"; tidx="$FIELD"
            [ -n "$tidx" ] || continue
            field gpchar "$tl"; GTILE_CHAR[$tidx]="$FIELD"
            field gchar  "$tl"; GTILE_LOCAL[$tidx]="$FIELD"
            field gspan  "$tl"; GTILE_SPAN[$tidx]="$FIELD"
            field glines "$tl"; GTILE_LINES[$tidx]="$FIELD"
            field style  "$tl"; GTILE_STYLE[$tidx]="$FIELD"
            # A tile lays out only its visible slice; compare it with that slice's
            # expected line count. `glines` is the whole-passage table, so comparing
            # these directly confuses a viewport slice with the full passage.
            field drawn    "$tl"; drawn="$FIELD"
            field expected "$tl"; expected="$FIELD"
            if [ -n "$drawn" ] && [ "$drawn" -gt 0 ] && [ -n "$expected" ] \
                    && [ "$expected" -gt 0 ] && [ "$drawn" -ne "$expected" ]; then
                echo "  FAIL press=$press: output $tidx draws $drawn line(s) in its slice; expected $expected"
                dump_ctx "$press" "slice-grid-mismatch tile=$tidx drawn=$drawn expected=$expected"
                GRID_MISMATCH=$((GRID_MISMATCH + 1))
                FAILS=$((FAILS + 1))
            fi
        done <<EOF
$tiles
EOF
    fi
    # the two outputs against each other, at this same instant
    g0="${GTILE_CHAR[0]:-}"; g1="${GTILE_CHAR[1]:-}"
    if [ -n "$g0" ] && [ -n "$g1" ] && [ "$g0" -ge 0 ] && [ "$g1" -ge 0 ]; then
        TWO_STYLE_PRESSES=$((TWO_STYLE_PRESSES + 1))
        if [ -z "$GRID_PROOF" ]; then
            GRID_PROOF="styles ${GTILE_STYLE[0]}/${GTILE_STYLE[1]} make ${GTILE_LINES[0]}/${GTILE_LINES[1]} lines of the passage (longest ${GTILE_SPAN[0]}/${GTILE_SPAN[1]} chars)"
        fi
        [ "$g0" = "$g1" ] && [ "${GTILE_LINES[0]}" = "${GTILE_LINES[1]}" ] || GRID_SAME=0
        gchar_check 0 "$g0" "$PREV_GCHAR0" "$press"
        gchar_check 1 "$g1" "$PREV_GCHAR1" "$press"
        # A WINDOW SLIDE, not a jump: the RUN-relative offset is allowed to drop —
        # the run's text lost its front at that same instant — and it is counted so
        # the summary shows how often the walk crossed one. A drop with NO forward
        # advance of the pick-wide number would be the picture moving backwards.
        for t in 0 1; do
            lnow="${GTILE_LOCAL[$t]:-}"; lprev="$PREV_LOCAL0"
            [ "$t" = "1" ] && lprev="$PREV_LOCAL1"
            if [ -n "$lnow" ] && [ -n "$lprev" ] && [ "$lnow" -lt "$lprev" ]; then
                RUN_REBASE_MOVES=$((RUN_REBASE_MOVES + 1))
            fi
        done
        PREV_LOCAL0="${GTILE_LOCAL[0]:-}"
        PREV_LOCAL1="${GTILE_LOCAL[1]:-}"
        # A FORWARD JUMP on one output alone — the "the left skipped while the right
        # was smooth" report. A press advances about one line, so more than two of
        # that output's own longest lines in one press is a skip, not a step.
        if [ "$moved" = "true" ] && [ -n "$PREV_GCHAR0" ] && [ "$g0" -gt "$PREV_GCHAR0" ] \
           && [ "$g0" -gt "$((PREV_GCHAR0 + 2 * ${GTILE_SPAN[0]:-0}))" ]; then
            echo "  FAIL press=$press: output 0 jumped $((g0 - PREV_GCHAR0)) characters in one press (more than two of its own lines) — it skipped words the pane did not"
            dump_ctx "$press" "style-forward-jump tile=0 $PREV_GCHAR0->$g0"
            FAILS=$((FAILS + 1))
        fi
        if [ "$moved" = "true" ] && [ -n "$PREV_GCHAR1" ] && [ "$g1" -gt "$PREV_GCHAR1" ] \
           && [ "$g1" -gt "$((PREV_GCHAR1 + 2 * ${GTILE_SPAN[1]:-0}))" ]; then
            echo "  FAIL press=$press: output 1 jumped $((g1 - PREV_GCHAR1)) characters in one press (more than two of its own lines) — it skipped words the pane did not"
            dump_ctx "$press" "style-forward-jump tile=1 $PREV_GCHAR1->$g1"
            FAILS=$((FAILS + 1))
        fi
        if [ "$g0" -gt "$g1" ]; then spread=$((g0 - g1)); else spread=$((g1 - g0)); fi
        bound="${GTILE_SPAN[0]:-0}"
        [ "${GTILE_SPAN[1]:-0}" -gt "$bound" ] && bound="${GTILE_SPAN[1]}"
        if [ "$spread" -gt "$MAX_SPREAD" ]; then MAX_SPREAD="$spread"; SPREAD_AT="$press"; fi
        # More than one wrap apart is not a wrap difference any more: the two
        # outputs are on different words, which is the reported "not going at the
        # same speed" (it used to be a whole page, then a line or two, per press).
        if [ "$spread" -gt "$bound" ]; then
            echo "  FAIL press=$press: the two outputs are $spread characters apart — more than one wrap (longest line $bound) — they are not on the same words"
            dump_ctx "$press" "style-drift spread=$spread bound=$bound gchar0=$g0 gchar1=$g1"
            FAILS=$((FAILS + 1))
        fi
        PREV_GCHAR0="$g0"
        PREV_GCHAR1="$g1"
    fi
    # THE PARAGRAPH BOUNDARY, ON BOTH SURFACES AT ONCE. The pane turning to the
    # next paragraph is only half the report ("the left is still showing while the
    # right has finished"): the wall has to be drawing that paragraph's words too.
    # Before the fix the wall stayed on the page it was pinned to while the pane
    # walked on, which is exactly a paragraph advance with unchanged tile text.
    if [ -n "$TILE_TEXT" ]; then
        if [ -n "$PREV_TILETEXT" ] && [ "$TILE_TEXT" != "$PREV_TILETEXT" ]; then
            TILE_TEXT_MOVES=$((TILE_TEXT_MOVES + 1))
        fi
        if [ -n "$PARA_NOW" ] && [ "$PARA_NOW" != "-1" ] && [ -n "$PREV_PARA" ] \
           && [ "$PREV_PARA" != "-1" ] && [ "$PARA_NOW" -gt "$PREV_PARA" ]; then
            if [ "$TILE_TEXT" = "$PREV_TILETEXT" ]; then
                echo "  FAIL press=$press: the pane moved to paragraph $PARA_NOW but the wall still draws paragraph $PREV_PARA's words — tile "
                echo "$TILE_TEXT"
                dump_ctx "$press" "pane-advanced-wall-did-not $PREV_PARA->$PARA_NOW tile=$TILE_TEXT"
                FAILS=$((FAILS + 1))
            fi
        fi
        PREV_TILETEXT="$TILE_TEXT"
    fi
    [ "$moved" = "true" ] || continue
    MOVED=$((MOVED + 1))
    if [ -n "$PREV_ABS" ]; then
        awk -v a="$PREV_ABS" -v b="$absv" 'BEGIN { x = b - a; exit !(x > 0.99 && x < 1.01) }' || {
            echo "  FAIL press=$press: position went $PREV_ABS -> $absv (not one line)"
            dump_ctx "$press" "position-jump $PREV_ABS->$absv"
            FAILS=$((FAILS + 1))
        }
    fi
    if [ -n "$PREV_PAGE" ]; then
        awk -v a="$PREV_PAGE" -v b="$page" 'BEGIN { exit !(b >= a) }' || {
            echo "  FAIL press=$press: page regressed ($PREV_PAGE -> $page)"
            dump_ctx "$press" "page-regressed $PREV_PAGE->$page"
            FAILS=$((FAILS + 1))
        }
    fi
    PREV_ABS="$absv"
    PREV_PAGE="$page"
done <<EOF
$LINES
EOF

echo "== presses =="
printf '%s\n' "$LINES"
if [ "$FAILS" -ne 0 ] && [ -f "$FINDINGS" ]; then
    echo "== the failing presses, with their neighbours ($(basename "$FINDINGS")) =="
    cat "$FINDINGS"
fi
# ---- THE SWITCH'S CONTRACT, judged from the scenario's own photographs ------
# Turned OFF the passage must leave the slide the outputs draw (Slide mode: the
# arrows change slides over the page as authored); turned back ON it must return.
# `slideFlow` is the length of the flowText the SLIDE carries — the field the
# engine raster, the monitor tiles and the NDI feed all scroll from — so a switch
# that only repaints its own button fails here.
MODE_SLIDE="$(grep -a "\[SELFTEST-TABLEWALK\] mode=slide " "$LOG" 2>/dev/null | tail -1)"
MODE_SCROLL="$(grep -a "\[SELFTEST-TABLEWALK\] mode=scroll " "$LOG" 2>/dev/null | tail -1)"
MODE_SLIDE_FLOW="$(printf '%s\n' "$MODE_SLIDE" | sed -n 's/.* slideFlow=\([0-9]*\).*/\1/p')"
MODE_SCROLL_FLOW="$(printf '%s\n' "$MODE_SCROLL" | sed -n 's/.* slideFlow=\([0-9]*\).*/\1/p')"
MODE_FAILS=0
if [ -z "$MODE_SLIDE_FLOW" ] || [ -z "$MODE_SCROLL_FLOW" ]; then
    echo "  FAIL the switch never reported both modes (slide='${MODE_SLIDE}' scroll='${MODE_SCROLL}')"
    MODE_FAILS=$((MODE_FAILS + 1))
else
    if [ "$MODE_SLIDE_FLOW" != "0" ]; then
        echo "  FAIL Slide mode still draws a passage on the slide (slideFlow=$MODE_SLIDE_FLOW) — the arrows would scroll where they should change slides"
        MODE_FAILS=$((MODE_FAILS + 1))
    fi
    if [ "$MODE_SCROLL_FLOW" -le 0 ]; then
        echo "  FAIL Scroll mode left the slide without a passage (slideFlow=$MODE_SCROLL_FLOW) — the arrows would have nothing to scroll"
        MODE_FAILS=$((MODE_FAILS + 1))
    fi
fi
FAILS=$((FAILS + MODE_FAILS))

echo "== walk state =="
cat "$PANE"
echo "== the mode switch =="
echo "  $(printf '%s\n' "$MODE_LINE")"
echo "  slide : ${MODE_SLIDE:-missing}"
echo "  scroll: ${MODE_SCROLL:-missing}"
echo "== the two outputs, side by side =="
echo "  ${GRID_PROOF:-no press read both outputs}"
echo "  presses with both outputs read: $TWO_STYLE_PRESSES, furthest apart: $MAX_SPREAD char(s) (press ${SPREAD_AT:-none}) — one wrap is allowed, more is drift"
echo "  run-window slides crossed: $RUN_REBASE_MOVES (the run-relative offset drops there by design; the pick-wide one above must not)"
rm -f "$TILES" "$PANE"
echo "== moved presses: $MOVED, wall tile reads: $TILE_READS, wall text changes: $TILE_TEXT_MOVES, pane paragraph advances: $PARA_MOVES, drawn-vs-slice grid mismatches: $GRID_MISMATCH, findings: $FAILS =="
if [ "$MOVED" -lt 15 ]; then
    echo "TABLE_WALK: FAIL too-few-moves ($MOVED) — the walk stopped early"
    exit 1
fi
if [ "$TILE_READS" -eq 0 ]; then
    echo "TABLE_WALK: FAIL no-wall — nothing read the monitor tiles"
    exit 1
fi
# ONE paragraph boundary is enough here, and the query is why: the default book
# (47-1100X) is a short passage that spans Fellowship 1 and 2 — two paragraphs — so
# the pane's own reference number advances exactly once over any full walk of it.
# The check that made this number matter is the PER-PRESS one above ("the pane moved
# to paragraph N but the wall still draws the old paragraph's words"); this gate only
# says the walk reached the boundary at all. A longer query still exercises the
# several-boundary case: run it with a multi-section passage and PARA_MOVES follows.
if [ "$PARA_MOVES" -lt 1 ]; then
    echo "TABLE_WALK: FAIL no-paragraph-advance ($PARA_MOVES) — the walk never crossed a paragraph boundary"
    exit 1
fi
if [ "$TWO_STYLE_PRESSES" -lt 15 ]; then
    echo "TABLE_WALK: FAIL no-two-style ($TWO_STYLE_PRESSES) — no press read two outputs, so their words were never compared"
    exit 1
fi
if [ "$GRID_SAME" -eq 1 ]; then
    echo "TABLE_WALK: FAIL one-grid — both outputs laid the passage out identically ($GRID_PROOF), so this run proves nothing about two different type sizes"
    exit 1
fi
if [ "$FAILS" -ne 0 ]; then
    echo "TABLE_WALK: FAIL findings=$FAILS"
    exit 1
fi
echo "TABLE_WALK: PASS $MOVED press(es) on a real passage in Scroll mode, $TILE_READS tile reads (none stale, all drawing the passage), pane advanced through $PARA_MOVES paragraph(s), no skipped frame, two outputs within $MAX_SPREAD char(s) of each other ($GRID_PROOF), switch slide/scroll slideFlow=$MODE_SLIDE_FLOW/$MODE_SCROLL_FLOW"
