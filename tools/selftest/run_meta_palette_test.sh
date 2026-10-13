#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_meta_palette_test.sh — do the metadata palette's two
# gestures actually work?
#
# The gap this closes: the palette (the Edit screen's ">" + menu, Metadata chip)
# could add a metadata field TWO ways — CLICK a chip to add a text item carrying
# that placeholder, or DRAG a chip onto the canvas to land it where the pointer
# is. Neither gesture had a scenario; the boot check loads the whole palette
# tree, but nothing drove the gesture itself.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_METAPAL_TEST=1 (output ->
#      build/meta_palette_test.log)
#   2. the Main.qml scenario drives the REAL path with REAL input: a new show,
#      the + chip, the Metadata chip in the + menu (a zone-hover chip, so the
#      hover move is part of the gesture), then hovers a field chip, CLICKS it,
#      and finally presses another field chip and drags it onto the canvas
#   3. the fields come from MetadataFields.all — the registry the palette draws
#      from — so the run follows the list instead of spelling tokens here; the
#      first field is clicked and the LAST one (today The Table's code) dragged
#   4. each added item is read back off its live DELEGATE (the thing the user
#      sees): its text must be the token, and a dropped item's own centre must
#      sit within 60 canvas units of the drop point ("land it where the pointer
#      is"); the palette itself is grabbed to build/meta_palette.png
#
# VERDICT (last line, greppable):
#   METAPAL: PASS            — both gestures added the right item, in the right
#                              place, driven through the real palette
#   METAPAL: FAIL missing    — the scenario never reported (the driver never
#                              armed; see the log)
#   METAPAL: FAIL palette    — the + menu or the palette never opened (the
#                              gesture could not be reached at all)
#   METAPAL: FAIL hint       — hovering a field chip published no hint
#   METAPAL: FAIL click      — clicking a field chip added no item, or one whose
#                              text is not that field's token
#   METAPAL: FAIL drag       — dragging a field chip onto the canvas added no
#                              item, or one that did not land at the pointer
#   METAPAL: FAIL popup      — the popup checks never reported
#   METAPAL: FAIL cursor     — over the panel the cursor was still the I-beam of
#                              the text item BENEATH it (or that item did not
#                              read blocked): the panel is not lifted above the
#                              canvas, and a chip does not even show its hand
#   METAPAL: FAIL dismiss    — a click outside the panel did not close it
#
# Usage:  bash tools/selftest/run_meta_palette_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/meta_palette_test.log"
PNG="$BUILD/meta_palette.png"
WAIT_SECS="${1:-80}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "METAPAL: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$PNG"
echo "== launching with VGR_SELFTEST=1 VGR_METAPAL_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_METAPAL_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-METAPAL\] summary" "$LOG" 2>/dev/null; then
        break     # both gestures reported; the grab + quit follow
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
# The scenario quits itself after the grab, but a stall there must not hang the
# runner: every verdict line is in the log by now, so stop whatever is left and
# read it. (An unbounded `wait` here made a stalled grab look like a hung test —
# the run produced its lines and then never returned.)
if kill -0 "$RUN_PID" 2>/dev/null; then
    echo "== the app did not quit within ${WAIT_SECS}s — stopping it (pid $RUN_PID) =="
    kill "$RUN_PID" 2>/dev/null
    sleep 2
    kill -9 "$RUN_PID" 2>/dev/null
fi
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
LINES="$(grep -a "\[SELFTEST-METAPAL\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-METAPAL lines — the driver never armed; see $LOG)"
    echo "METAPAL: FAIL missing"
    exit 3
fi
echo "$LINES"

PALETTE="$(printf '%s\n' "$LINES" | grep "palette open=" | tail -1)"
if [ -z "$PALETTE" ] || printf '%s' "$PALETTE" | grep -qv "palette open=true"; then
    echo "== the + menu or the metadata palette never opened =="
    echo "METAPAL: FAIL palette"
    exit 4
fi
if printf '%s' "$PALETTE" | grep -q "palette unusable"; then
    echo "METAPAL: FAIL palette"
    exit 4
fi

HINT="$(printf '%s\n' "$LINES" | grep "hint got=" | tail -1)"
if [ -z "$HINT" ] || printf '%s' "$HINT" | grep -qv "hintOk=true"; then
    echo "== hovering a field chip published no hint =="
    echo "METAPAL: FAIL hint"
    exit 5
fi

CLICK="$(printf '%s\n' "$LINES" | grep "click token=" | tail -1)"
if [ -z "$CLICK" ] || printf '%s' "$CLICK" | grep -qv "textOk=true"; then
    echo "== clicking a field chip did not add that field =="
    echo "METAPAL: FAIL click"
    exit 6
fi
if printf '%s' "$CLICK" | grep -q "delegate=no"; then
    echo "METAPAL: FAIL click"
    exit 6
fi

DRAG="$(printf '%s\n' "$LINES" | grep "drag token=" | tail -1)"
if [ -z "$DRAG" ] || printf '%s' "$DRAG" | grep -qv "textOk=true"; then
    echo "== dragging a field chip onto the canvas did not add that field =="
    echo "METAPAL: FAIL drag"
    exit 7
fi
if printf '%s' "$DRAG" | grep -qv "landed=true"; then
    echo "== the dropped item did not land where the pointer was =="
    echo "METAPAL: FAIL drag"
    exit 7
fi
if printf '%s' "$DRAG" | grep -q "delegate=no"; then
    echo "METAPAL: FAIL drag"
    exit 7
fi
# A drop is the same insert as a click: it must carry the field's own engine
# slot and the registry's style, not a bare new text box.
if printf '%s' "$DRAG" | grep -qv "bindOk=true"; then
    echo "== the dropped item did not carry the field's engine slot (then: $DRAG)"
    echo "METAPAL: FAIL drag-slot"
    exit 7
fi
if printf '%s' "$DRAG" | grep -qv "font=40"; then
    echo "== the dropped item was not built to its token's style (then: $DRAG)"
    echo "METAPAL: FAIL drag-style"
    exit 7
fi

# The POPUP half: over the panel the cursor must be the panel's (a chip's
# pointing hand, the panel's arrow on its own padding) — never the I-beam the
# text item BENEATH it claims, which is what made the panel read as not lifted
# above the canvas — and a click outside the box must close it.
POPUP="$(printf '%s\n' "$LINES" | grep "popup chipCursor=" | tail -1)"
if [ -z "$POPUP" ]; then
    echo "== the popup checks never reported =="
    echo "METAPAL: FAIL popup"
    exit 8
fi
if [ "$(printf '%s' "$POPUP" | grep -o 'cursorOk=[^ ]*' | cut -d= -f2)" != "true" ]; then
    echo "== the canvas cursor still shows through the panel, or the item under it is not blocked =="
    echo "METAPAL: FAIL cursor (then: $POPUP)"
    exit 8
fi
if [ "$(printf '%s' "$POPUP" | grep -o 'dismissOk=[^ ]*' | cut -d= -f2)" != "true" ]; then
    echo "== a click outside the panel did not close it =="
    echo "METAPAL: FAIL dismiss (then: $POPUP)"
    exit 9
fi

if [ -f "$PNG" ]; then
    echo "== palette grab: $PNG =="
else
    echo "(the palette grab was not written — the measurement above still stands)"
fi

echo "METAPAL: PASS"
exit 0
