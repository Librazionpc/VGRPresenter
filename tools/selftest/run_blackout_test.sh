#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_blackout_test.sh — the wall toolbar's Blackout control,
# pressed for real: does it paint an EMPTY frame on every output, and does the
# second press put the content back?
#
# WHY THIS ONE EXISTS
#   "Nothing at all on screen" has to be a deliberate gesture. Clear all used to
#   send a `clearStage` carrier that painted nothing, and it was changed to keep
#   the output's own look instead (the branded backdrop and background image
#   survive a clear, which is what an operator clearing content expects) — so
#   for a while the app had NO control that could ask for a truly empty screen.
#   The wall toolbar's Blackout control is that control: it sends the clearStage
#   carrier on purpose, memory of what it hid included, and a second press
#   brings that content back without restarting the live session.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 VGR_BLACKOUT_TEST=1 (stdout+stderr
#      -> build/blackout_test.log). Main.qml puts ONE real slide on air, then
#      CLICKS the button (SelfTest.clickItem — a real click at the control, not a
#      service call), then clicks it again, and quits.
#   2. reads the ENGINE log (build/logs/engine.log) across the three phases the
#      engine marks itself:
#        A. live content 'Blackout Probe' on air
#        B. a live replacement with an EMPTY title (the blackout carrier)
#        C. the content back on air (the same title, again)
#      and checks the per-output frame sanity line RenderOutputs logs whenever a
#      presented frame changes:
#        * phase A ends with a NON-black frame on every output (the premise: there
#          was a picture to hide),
#        * phase B ends with an ALL-BLACK frame on every output — the carrier
#          really reached HDMI and the NDI feed, not just the app's tile,
#        * phase C ends with a NON-black frame on every output (the content is
#          back, through the same live session).
#      These are the engine's own pixels, the same ones the outputs consume, so
#      no screenshot is needed to trust them.
#   3. blackens again, then presses STOP, and checks the staged slot holds the
#      CONTENT the blackout was hiding (not the carrier): STOP must never make
#      the next GO LIVE put a black frame back on the screens.
#
# VERDICT (last line, greppable):
#   BLACKOUT: PASS                          — content -> empty frames -> content
#   BLACKOUT: FAIL nocontrol                — the button was not on screen/clickable
#   BLACKOUT: FAIL nolive                   — the scenario never went live
#   BLACKOUT: FAIL notpressed               — the click did not put the carrier up
#   BLACKOUT: FAIL noframes <phase>         — a phase ran but no output reported a frame
#   BLACKOUT: FAIL lit <phase>              — phase A/C ends on an all-black frame
#   BLACKOUT: FAIL notblack                 — the blackout phase is NOT empty on an output
#   BLACKOUT: FAIL noend                    — the second press did not bring content back
#   BLACKOUT: FAIL staged                   — STOP while blacked out staged the carrier, not the content
#   BLACKOUT: FAIL layers                   — the clear carrier still composites the taken input
#   BLACKOUT: FAIL nopremise                — no take, or the take never reached the content scene
#
# Usage:  bash tools/selftest/run_blackout_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/blackout_test.log"
ENGINE_LOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-60}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "BLACKOUT: FAIL nolive — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_BLACKOUT_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_BLACKOUT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself; poll for that rather than always sleeping
# the whole budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-BLACKOUT\] done" "$LOG" 2>/dev/null; then
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
LINES="$(grep -a "\[SELFTEST-BLACKOUT\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-BLACKOUT lines — the driver never armed; see $LOG)"
    echo "BLACKOUT: FAIL nolive"
    exit 3
fi
echo "$LINES"

