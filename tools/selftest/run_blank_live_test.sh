#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_blank_live_test.sh — GO LIVE with NOTHING staged, over a
# live camera/screen take ("the moment it is only media it doesn't go").
#
# WHY THIS ONE EXISTS
#   Pushing scripture or table content renders fine, but GO LIVE with only a
#   taken input (or media on air) and NOTHING else made every real output —
#   the monitor wall's engine frame, the output window and the NDI feed — come
#   out as an opaque BLACK frame, while the monitor tile (which draws the tap
#   itself) looked right. GO LIVE-with-nothing sends ONE blank carrier slide
#   (LiveOutputService::goLiveBlank: no blocks, no title, no text), so the only
#   part of the picture the operator asked for is the compositor's video layer
#   over the active style's background.
#
#   The cause was on the ENGINE side of a rule the UI side already had: a slide
#   with no content at all must not wear the style's TEMPLATE. A template's
#   decorative blocks (a full-stage background slab, bands) are authored to sit
#   over the slide's TEXT; with no text they are pure cover, and they are painted
#   in the TEXT pass — above the compositor's video layer — so the live picture
#   was buried (4.86's "go live with nothing shows a black window" was the same
#   defect on the tile, where it was already fixed). SceneBuilder now skips the
#   style's template when the slide has no blocks, no title and no text.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 VGR_BLANKLIVE_TEST=1 (stdout+stderr
#      -> build/blank_live_test.log). The Main.qml scenario takes the first real
#      captured window (not this app's own), then presses GO LIVE — which lands
#      on the blank carrier because this run opens no show — waits, then pushes a
#      real CONTENT slide through the same live session (the operator's own
#      styles wear templates with full-stage slabs, which is the state that used
#      to bury the feed), and quits.
#   2. reads the ENGINE log (build/logs/engine.log) across BOTH phases, marked by
#      "live output started from content" (the blank carrier) and "live output
#      replaced with content" (the content slide), and checks the per-output
#      frame sanity line RenderOutputs logs on every state change:
#        * at least one live frame reported in each phase (the loop really
#          rasterized), and
#        * the LAST one per output is not all-black in either phase.
#      A black frame here is the engine's own pixels — the same pixels the
#      output window, HDMI and the NDI sender consume — so it needs no
#      screenshot to be trustworthy.
#   3. ALSO checks WHAT the content phase was composed of, because "not black"
#      alone cannot tell "the feed is over the design's backdrop" from "a
#      backdrop is over the feed": in the content phase the engine must report
#      the feed actually IN the built scene (SceneBuilder's "taken input layer
#      present") AND both halves of the split its rule produced ("backdrop layer
#      <n> of <m> blocks under the video layer (stage covered: yes|no)"). Paint
#      that REACHES the stage must be under the video layer (n >= 1) while the
#      feed sits ON it — the layer order that makes the feed the output's
#      picture. Both lines come from the engine that composes the frame, so this
#      holds for every consumer at once (monitor wall, output window, HDMI, NDI).
#      This is the check that catches a cover-up on the designs whose slab is
#      TRANSLUCENT (a dimmed feed is not a black one — the app's own designs are
#      built that way), which the frame check alone cannot see.
#
# VERDICT (last line, greppable):
#   BLANK_LIVE: PASS          — both phases flowed frames, no output's latest
#                               frame was black in either, and the content phase
#                               really had the feed over the design's backdrop
#   BLANK_LIVE: FAIL nolive   — the scenario never went live, or the content
#                               phase never landed (premise missing)
#   BLANK_LIVE: FAIL nopremise— a phase ran but the engine never said what it
#                               was built from (no feed in the content scene, or
#                               no backdrop split reported): nothing was proven
#   BLANK_LIVE: FAIL noframe  — no frame sanity line in a phase that ran
#   BLANK_LIVE: FAIL black    — a live output's latest frame is all black
#   BLANK_LIVE: FAIL backdrop — the design's paint covers the stage yet was left
#                               as content, i.e. painted over the live feed
#
# NOTE: takes a real screen/window input for the length of the run (the
# scenario clears it before quitting) and mutates NOTHING else.
#
# Usage:  bash tools/selftest/run_blank_live_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/blank_live_test.log"
ENGINE_LOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-45}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "BLANK_LIVE: FAIL nolive — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_BLANKLIVE_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_BLANKLIVE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself; poll for that rather than always sleeping
# the whole budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-BLANKLIVE\] done" "$LOG" 2>/dev/null; then
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

