#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_style_bg_test.sh — does an output tile repaint when its
# style gains a background image?
#
# The regression guard for "i select the style for output when there was no bg
# image in it, then i went to add the bg image — the output screen did not
# update its bg image, it still remained transparent". The tile composes its
# style background ({ color, image, hasImage }) at READ time, so the failure
# has two possible shapes and they need different fixes:
#   * the MODEL never carried the image (the save path dropped it), or
#   * the model HAS it and the TILE never re-read it (a stale binding).
# Only the second is a UI bug, and the operator's screenshot can't tell them
# apart — so this scenario measures both, side by side, FOR THE SAME OUTPUT.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_STYLEBG_TEST=1 (stdout+stderr
#      -> build/style_bg_test.log)
#   2. the Main.qml scenario (SelfTestStyleBg) resolves the SHOW wall's first
#      tile (the surface the operator looks at), puts THAT tile's output on a
#      style with NO background image, prints the tile's own styleBg beside the
#      model's answer for the same output index, then gives that style a
#      background image and prints both again (twice, in case a binding only
#      needs another event-loop turn), and finally restores the style's
#      original image
#      It ALSO composes a synthetic slide three ways and prints the composed
#      background each time: with a COLOUR just written (the roster identity
#      probe), with a style that names NO background at all, and with no style
#      assigned at all. The last two must compose "" — nothing painted — because
#      the engine leaves such a frame keyable (SceneBuilder::ComposeSlide); a
#      colour there is a black slab over whatever the output is keyed onto
#   3. reads the [SELFTEST-STYLEBG] lines and checks that the tile's hasImage
#      follows the model's hasImage at every step
#
# Every line carries tileIndex/tileStyleId/styleMatch so the two halves of a
# comparison can never drift onto different outputs (the first version of this
# scenario did exactly that, and read as a pass on a stale tile).
#
# VERDICT (last line, greppable):
#   STYLE_BG: PASS        — the tile reports the image the model carries
#   STYLE_BG: FAIL stale  — the model has the image and the tile does not (the
#                           reported "still transparent")
#   STYLE_BG: FAIL missing— the scenario produced no lines, a step never
#                           reported, or the premise was not established
#   STYLE_BG: FAIL roster — the Styles screen and the output model resolve styles
#                           through DIFFERENT rosters (two StyleListModel
#                           instances), so a style edit can never reach an
#                           output no matter how the tile binds. This is the
#                           defect the scenario found; it is reported first
#                           because none of the checks below can pass while it
#                           holds.
#   STYLE_BG: FAIL noimage— no usable image file on this machine to test with
#   STYLE_BG: FAIL layers  — the tile's transparency checkerboard is not below
#                           the media/block layers, so it paints over a pushed
#                           media or camera frame (the reported "the checkerboard
#                           area is where the media is meant to show")
#   STYLE_BG: FAIL previewroster — the tile's COMPOSED picture (previewWithStyle)
#                           resolves styles through the other StyleListModel, so
#                           the tile renders a template/background the operator
#                           never picked while the real output renders the edit
#   STYLE_BG: FAIL unnamedbg — a style that names NO background (no colour, no
#                           image) composes to an opaque colour in the preview, so
#                           the tile paints a black slab over whatever the output
#                           is keyed onto while the real output stays transparent
#                           (the engine paints nothing at all in that state)
#   STYLE_BG: FAIL styleimage — clearBackgroundOnText does not step the style's
#                           background image aside for the right slides: the
#                           composition must suppress it for a slide that CARRIES
#                           content, and keep it for a blank go-live carrier /
#                           title-only slide (the branded backdrop the engine is
#                           still painting there)
#   STYLE_BG: FAIL framerule — the composition does not say which on-air content
#                           needs the compositor's DISTRIBUTED FRAME: a slide
#                           carrying a camera/media/audio/image block can only be
#                           stood in for by a block renderer, so the frame wins
#                           there, and a text/shape/clock/timer slide draws true
#                           and keeps the blocks. Both preview surfaces read that
#                           one answer (each used to keep its own copy of the
#                           kind test)
#
# NOTE: MUTATES one style's background image (clears it, adds one, restores the
# original at the end) and points the measured output at the first style.
#
# Usage:  bash tools/selftest/run_style_bg_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/style_bg_test.log"
WAIT_SECS="${1:-40}"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "STYLE_BG: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_STYLEBG_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_STYLEBG_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-STYLEBG\] restored" "$LOG" 2>/dev/null; then
        sleep 1
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
LINES="$(grep -a "\[SELFTEST-STYLEBG\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-STYLEBG lines — the driver never armed; see $LOG)"
    echo "STYLE_BG: FAIL missing"
    exit 3
