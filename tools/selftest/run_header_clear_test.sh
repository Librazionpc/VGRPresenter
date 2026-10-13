#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_header_clear_test.sh — do the header's right-hand tiles
# stay clear of the window's caption buttons at every window size?
#
# The report: "the header tile is overlapping the minimize, maximize, close
# button ... it only happens when the window scales down". Two separate facts
# make that true, and this checks BOTH:
#
#   * the tab row's tiles (GO LIVE, then Settings) hold the window's own right
#     end, in the SAME column as the caption buttons above them — that is the
#     design. The header must keep the two rows apart (a tile that drifted up
#     into the caption row would sit under the buttons), so the check is
#     separation in the header's own design space, at every size;
#   * the window's resize grips do NOT scale (they are the frame), so the
#     smaller the window, the larger a share of the buttons an 8px strip covers
#     — the symptom being a SIZE cursor and a press that resizes the window
#     instead of minimizing/closing it.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_HEADER_TEST=1 (output ->
#      build/header_clear_test.log)
#   2. the Main.qml scenario measures, at the LAUNCH size, the MINIMUM size and
#      one size in between: the caption cluster and the tiles in the header's
#      own design units (clearance, row order), the cluster in window pixels
#      against every resize grip's rect, and the LIVE OS cursor parked 3px
#      inside the close button's top edge (Win32 GetCursorInfo — the same call a
#      user's eye makes; a size arrow there is the defect)
#   3. it then leaves the window at its smallest size and grabs the WHOLE WINDOW
#      (build/header_clear_small.png) for the pixel check
#
# VERDICT (last line, greppable):
#   HEADER_CLEAR: PASS        — tiles clear of the column, no grip over the
#                               buttons, the caption row's own cursor an arrow,
#                               and the column under the buttons empty in pixels
#   HEADER_CLEAR: FAIL missing   — the scenario never reported (the driver never
#                                  armed; see the log)
#   HEADER_CLEAR: FAIL overlap   — a tile intersects the caption cluster, or has
#                                  left its own row (overlap=false, below=true)
#   HEADER_CLEAR: FAIL grips     — a resize grip's rect covers the buttons
#   HEADER_CLEAR: FAIL cursor    — the OS shows a resize arrow ON a caption button
#   HEADER_CLEAR: FAIL pixels    — the live grab draws ink in the column under the
#                                  caption buttons (a tile strayed back into it)
#   HEADER_CLEAR: FAIL grab      — the whole-window grab was never written
#
# Usage:  bash tools/selftest/run_header_clear_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/header_clear_test.log"
PNG="$BUILD/header_clear_small.png"
WAIT_SECS="${1:-70}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "HEADER_CLEAR: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$PNG"
echo "== launching with VGR_SELFTEST=1 VGR_HEADER_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_HEADER_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-HEADER\] summary" "$LOG" 2>/dev/null; then
        break     # the measurements are in; the grab + quit follow
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
LINES="$(grep -a "\[SELFTEST-HEADER\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-HEADER lines — the driver never armed; see $LOG)"
    echo "HEADER_CLEAR: FAIL missing"
    exit 3
fi
echo "$LINES"

BAD="$(printf '%s\n' "$LINES" | grep -v "clear=true")"
if [ -n "$BAD" ] && printf '%s\n' "$BAD" | grep -q .; then
    echo "== a caption-clearance measurement failed =="
    printf '%s\n' "$BAD"
    if printf '%s\n' "$BAD" | grep -q "gripHits=[1-9]"; then
        echo "HEADER_CLEAR: FAIL grips"
        exit 5
    fi
    if printf '%s\n' "$BAD" | grep -q "probeCursor=SIZE"; then
        echo "HEADER_CLEAR: FAIL cursor"
        exit 6
    fi
    echo "HEADER_CLEAR: FAIL overlap"
    exit 4
fi

# The pixel half: the whole-window grab at the smallest size, where the report
# says the collision shows. The region sampled is the caption COLUMN, one row
# below the buttons — empty means no tile strayed back under them.
SMALL="$(printf '%s\n' "$LINES" | grep " minimum " | tail -1)"
if [ -z "$SMALL" ]; then
    SMALL="$(printf '%s\n' "$LINES" | tail -1)"
fi
WIN="$(printf '%s\n' "$SMALL" | sed -n 's/.*window=\([0-9]*x[0-9]*\).*/\1/p')"
WINW="${WIN%x*}"
WINH="${WIN#*x}"
US="$(printf '%s\n' "$SMALL" | sed -n 's/.*uiScale=\([0-9.]*\).*/\1/p')"
CW="$(printf '%s\n' "$SMALL" | sed -n 's/.*clusterWin=\([0-9-]*,[0-9-]*,[0-9-]*,[0-9-]*\).*/\1/p')"
CX="$(printf '%s' "$CW" | cut -d, -f1)"
CY="$(printf '%s' "$CW" | cut -d, -f2)"
CWD="$(printf '%s' "$CW" | cut -d, -f3)"
CH="$(printf '%s' "$CW" | cut -d, -f4)"

if [ ! -f "$PNG" ]; then
    echo "== the whole-window grab is missing: $PNG =="
    echo "HEADER_CLEAR: FAIL grab"
    exit 8
fi

echo "== pixel check (whole window at the smallest size) =="
if ! node "$HERE/corner_empty_pixels.mjs" "$PNG" "$WINW" "$WINH" \
        "$CX" "$CY" "$CWD" "$CH" "$US" --max 40; then
    echo "HEADER_CLEAR: FAIL pixels"
    exit 7
fi

echo "HEADER_CLEAR: PASS"
exit 0