echo "== scenario output =="
LINES="$(grep -a "\[SELFTEST-BLANKLIVE\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-BLANKLIVE lines — the driver never armed; see $LOG)"
    echo "BLANK_LIVE: FAIL nolive"
    exit 3
fi
echo "$LINES"

# THE PREMISE: the run really went live (a GO LIVE that refused, or an abort,
# would make every check below vacuous).
TOOK="$(printf '%s\n' "$LINES" | grep "take label=" | tail -1)"
if echo "$TOOK" | grep -q 'taken=""' || [ -z "$TOOK" ]; then
    echo "== no camera/screen source was taken — cannot exercise the media-only case =="
    echo "$TOOK"
    echo "BLANK_LIVE: FAIL nolive — no input was taken (this machine had no capturable window)"
    exit 4
fi
GOLIVE="$(printf '%s\n' "$LINES" | grep "goLive live=" | tail -1)"
if ! echo "$GOLIVE" | grep -q "live=true"; then
    echo "== GO LIVE never took: the outputs are not on air, nothing to measure =="
    echo "$GOLIVE"
    echo "BLANK_LIVE: FAIL nolive"
    exit 5
fi

# The engine's own live lines are the markers: frame lines BEFORE the first one
# belong to the idle preview / previous session and must not decide this verdict.
#
# PHASE 1's marker is the BLANK CARRIER — a live START whose title is empty
# (LiveOutputService::goLiveBlank's carrier; this run opens no show, so going
# live with nothing staged is exactly that).
LIVE_LINE="$(grep -an "live output started from content: ''" "$ENGINE_LOG" 2>/dev/null | tail -1 | cut -d: -f1)"
if [ -z "$LIVE_LINE" ]; then
    echo "== the engine never reported the blank carrier going live (see $ENGINE_LOG) =="
    echo "BLANK_LIVE: FAIL nolive"
    exit 6
fi

# PHASE 2's marker is the CONTENT SLIDE, by ITS OWN TITLE — and deliberately not
# by the verb. Pushing content while the carrier is on air usually REPLACES it,
# but the live session can also restart in between (an output roster change
# makes the controller stop and start again, see "live output stopped" in the
# log), which turns the same push into a fresh START. Both land the content on
# air through the same chain of outputs, which is what these checks measure, so
# both are accepted: keying on "replaced" alone reported a whole phase as
# missing on the runs where the platform had restarted the session.
#
# FIRST match after the carrier, so a carrier start can never answer for it.
CONTENT_TITLE="'Blank Live Probe'"
CONTENT_LINE="$(awk -v floor="$LIVE_LINE" -v title="$CONTENT_TITLE" \
    'NR >= floor && index($0, "live output") && index($0, title) { print NR; exit }' \
    "$ENGINE_LOG" 2>/dev/null)"
if [ -z "$CONTENT_LINE" ] || [ "$CONTENT_LINE" -le "$LIVE_LINE" ]; then
    echo "== the content phase never reached the engine (no live line carrying it) =="
    echo "BLANK_LIVE: FAIL nolive — the run only exercised the blank carrier"
    exit 9
fi

# check_phase <first-line> <last-line> <label> — every output's LATEST frame in
# this slice must be non-black. An output can legitimately be black for one frame
# while its own per-output pass warms up; a feed that STAYS black is the defect.
# The slice is bounded on BOTH ends: phase 1 must not be judged by the frames the
# content phase wrote afterwards (that would hide a black blank carrier).
check_phase() {
    local from="$1" to="$2" label="$3"
    local frames
    frames="$(sed -n "${from},${to}p" "$ENGINE_LOG" | grep -a "output frame sanity")"
    if [ -z "$frames" ]; then
        echo "== $label: no output frame sanity line — the loop never rasterized =="
        return 7
    fi
    echo "== $label frame sanity =="
    printf '%s\n' "$frames" | sed 's/^/  /'
    local black="" name last
    for name in $(printf '%s\n' "$frames" | sed -n "s/.*sanity: '\([^']*\)'.*/\1/p" | sort -u); do
        last="$(printf '%s\n' "$frames" | grep -a "sanity: '$name'" | tail -1)"
        if echo "$last" | grep -q "allBlack=yes"; then
            black="$black
$last"
        fi
    done
    if [ -n "$black" ]; then
        echo "== $label: a live output is all black =="
        printf '%s\n' "$black"
        return 8
    fi
    return 0
}

