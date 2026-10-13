#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_modal_fit_test.sh — is the Settings card still inside the
# window it opens in?
#
# The regression guard for "the Settings modal is not responsive": the card
# (ModalShell) carries a 1080x640 design size while the window it lives in is
# frameless and can be smaller than that — Main.qml bounds the window to the
# active screen and lets the user drag it down to 820x560. When the card does
# not shrink to fit, it is clipped at BOTH edges: the nav rail hangs off the
# left of the window and the section content off the right, which reads as a
# broken settings screen rather than an oversized one.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_MODALFIT_TEST=1 (stderr ->
#      build/modal_fit_test.log)
#   2. the Main.qml scenario opens EVERY popup (Settings sections and their Add
#      dialogs, Outputs, New show, Import, quick search, the new-category Name
#      dialog) at the smallest window size, measures each CARD against the window
#      and grabs it to a PNG, then restores the launch size and quits
#   3. reads the [SELFTEST-MODALFIT] lines (fits=true), the [SELFTEST-UISCALE]
#      line (every layer reports the same scale), the [SELFTEST-SCRIMS] lines
#      (each dim backdrop still covers the whole window), [SELFTEST-AVBOARD],
#      the [SELFTEST-DROP] line (an open dropdown is stacked in the modal's own
#      layer, above it), the [SELFTEST-CARD] line (the Add Source dialog has
#      content AND a viewport tall enough to show it), the
#      [SELFTEST-TOASTDEDUP] line (six pushes of ONE notice message became one
#      card, not six), the [SELFTEST-TOASTCRUX] line (that notice's device name and
#      quoted source name are drawn BOLD, the text otherwise intact) and the
#      [SELFTEST-TOASTACTION] line (that notice's one button exists and its click
#      ran the action the notice named)
#
# VERDICT (last line, greppable):
#   MODAL_FIT: PASS          — every card fits, every scrim covers the window
#   MODAL_FIT: FAIL clipped  — a card hangs outside the window (see the lines)
#   MODAL_FIT: FAIL missing  — the scenario produced no measurements
#   MODAL_FIT: FAIL scale    — two window-level layers disagree about the app
#                              scale (a layer that forgot it, or one that applied
#                              it twice)
#   MODAL_FIT: FAIL detached — a popup lost its full-window backdrop (a scaled
#                              scrim — the dialog's black box shrank with it), or
#                              a scaled screen stopped short of the window's own
#                              edges, leaving the panes detached from it
#   MODAL_FIT: FAIL overlay  — an open dropdown is not in the modal's layer, so it
#                              draws behind the dialog (the list is invisible)
#   MODAL_FIT: FAIL blank    — a dialog has content but its viewport collapsed to
#                              zero height, so it renders as an empty shell
#   MODAL_FIT: FAIL toast-duplicates — the same notice pushed six times stacked six
#                              cards instead of coalescing onto one
#   MODAL_FIT: FAIL toast-crux-plain — the notice drew its whole sentence in plain
#                              text: the device name it is about (its crux) never
#                              reached the renderer bold, so the operator has to
#                              read the sentence to find the subject
#   MODAL_FIT: FAIL toast-action-missing / toast-action-dead — a notice that
#                              offers something (the crash-recovery offer is the
#                              one that matters) drew no button, or drew one whose
#                              click ran nothing
#
# Usage:  bash tools/selftest/run_modal_fit_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/modal_fit_test.log"
WAIT_SECS="${1:-45}"
# The app needs the MSYS2 UCRT64 + Qt bin dirs on PATH or it dies with a
# DLL-missing popup before any scenario could arm.
# The paths are picked per shell: the agent shell is busybox ash, where /c/...
# does not resolve and only the native C:/... form works; the developer's MSYS
# shell needs the POSIX form (a Windows-form entry in a POSIX PATH leaves the
# child with no usable entries at all). Same overridable names as dev_cycle.sh.
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "MODAL_FIT: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_MODALFIT_TEST=1 (stderr -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_MODALFIT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-MODALFIT\] minimum" "$LOG" 2>/dev/null; then
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
LINES="$(grep -a "\[SELFTEST-MODALFIT\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-MODALFIT lines — the driver never armed; see $LOG)"
    echo "MODAL_FIT: FAIL missing"
    exit 3