fi
echo "$LINES"

if echo "$LINES" | grep -q "no usable image file"; then
    echo "STYLE_BG: FAIL noimage"
    exit 4
fi

# FIRST: does the UI and the output model even share a style roster? The
# scenario writes a colour through the QML singleton and reads it back through
# OutputListModel's own style lookup; when those disagree, nothing below can be
# trusted (and the background-image symptom is a symptom, not the cause).
PROBE="$(printf '%s\n' "$LINES" | grep "identity probe" | tail -1)"
if [ -n "$PROBE" ] && echo "$PROBE" | grep -q "sameRoster=false"; then
    echo "== the Styles screen and the output model resolve styles through DIFFERENT rosters =="
    echo "$PROBE"
    echo "== instances constructed during this run (from $LOG) =="
    grep -a "SELFTEST-STYLEBG-ID" "$LOG" 2>/dev/null | head -6
    echo "STYLE_BG: FAIL roster — the tiles, output windows and the engine read a second StyleListModel copy that the Styles screen's edits never reach, so a style's new background image stays invisible until restart"
    exit 8
fi

# SECOND, and the one the operator's screenshot cannot separate either: the
# monitor tile's own PICTURE is composed by previewWithStyle, which resolves
# the style itself. Reading the C++ singleton there (a second StyleListModel,
# see the roster note above) made the tile compose a DIFFERENT template and
# background from the real output while every StyleBackgroundRole above looked
# correct — "it only happens on the preview tile". The scenario writes a
# colour through the QML singleton, composes a synthetic slide with that
# style and prints the composed background back; only the UI's own roster can
# return the colour it just wrote.
PREVPROBE="$(printf '%s\n' "$LINES" | grep "preview composition probe" | tail -1)"
if [ -z "$PREVPROBE" ]; then
    echo "STYLE_BG: FAIL missing — the composition probe never reported"
    exit 3
fi
if echo "$PREVPROBE" | grep -q "sameRoster=false"; then
    echo "== the preview composition resolves styles through a DIFFERENT roster than the UI =="
    echo "$PREVPROBE"
    echo "STYLE_BG: FAIL previewroster — the composed tile picture reads a second StyleListModel, so it renders a style other than the one the operator edits (a different template/background from the real output) until a restart"
    exit 9
fi

# THE UNNAMED BACKGROUND (the fourth probe): a style with neither a colour nor an
# image. The engine paints NOTHING in that state — the frame is keyable, which is
# what the monitor tile's transparency convention exists to show — so the preview
# must compose to "" (nothing named), never to a colour it invented. A preview
# that answered with a colour here is a black slab over the feed on the tile
# while the real output stays keyable.
UNSETBG="$(printf '%s\n' "$LINES" | grep "unnamed background probe" | tail -1)"
if [ -z "$UNSETBG" ]; then
    echo "STYLE_BG: FAIL missing — the unnamed-background probe never reported"
    exit 3
fi
# THE STYLE IMAGE RULE (the fifth probe): clearBackgroundOnText steps the
# style's image aside for a slide that CARRIES content and not for one that
# merely exists. The engine decides it in the composition and hands the answer
# to the tile and the output window; both surfaces used to re-derive it from
# `hasSlidePreview`, which dropped the branded backdrop on a blank go-live
# carrier the engine was still painting it on.
IMGRULE="$(printf '%s\n' "$LINES" | grep "style image rule" | tail -1)"
if [ -z "$IMGRULE" ]; then
    echo "STYLE_BG: FAIL missing — the style-image rule probe never reported"
    exit 3
fi
if echo "$IMGRULE" | grep -q "ok=false"; then
    echo "== the style background image is suppressed for the wrong slides =="
    echo "$IMGRULE"
    echo "STYLE_BG: FAIL styleimage — clearBackgroundOnText must step the style's background image aside for a slide that CARRIES content (a blank go-live carrier and a title-only slide keep it); a surface deriving this for itself instead of reading the engine's answer is how the branded backdrop disappears on the tile while the output still shows it"
    exit 12
fi

if echo "$UNSETBG" | grep -q "unpainted=false"; then
    echo "== a style that names no background composed to a colour =="
    echo "$UNSETBG"
    echo "STYLE_BG: FAIL unnamedbg — with no style colour and no style image the composition must leave the frame UNPAINTED (the engine paints no background object at all); composing a colour instead puts an opaque slab of the tile's own over whatever the output is keyed onto"
    exit 11