check_phase "$LIVE_LINE" "$((CONTENT_LINE - 1))" "blank carrier (media only)" || {
    echo "BLANK_LIVE: FAIL black — the engine rasterized an all-black frame on the blank carrier: it must NOT wear the style's template (its decorative blocks are painted above the compositor's video layer)"
    exit 8
}
check_phase "$CONTENT_LINE" "\$" "content + the style's template" || {
    echo "BLANK_LIVE: FAIL black — the engine rasterized an all-black frame with content on air: a template's full-stage slab belongs UNDER the compositor's video layer, because a live feed is the output's picture"
    exit 10
}

# --- WHAT the content phase was COMPOSED of --------------------------------
# "Not black" cannot tell the two z-orders apart (a translucent backdrop over
# the feed is not black either), so ask the engine that composes the frame.
#
# Two bounds, for two different mistakes:
#  * THIS RUN's slice starts at its own "live output started from content" (the
#    blank carrier) — engine.log is appended to across runs, and a content-phase
#    scene id is stable run to run, so without the floor a PREVIOUS run's line
#    answers for this one.
#  * Selected by SCENE ID, not by line position: the content phase's own build
#    can land a few lines BEFORE the "live output replaced with content" marker
#    (the scene is built, then handed to the outputs), so a slice starting at
#    that marker silently misses it and reports a false premise. The content
#    phase's scene id carries the scenario's slide title —
#    `selfTestBlankLiveContent` in qml/Main.qml pushes the slide "Blank Live
#    Probe" — while the blank carrier's is `pres:temp::temp:-1…` (no
#    presentation), so this pattern names the content phase exactly.
RUN_SLICE="$(sed -n "${LIVE_LINE},\$p" "$ENGINE_LOG")"
CONTENT_SCENE="pres:temp:Blank Live Probe:"
FEED="$(printf '%s\n' "$RUN_SLICE" | grep -a "taken input layer present" | grep -a "$CONTENT_SCENE" | tail -1)"
if [ -z "$FEED" ]; then
    echo "== the content phase's scenes were built with NO feed in them — the case under test was not exercised =="
    echo "BLANK_LIVE: FAIL nopremise — content phase had no taken-input layer"
    exit 11
fi
BACKDROP_LINES="$(printf '%s\n' "$RUN_SLICE" | grep -a "backdrop layer" | grep -a "$CONTENT_SCENE")"
if [ -z "$BACKDROP_LINES" ]; then
    echo "== the content phase never reported its backdrop split — SceneBuilder's rule did not run =="
    echo "BLANK_LIVE: FAIL nopremise — no backdrop layer line in the content phase"
    exit 12
fi
# EVERY content-phase scene is checked, not just the last: each output wears its
# own style, so one scene's design can cover the stage while another's does not,
# and a regression in either must not hide behind whichever line came last.
DEMOTED=""; TOTAL=""; COVERED=""; COVERING=0; SCENES=0; BURIED=""
while IFS= read -r line; do
    [ -z "$line" ] && continue
    d="$(printf '%s\n' "$line" | sed -n "s/.*backdrop layer \([0-9][0-9]*\) of \([0-9][0-9]*\) blocks.*/\1/p")"
    t="$(printf '%s\n' "$line" | sed -n "s/.*backdrop layer \([0-9][0-9]*\) of \([0-9][0-9]*\) blocks.*/\2/p")"
    c="$(printf '%s\n' "$line" | sed -n "s/.*stage covered: \(yes\|no\).*/\1/p")"
    if [ -z "$d" ] || [ -z "$t" ] || [ -z "$c" ]; then
        echo "== unparseable backdrop layer line: $line"
        echo "BLANK_LIVE: FAIL nopremise"
        exit 13
    fi
    SCENES=$((SCENES + 1))
    DEMOTED="$d"; TOTAL="$t"; COVERED="$c"
    if [ "$c" = "yes" ]; then
        COVERING=$((COVERING + 1))
        # THE DECISIVE PAIRING. The engine's line states both halves of the
        # rule's own reading of the design: whether its leading paint REACHES the
        # stage, and how much of that paint it placed on the scene's "bg" layer.
        # "under the video layer" is a claim about z-order, not a synonym for
        # "small": the scene's layer table is bg=0 < media=1 < text=2, so a
        # demoted block sits BELOW the feed the same line's scene carries — and
        # covering paint left as CONTENT (demoted = 0) is paint sitting ON TOP of
        # the live feed, which is the defect. The frame checks above are the other
        # half of the proof, not a substitute: the app's own designs cover the
        # stage with a TRANSLUCENT slab, so a buried feed there is a dimmed feed
        # rather than a black one, and this pairing is what catches that.
        if [ "$d" = "0" ]; then
            BURIED="$BURIED