fi
echo "$LINES"

# The Audio & Video routing board is measured too: its columns are laid out by
# hand inside a Flickable that never scrolls sideways, so a board wider than its
# room puts the video column out of reach ("resizing cuts off the video bus").
# The app's ONE scale must be on every window-level layer. A layer that forgot it
# (or a component that scaled itself twice) reports a different number here.
UISCALE="$(grep -a "\[SELFTEST-UISCALE\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$UISCALE" ]; then
    echo "MODAL_FIT: FAIL missing — the ui-scale check never ran (see $LOG)"
    exit 6
fi
echo "$UISCALE"
UNIQUE="$(echo "$UISCALE" | tr ' ' '\n' | grep '=' | cut -d= -f2 | sort -u | wc -l)"
if [ "$UNIQUE" -ne 1 ]; then
    echo "== layers disagree about the app scale =="
    echo "$UISCALE"
    echo "MODAL_FIT: FAIL scale"
    exit 7
fi

BOARD="$(grep -a "\[SELFTEST-AVBOARD\]" "$LOG" 2>/dev/null)"
if [ -z "$BOARD" ]; then
    echo "MODAL_FIT: FAIL missing — the board check never ran (see $LOG)"
    exit 5
fi
echo "$BOARD"

# The other half of the modal rule: the CARD is scaled, the SCRIM behind it never
# is. A scrim that inherited a layer's scale no longer covers the window, which is
# exactly what "the popup has no black box around it" looked like. The same line
# reports whether the scaled screens still COVER the window (the panes must stay
# attached to its edges: a design-sized canvas left the bottom dock floating ~26px
# above the window at 820x560).
SCRIMS="$(grep -a "\[SELFTEST-SCRIMS\]" "$LOG" 2>/dev/null)"
if [ -z "$SCRIMS" ]; then
    echo "MODAL_FIT: FAIL missing — the scrim check never ran (see $LOG)"
    exit 8
fi
echo "$SCRIMS"
DETACHED="$(printf '%s\n%s\n' "$LINES" "$SCRIMS" | grep "covers=" | grep -v "covers=true")"
if [ -n "$DETACHED" ]; then
    echo "== a popup lost its full-window backdrop, or a screen detached from the window =="
    echo "$DETACHED"
    echo "MODAL_FIT: FAIL detached"
    exit 9
fi

# The desktop the app sized itself to (Main.qml's reportDesktop): the launch window
# must fill the work area the app opened on — that is what "the app scales to the
# desktop screen" means, and what the engine's own display size feeds. fills=false
# means the window opened at something else (the old fixed 1440x900).
DESKTOP="$(grep -a "\[SELFTEST-DESKTOP\]" "$LOG" 2>/dev/null)"
if [ -z "$DESKTOP" ]; then
    echo "MODAL_FIT: FAIL missing — the desktop check never ran (see $LOG)"
    exit 11
fi
echo "$DESKTOP"
if echo "$DESKTOP" | grep -v "fills=true" | grep -q .; then
    echo "MODAL_FIT: FAIL desktop"
    exit 11
fi

# The notices: scaled by the app scale (so they resize with everything else) and
# still pinned inside the window's corner. Its fits= combines "inside the window"
# with "drawn at 320 x uiScale" — a toast left at its 320px design width fails.
TOAST="$(grep -a "\[SELFTEST-TOAST\]" "$LOG" 2>/dev/null)"
if [ -z "$TOAST" ]; then
    echo "MODAL_FIT: FAIL missing — the toast check never ran (see $LOG)"
    exit 10
fi
echo "$TOAST"

# The notices must COALESCE, not stack. The toast step pushes the SAME text six
# times — the shape a settings write raises when the engine is not running, one
# notice per attempt — and six copies used to become six cards, none of them
# readable. One card standing for all six is the pass.
DEDUP="$(grep -a "\[SELFTEST-TOASTDEDUP\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$DEDUP" ]; then
    echo "MODAL_FIT: FAIL missing — the toast coalescing check never ran (see $LOG)"
    exit 16
fi
echo "$DEDUP"
if echo "$DEDUP" | grep -v "dedupe=true" | grep -q .; then
    echo "MODAL_FIT: FAIL toast-duplicates"
    exit 17
fi

# The notice's CRUX must be BOLD on screen: the names a notice is about (a
# device, a source) ride the payload as `crux` and the card wraps each of them
# in <b> for the renderer, leaving the rest of the sentence plain. The step
# pushes the real missing-device notice — device name as crux, one quoted
# source name inside the sentence — so emphasis= is the verdict on both paths:
# a card drawn entirely in plain text is the complaint this check answers.
CRUX="$(grep -a "\[SELFTEST-TOASTCRUX\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$CRUX" ]; then
    echo "MODAL_FIT: FAIL missing — the toast crux check never ran (see $LOG)"
    exit 18
fi
echo "$CRUX"
if echo "$CRUX" | grep -v "emphasis=true" | grep -q .; then
    echo "MODAL_FIT: FAIL toast-crux-plain"
    exit 19
fi

# ...and the notice's ONE CLICK, which is how the crash-recovery offer opens the
# show it is about: the notice carries an action (an id and a label), the card
# draws a button for it and pressing it runs what that id was registered for. A
# card whose button draws but does nothing is the failure this catches.
ACTION="$(grep -a "\[SELFTEST-TOASTACTION\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$ACTION" ]; then
    echo "MODAL_FIT: FAIL missing — the toast action check never ran (see $LOG)"
    exit 20
fi
echo "$ACTION"
if echo "$ACTION" | grep -v "button=true" | grep -q .; then
    echo "MODAL_FIT: FAIL toast-action-missing"
    exit 21
fi
if echo "$ACTION" | grep -v "ran=true" | grep -q .; then
    echo "MODAL_FIT: FAIL toast-action-dead"
    exit 22
fi

# A dropdown opened from a field inside a dialog must share the MODAL's layer and
# sit above it. Reparented to the window's contentItem instead of the Controls
# overlay it was a sibling BELOW every modal (all of which live in Overlay.overlay),
# so a device list drew behind the open dialog: the field's options were "nowhere in
# sight" while the panel's own geometry looked perfect.
DROP="$(grep -a "\[SELFTEST-DROP\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$DROP" ]; then
    echo "MODAL_FIT: FAIL missing — the dropdown layering check never ran (see $LOG)"
    exit 12
fi
echo "$DROP"
if echo "$DROP" | grep -v "layer=true" | grep -q .; then
    echo "MODAL_FIT: FAIL overlay"
    exit 13
fi

# The Add Source dialog's own anatomy: real content with a zero-height viewport is
# an EMPTY-LOOKING dialog that every other check calls a pass (an empty card still
# fits the window). blank=true is the "add source has cleared it" shape.
CARD="$(grep -a "\[SELFTEST-CARD\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$CARD" ]; then
    echo "MODAL_FIT: FAIL missing — the card anatomy check never ran (see $LOG)"
    exit 14
fi
echo "$CARD"
if echo "$CARD" | grep -v "blank=false" | grep -q .; then
    echo "MODAL_FIT: FAIL blank"
    exit 15
fi

# Every measurement must hold at every window size.
BAD="$(printf '%s\n%s\n%s\n' "$LINES" "$BOARD" "$TOAST" | grep -v "fits=true")"
if [ -n "$BAD" ]; then
    echo "== something hangs outside its room =="
    echo "$BAD"
    echo "MODAL_FIT: FAIL clipped"
    exit 4
fi

echo "MODAL_FIT: PASS"
exit 0
