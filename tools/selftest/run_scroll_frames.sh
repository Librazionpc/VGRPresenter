#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_scroll_frames.sh — photograph Scroll mode on a Table passage AND a Bible
# chapter: the monitor tiles and the real NDI output, at the same moment, at rest.
#
# WHY: Scroll must read like a book page on every screen — the same words, the same line breaks,
# the box full — and the monitors (QML) and the outputs (engine, NDI/HDMI) are drawn by different
# code. Eyeballing one screenshot after another missed what a side-by-side frame shows at once.
#
# WHAT IT DOES (per case)
#   1. launches the app with VGR_SELFTEST=1 VGR_TABLEWALK_TEST=1 and a "live:" query, so the pick is
#      searched, opened (The Table or Scripture, whichever the query resolves to), put in Scroll,
#      PUT ON AIR, walked 30 lines, then HELD STILL (Main.qml: twHold) with an engine.log marker
#      `TWGRAB phase=rest30`;
#   2. at the marker it waits for NDI to settle, grabs the "Lower Third" NDI source to a PNG
#      (tools/selftest/ndigrab.cpp, built on first use) and copies the tiles the scenario saved;
#   3. counts `ReadingBreaks ... DIFFERS` (a monitor surface whose drawn lines disagree with the
#      shared table) in that run's engine.log.
#
# OUTPUT   build/scroll_frames/<case>_tile<N>.png   the monitor tiles (0 = Main, 1 = Lower Third ...)
#          build/scroll_frames/<case>_ndi.png       the NDI "Lower Third" frame
#          last line:  SCROLL_FRAMES: DONE cases=<n> differs=<total>
# NOTE     it stops THIS build's running instances first (as the other selftests do).
# Usage:   bash tools/selftest/run_scroll_frames.sh [table-query] [bible-query]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
OUT="$BUILD/scroll_frames"
TABLE_QUERY="${1:-47-1100X}"
BIBLE_QUERY="${2:-Genesis 1:5}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
export PATH="$MINGW_BIN:$QT_BIN:$PATH"

mkdir -p "$OUT"
GRAB="$OUT/ndigrab.exe"
if [ ! -x "$GRAB" ]; then
    echo "== building ndigrab =="
    g++ -O1 -o "$GRAB" "$HERE/ndigrab.cpp" || { echo "SCROLL_FRAMES: FAIL ndigrab-build"; exit 2; }
fi

BUILD_WIN="$(cygpath -w "$BUILD" 2>/dev/null || printf '%s' "$BUILD")"
stop_app() {
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | Stop-Process -Force" \
        >/dev/null 2>&1
    sleep 2
}

TOTAL_DIFF=0
TOTAL_RESET=0
CASES=0
run_case() {
    local name="$1" query="$2"
    echo "== case: $name  query=\"$query\" =="
    stop_app
    rm -f "$BUILD"/tw_rest30_*.png "$BUILD"/tw_afterpick_*.png
    local start; start=$(grep -a -c . "$BUILD/logs/engine.log" 2>/dev/null || echo 0)
    ( cd "$BUILD" && VGR_SELFTEST=1 VGR_TABLEWALK_TEST=1 VGR_TABLEWALK_QUERY="live:$query~pickrow2" \
        "./appVGRPresenterUI.exe" > "$OUT/${name}_stdout.log" 2>&1 ) &
    local app=$!
    local got=0
    for _ in $(seq 1 240); do
        if tail -n +"$start" "$BUILD/logs/engine.log" 2>/dev/null | grep -a -q "TWGRAB.*phase=rest30"; then got=1; break; fi
        sleep 1
    done
    if [ "$got" -ne 1 ]; then
        echo "   no rest30 marker (the scenario did not reach its hold)"
        kill "$app" 2>/dev/null; stop_app
        return
    fi
    sleep 5   # the engine applies a window about every 1.5 s; let NDI settle on the held position
    "$GRAB" "Lower Third" "$OUT/${name}_ndi.png" > "$OUT/${name}_ndigrab.txt" 2>&1
    for f in "$BUILD"/tw_rest30_*.png; do
        [ -f "$f" ] && cp "$f" "$OUT/${name}_tile${f##*_}"
    done
    # A double-click must reach the outputs at once: after the pick marker the engine may never be told
    # "resting at 0.00" (the passage start) for a pick made further in.
    local pickReset
    for _ in $(seq 1 90); do   # the scenario picks again after its hold; wait for it to finish settling
        if tail -n +"$start" "$BUILD/logs/engine.log" | grep -a -q "TWGRAB.*phase=pick3done"; then break; fi
        sleep 1
    done
    # ...and photograph the pick as it settled (the tiles were saved at pick3done; NDI is grabbed now).
    "$GRAB" "Lower Third" "$OUT/${name}_afterpick_ndi.png" > "$OUT/${name}_afterpick_ndigrab.txt" 2>&1
    for f in "$BUILD"/tw_afterpick_*.png; do
        [ -f "$f" ] && cp "$f" "$OUT/${name}_afterpick_tile${f##*_}"
    done
    for _ in $(seq 1 40); do   # the same pick in SLIDE mode follows (PickCost mode=slide); let it finish
        if tail -n +"$start" "$BUILD/logs/engine.log" | grep -a -q "TWGRAB.*phase=slidepickdone"; then break; fi
        sleep 1
    done
    echo "   pick cost (ms) in this case:"
    tail -n +"$start" "$BUILD/logs/engine.log" | grep -a -F "[PickCost] " | sed "s/.*PickCost] /      /"
    pickReset=$(tail -n +"$start" "$BUILD/logs/engine.log" | sed -n '/TWGRAB.*phase=pick3$/,/TWGRAB.*phase=pick3done/p' | grep -a -c "resting at 0.00")
    # The double-click lands on ROW 2 (~pickrow2: the third paragraph / verse 3), never the passage start, so any
    # "resting at 0.00" after it is the bug this counts. (Row 0 made that answer correct and the check meaningless.)
    echo "   after the double-click on row 2: engine told \"resting at 0.00\" $pickReset time(s) (want 0)"
    TOTAL_RESET=$((TOTAL_RESET + pickReset))
    local differs
    differs=$(tail -n +"$start" "$BUILD/logs/engine.log" | grep -a -c "ReadingBreaks.*DIFFERS")
    local agrees
    agrees=$(tail -n +"$start" "$BUILD/logs/engine.log" | grep -a -c "ReadingBreaks.*agrees")
    echo "   frames: $(ls "$OUT" | grep -c "^${name}_") files | ReadingBreaks agrees=$agrees differs=$differs"
    TOTAL_DIFF=$((TOTAL_DIFF + differs))
    CASES=$((CASES + 1))
    kill "$app" 2>/dev/null
    stop_app
}

run_case table "$TABLE_QUERY"
run_case bible "bible:$BIBLE_QUERY"
echo "frames in: $OUT"
echo "SCROLL_FRAMES: DONE cases=$CASES differs=$TOTAL_DIFF pickResets=$TOTAL_RESET"