TOOK="$(printf '%s\n' "$LINES" | grep "take label=" | tail -1)"
TAKEN="$(printf '%s\n' "$TOOK" | sed -n 's/.*taken="\([^"]*\)".*/\1/p')"
if [ -z "$TOOK" ]; then
    echo "== the scenario never reported its take attempt =="
    echo "BLACKOUT: FAIL nopremise"
    exit 18
fi
GOLIVE="$(printf '%s\n' "$LINES" | grep "live live=" | tail -1)"
if ! echo "$GOLIVE" | grep -q "live=true"; then
    echo "== the scenario never went live: there is no screen to blank =="
    echo "BLACKOUT: FAIL nolive"
    exit 4
fi
PRESSED="$(printf '%s\n' "$LINES" | grep "pressed visible=" | tail -1)"
if echo "$PRESSED" | grep -q "visible=false"; then
    echo "== the Blackout control was not on screen/laid out — the click proved nothing =="
    echo "BLACKOUT: FAIL nocontrol"
    exit 5
fi
if ! echo "$PRESSED" | grep -q "blackedOut=true"; then
    echo "== the click did not put the clearStage carrier up =="
    echo "$PRESSED"
    echo "BLACKOUT: FAIL notpressed"
    exit 6
fi
ENDED="$(printf '%s\n' "$LINES" | grep "ended blackedOut=" | tail -1)"
if ! echo "$ENDED" | grep -q "blackedOut=false"; then
    echo "== the second press did not come out of the blackout =="
    echo "$ENDED"
    echo "BLACKOUT: FAIL noend"
    exit 7
fi
REBLACKED="$(printf '%s\n' "$LINES" | grep "reblacked blackedOut=" | tail -1)"
if ! echo "$REBLACKED" | grep -q "blackedOut=true"; then
    echo "== the third press did not black out again (the STOP check has no premise) =="
    echo "$REBLACKED"
    echo "BLACKOUT: FAIL notpressed"
    exit 14
fi
# STOP WHILE BLACKED OUT leaves the HIDDEN content staged, never the carrier:
# the next GO LIVE must not put the black frame back on the screens.
STOPPED="$(printf '%s\n' "$LINES" | grep "stopped live=" | tail -1)"
if ! echo "$STOPPED" | grep -q "live=false" || ! echo "$STOPPED" | grep -q "stagedClearStage=false"; then
    echo "== STOP while blacked out did not stage the content the blackout was hiding =="
    echo "$STOPPED"
    echo "BLACKOUT: FAIL staged"
    exit 15
fi

# --- THE ENGINE'S THREE PHASES ---------------------------------------------
# Read BACKWARDS from the end of this run, because the content title appears
# twice (before the blackout and after it): the LAST title marker is the
# restoration, the last EMPTY-title marker before it is the blackout, and the
# last title marker before that is the content that was hidden. engine.log is
# appended to across runs, and this ordering makes every marker this run's own
# without a clock comparison.
TITLE="'Blackout Probe'"
C_LINE="$(grep -an "live output .*content: $TITLE" "$ENGINE_LOG" 2>/dev/null | tail -1 | cut -d: -f1)"
if [ -z "$C_LINE" ]; then
    echo "== the engine never reported the content coming back (see $ENGINE_LOG) =="
    echo "BLACKOUT: FAIL noend"
    exit 8
fi
B_LINE="$(grep -an "live output .*content: ''" "$ENGINE_LOG" 2>/dev/null \
    | awk -F: -v ceiling="$C_LINE" '$1 < ceiling { line = $1 } END { print line }')"
if [ -z "$B_LINE" ]; then
    echo "== the engine never reported the empty carrier going on air =="
    echo "BLACKOUT: FAIL notpressed"
    exit 9
fi
A_LINE="$(grep -an "live output .*content: $TITLE" "$ENGINE_LOG" 2>/dev/null \
    | awk -F: -v ceiling="$B_LINE" '$1 < ceiling { line = $1 } END { print line }')"
if [ -z "$A_LINE" ] || [ "$A_LINE" -ge "$B_LINE" ]; then
    echo "== the scenario went live but no content phase preceded the blackout =="
    echo "BLACKOUT: FAIL nolive — the content that should have been hidden never reached the engine"
    exit 10
fi
echo "== engine phases: content@$A_LINE blackout@$B_LINE restored@$C_LINE =="

# check_phase <from-line> <to-line> <want: lit|black> <label>
#   Every output's LATEST frame inside the slice must be non-black (lit) or
#   all-black (black). The slice is bounded on both ends, so the blackout cannot
#   be judged by the frames the restoration wrote afterwards, or the other way
#   round. An output may legitimately be black for one frame while its per-output
#   pass warms up; a phase that STAYS black (or lit) is the defect.
check_phase() {
    local from="$1" to="$2" want="$3" label="$4"
    local frames
    frames="$(sed -n "${from},${to}p" "$ENGINE_LOG" | grep -a "output frame sanity")"
    if [ -z "$frames" ]; then
        echo "== $label: no output frame sanity line — the loop never rasterized =="
        return 1
    fi
    echo "== $label frame sanity =="
    printf '%s\n' "$frames" | sed 's/^/  /'
    local name last bad=""
    for name in $(printf '%s\n' "$frames" | sed -n "s/.*sanity: '\([^']*\)'.*/\1/p" | sort -u); do
        last="$(printf '%s\n' "$frames" | grep -a "sanity: '$name'" | tail -1)"
        if [ "$want" = "black" ]; then
            echo "$last" | grep -q "allBlack=yes" || bad="$bad
$last"
        else
            echo "$last" | grep -q "allBlack=yes" && bad="$bad
$last"
        fi
    done
    if [ -n "$bad" ]; then
        echo "== $label: the frame is not what this gesture promises =="
        printf '%s\n' "$bad"
        return 1
    fi
    return 0
}

check_phase "$A_LINE" "$((B_LINE - 1))" lit "content on air" || {
    echo "BLACKOUT: FAIL lit — content was on air but an output's frame is all black"
    exit 11
}
check_phase "$B_LINE" "$((C_LINE - 1))" black "blackout" || {
    echo "BLACKOUT: FAIL notblack — an output did NOT go empty: the clearStage carrier never reached it (only the app's tile would look cleared)"
    exit 12
}
# Phase 3 blacked out again and then STOPPED, which bounds the restored slice: a
# slice running to the end of the log would be judged by the SECOND blackout's
# frames. The restored phase therefore ends at the next empty-title marker (or
# at the engine's own "live output stopped"), whichever comes first.
STOP_LINE="$(grep -an "live output stopped" "$ENGINE_LOG" 2>/dev/null | tail -1 | cut -d: -f1)"
if [ -z "$STOP_LINE" ] || [ "$STOP_LINE" -le "$C_LINE" ]; then
    echo "== the engine never reported the session stopping after the blackout =="
    echo "BLACKOUT: FAIL staged — no live output stopped line for this run"
    exit 16
fi
D_LINE="$(grep -an "live output .*content: ''" "$ENGINE_LOG" 2>/dev/null \
    | awk -F: -v floor="$C_LINE" -v ceiling="$STOP_LINE" \
      '$1 > floor && $1 < ceiling { print $1; exit }')"
RESTORE_END="$((STOP_LINE - 1))"
if [ -n "$D_LINE" ]; then
    RESTORE_END="$((D_LINE - 1))"
fi
check_phase "$C_LINE" "$RESTORE_END" lit "restored" || {
    echo "BLACKOUT: FAIL lit — the content came back but an output's frame is black"
    exit 13
}
if [ -n "$D_LINE" ]; then
    check_phase "$D_LINE" "$((STOP_LINE - 1))" black "blackout again, then STOP" || {
        echo "BLACKOUT: FAIL notblack — the second blackout did not empty every output before STOP"
        exit 17
    }
fi

# --- THE COMPOSITOR'S OWN LAYERS -----------------------------------------
# A taken camera rides the engine scene's video layer. The frames above already
# prove the picture is empty; this proves WHY the layers had to be skipped —
# that the feed really was IN the content scene and really is NOT in the
# blackout's own scene (the engine logs one line per scene per state, and the
# clear scene must never carry a "present" line). Without a real take the
# check has no premise, and the run says so instead of passing on frames alone.
if [ -n "$TAKEN" ]; then
    BLACKOUT_LAYERS="$(sed -n "${B_LINE},$((C_LINE - 1))p" "$ENGINE_LOG" \
        | grep -a "taken input layer present")"
    if [ -n "$BLACKOUT_LAYERS" ]; then
        echo "== the blackout scene still has the taken input composited into it =="
        printf '%s\n' "$BLACKOUT_LAYERS"
        echo "BLACKOUT: FAIL layers — the clear carrier composites the camera, so \"nothing at all on screen\" is false"
        exit 19
    fi
    CONTENT_LAYERS="$(sed -n "${A_LINE},\$p" "$ENGINE_LOG" \
        | grep -a "taken input layer present" | grep -a "Blackout Probe")"
    if [ -z "$CONTENT_LAYERS" ]; then
        echo "== the take was held but the content scene never reported it — the case was not exercised =="
        echo "BLACKOUT: FAIL nopremise — no taken-input layer in the content scene"
        exit 20
    fi
    echo "== taken input: in the content scene, absent from the blackout ($TAKEN) =="
else
    echo "NOTE: no camera was taken on this machine, so the compositor-layer half was not exercised"
fi

echo "BLACKOUT: PASS content=$((B_LINE - A_LINE)) blackout=$((C_LINE - B_LINE)) staged=content outputs=checked"
exit 0