fi

# THE FRAME RULE (the sixth probe): a slide carrying a camera / media / audio /
# image block can only be STOOD IN FOR by a block renderer, so the compositor's
# distributed frame is the honest picture for it and must win; every other kind
# draws true block by block and the blocks win. The composition answers that
# once and BOTH preview surfaces read it — the monitor tile and the output
# window each used to run their own copy of the kind test, which is how one of
# them ends up showing a placeholder where the output shows the picture.
FRAMERULE="$(printf '%s\n' "$LINES" | grep "frame rule" | tail -1)"
if [ -z "$FRAMERULE" ]; then
    echo "STYLE_BG: FAIL missing — the frame-rule probe never reported"
    exit 3
fi
if echo "$FRAMERULE" | grep -q "ok=false"; then
    echo "== the composition does not say which content needs the distributed frame =="
    echo "$FRAMERULE"
    echo "STYLE_BG: FAIL framerule — a media-kind block (camera/media/audio/image) has to make the composition say the distributed frame wins, and a text block must not; a surface deriving that for itself is how a preview shows a placeholder tile where the output shows the real picture"
    exit 13
fi

# THIRD: the tile's own LAYER ORDER. "Same visible data, wrong picture" is a
# z-order bug, and the tile's transparency checkerboard is the one that bit:
# drawn above the media/block layers it paints opaquely OVER a pushed media
# frame, so the tile showed checkers where the media belongs while the real
# output window (which draws no checkerboard) showed the media. The scenario
# reports the pane's child order, so this can only pass when the checkers sit
# below the block render and the media layer.
LAYERS="$(printf '%s\n' "$LINES" | grep "tile layers:" | tail -1)"
if [ -z "$LAYERS" ]; then
    echo "STYLE_BG: FAIL missing — the tile layer-order probe never reported"
    exit 3
fi
if echo "$LAYERS" | grep -q "belowContent=false"; then
    echo "== the tile's checkerboard is not below the media/block layers =="
    echo "$LAYERS"
    echo "STYLE_BG: FAIL layers — the tile paints its transparency convention over the content stack, so a pushed media or camera frame is buried under checkers on the preview tile (the real output, which draws none, shows it)"
    exit 10
fi

CLEARED="$(printf '%s\n' "$LINES" | grep " cleared " | tail -1)"
ADDED="$(printf '%s\n' "$LINES" | grep " added " | tail -1)"
SETTLED="$(printf '%s\n' "$LINES" | grep " settled " | tail -1)"
if [ -z "$CLEARED" ] || [ -z "$ADDED" ] || [ -z "$SETTLED" ]; then
    echo "STYLE_BG: FAIL missing — a step never reported (cleared='$CLEARED' added='$ADDED' settled='$SETTLED')"
    exit 3
fi

# require <step-label> <line> <exit-code> <must-have substrings...>
require() {
    local label="$1" line="$2" code="$3"
    shift 3
    local want
    for want in "$@"; do
        case "$line" in
            *"$want"*) ;;
            *)
                echo "== $label: expected '$want' =="
                echo "$line"
                return "$code"
                ;;
        esac
    done
    return 0
}

# THE PREMISE, and it is checked before the verdict: the tile must really be
# showing the output that wears the style under test, and that style really
# must have no image yet. Without this the two comparisons below could agree
# with each other while describing the wrong output (a stale tile reads as a
# pass).
require "cleared" "$CLEARED" 5 \
    "tile=yes" "styleMatch=true" "modelHasImage=false" "tileHasImage=false" || {
    echo "STYLE_BG: FAIL missing — the starting state is not the one under test"
    exit 5
}

# From here the model MUST carry the image (a model that does not is a save-path
# bug, not a tile bug — reported separately), and the tile must agree with it.
for STEP in "$ADDED" "$SETTLED"; do
    require "model carries the image" "$STEP" 6 "modelHasImage=true" || {
        echo "STYLE_BG: FAIL stale — the style's image never reached the model"
        exit 6
    }
done
for STEP in "$ADDED" "$SETTLED"; do
    require "tile repaints (stale binding)" "$STEP" 7 "tileHasImage=true" || {
        echo "STYLE_BG: FAIL stale — the model has the image and the tile still reports none (still transparent)"
        exit 7
    }
done

echo "STYLE_BG: PASS"
exit 0