$line"
        fi
    fi
done <<< "$BACKDROP_LINES"

echo "== content phase composition =="
echo "  $FEED"
printf '%s\n' "$BACKDROP_LINES" | sed 's/^/  /'
if [ -n "$BURIED" ]; then
    echo "== the design's paint covers the stage yet was left as content =="
    printf '%s\n' "$BURIED"
    echo "BLANK_LIVE: FAIL backdrop — the design's paint covers the stage but was left as content: it paints OVER the live feed instead of under it"
    exit 14
fi
# A design whose paint never reaches the stage is left as decoration OVER the
# feed — nothing buried, nothing to demote. Said out loud rather than passed
# over in silence, so a machine whose active design cannot exercise the burying
# case is visible in the verdict.
if [ "$COVERING" = "0" ]; then
    echo "NOTE: no content-phase design's paint reaches the stage, so this run did not exercise the slab-over-feed case"
fi

# --- THE TILE, read off its own layer stack --------------------------------
# The engine half above proves what the OUTPUTS were composed of; this proves the
# monitor tile composes the SAME order — the design's backdrop paint BELOW the
# taken feed, the block content OVER it. The tile is a preview, so the only way
# it can know that is the engine's own answer (EngineBridge.backdropBlockIndexes
# -> SceneBuilder::PlanBlockLayers); a preview that decides for itself is how the
# operator gets one picture on the tile and another on air.
TILE="$(printf '%s\n' "$LINES" | grep "tile layers:" | tail -1)"
if [ -z "$TILE" ]; then
    echo "== the tile's layer-stack probe never reported =="
    echo "BLANK_LIVE: FAIL nopremise — no tile layers line"
    exit 15
fi
BL="$(printf '%s\n' "$TILE" | sed -n "s/.*backdrop\(-\{0,1\}[0-9][0-9]*\).*/\1/p")"
FL="$(printf '%s\n' "$TILE" | sed -n "s/.* feed\(-\{0,1\}[0-9][0-9]*\).*/\1/p")"
DL="$(printf '%s\n' "$TILE" | sed -n "s/.*design\(-\{0,1\}[0-9][0-9]*\).*/\1/p")"
if [ -z "$BL" ] || [ -z "$FL" ] || [ -z "$DL" ] || [ "$BL" -lt 0 ] || [ "$FL" -lt 0 ] \
   || [ "$DL" -lt 0 ]; then
    echo "== the tile's pane is missing a layer the split needs =="
    echo "$TILE"
    echo "BLANK_LIVE: FAIL nopremise — tile has no backdrop/feed/content layer (backdrop=$BL feed=$FL design=$DL)"
    exit 16
fi
echo "== tile layer stack =="
echo "  $TILE"
if [ "$BL" -ge "$FL" ] || [ "$FL" -ge "$DL" ]; then
    echo "BLANK_LIVE: FAIL tilelayers — the monitor tile does not compose the feed where the engine does: a design's backdrop paint must sit BELOW the taken input and the on-air content ABOVE it, so the tile shows the same picture every output does"
    exit 17
fi
echo "BLANK_LIVE: PASS backdropUnderVideo=$DEMOTED/$TOTAL stageCovered=$COVERED coveringScenes=$COVERING/$SCENES feedInScene=yes tileLayers=backdrop$BL<feed$FL<content$DL"
exit 0
