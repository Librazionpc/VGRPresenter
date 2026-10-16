#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_live_latency_test.sh — how long does GO LIVE take to put a
# frame on air, and how many frames a second does it then deliver — as the load
# grows?
#
# The report this guards is a latency report with a SHAPE: "when I go live there's
# a few seconds' delay, and it gets worse while the service runs". A single
# measurement cannot see that shape, and neither can a stopwatch: the two numbers
# that describe it have to be taken from the engine's own frame lines.
#
# The scenario (Main.qml, VGR_LIVELAT_TEST=1) drives a LADDER of live sessions —
# by default 1 slide/2 blocks, then 7/3, then 20/5 — and prints when each started.
# Against those, this script reads the engine log for, per session:
#
#   first frame   `live output started` -> the first frame distributed
#                 (what the operator waits for after pressing GO LIVE)
#   cadence       frames per second while that session was live
#                 (what "under stress it increases" actually moves)
#
# and FAILS when the worst first frame or the lowest cadence crosses the budget:
#
#   LIVELAT_MAX_FIRST_FRAME_MS (default 1500)  the worst rung's first frame
#   LIVELAT_MIN_FPS            (default 5)     any rung's cadence
#
# The budgets are set from the measured baseline on this machine, with headroom:
# with the CPU rasterizer the live loop runs ~8 frames/sec and a 3-slide document
# takes ~1.0 s to its first frame (see docs/gui_thread_audit.md §8 and the live
# latency entry in tools/selftest/README.md). They are a REGRESSION gate, not a
# target — the trend printed on every run is what the next fix should move, and a
# rung that gets slower has to show up in the numbers before it can be fixed.
#
# Frames are counted on the PREVIEW output only (`__live_preview__`): every output
# gets its own frame each pass, so counting all of them would multiply the cadence
# by the output count. One output is one pass.
#
# VERDICT (last line, greppable):
#   LIVELAT: PASS cycles=<n> worstFirstFrame=<ms>ms minFps=<fps> budget=<ms>ms/<fps>fps
#   LIVELAT: FAIL launch | FAIL missing | FAIL cycles | FAIL firstframe | FAIL fps
#
# Usage:  bash tools/selftest/run_live_latency_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/livelat_test.log"
ELOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-120}"
MAX_FIRST_FRAME_MS="${LIVELAT_MAX_FIRST_FRAME_MS:-1500}"
MIN_FPS="${LIVELAT_MIN_FPS:-5}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "LIVELAT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

# engine.log accumulates: only the lines written after this point are this run's.
ELOG_START=0
[ -f "$ELOG" ] && ELOG_START="$(wc -l < "$ELOG")"

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_LIVELAT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_LIVELAT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-LIVELAT\] step=done" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done

# The scenario quits itself (a finished scenario in a still-running app reads like
# a pass otherwise), so the loop's return is waited for and then the process is
# killed rather than waited out — the exit path is run_quit_test.sh's subject, not
# this harness's.
QUIT=0
while [ "$QUIT" -lt 20 ]; do
    if grep -qa "app event loop returned" "$LOG" 2>/dev/null; then
        QUIT=1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    QUIT=$((QUIT + 1))
done
WAITED=0
while kill -0 "$RUN_PID" 2>/dev/null && [ "$WAITED" -lt 10 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
done
if kill -0 "$RUN_PID" 2>/dev/null; then
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
fi
wait "$RUN_PID" 2>/dev/null

echo "== the ladder the scenario drove =="
grep -a "\[SELFTEST-LIVELAT\]" "$LOG" 2>/dev/null

if ! grep -qa "\[SELFTEST-LIVELAT\] step=done" "$LOG" 2>/dev/null; then
    echo "(the ladder never finished — see $LOG)"
    echo "LIVELAT: FAIL missing"
    exit 3
fi

# Per live session, from the engine's own lines: start, first frame, frame count,
# window. Timestamps are HH:MM:SS.mmm, so the differences are already in seconds.
MEASURED="$(tail -n +$((ELOG_START + 1)) "$ELOG" 2>/dev/null | awk '
function t2s(t,   a) { split(t, a, ":"); return a[1]*3600 + a[2]*60 + a[3] }
function report() {
    if (!sess) return
    ++idx
    const window = (stop > start ? stop - start : 0)
    printf "cycle=%d start=%s firstFrameMs=%d frames=%d windowS=%.1f fps=%.2f\n",
           idx, started, first, frames, window, (window > 0 ? frames / window : 0)
}
/\[LiveOutputController\] live output started/ { report(); sess=1; started=$1; start=t2s($1); stop=0; first=0; frames=0; next }
/\[RenderOutputs\] output frame sanity: .__live_preview__./ {
    if (sess) { frames++; if (first == 0) first = int((t2s($1) - start) * 1000) }
    next
}
/\[LiveOutputController\] live output stopped/ { if (sess) { stop=t2s($1); report(); sess=0 } next }
END { if (sess) { if (stop == 0) stop = start; report() } }
')"

echo "== per-cycle measurement (engine's own frame lines) =="
echo "$MEASURED"

CYCLES="$(printf '%s\n' "$MEASURED" | grep -c "^cycle=")"
if [ "$CYCLES" -lt 1 ]; then
    echo "LIVELAT: FAIL cycles — no live session produced a frame (nothing was measured)"
    exit 4
fi
# A cycle with no frame at all still has to FAIL the gate rather than be skipped:
# never going live IS the worst case of "took too long to put a frame on air".
WORST="$(printf '%s\n' "$MEASURED" | grep -oE "firstFrameMs=[0-9]+" | cut -d= -f2 | sort -n | tail -1)"
TINY="$(printf '%s\n' "$MEASURED" | grep -oE "fps=[0-9.]+" | cut -d= -f2 | sort -n | head -1)"

if [ -z "${WORST:-}" ] || [ "$WORST" -gt "$MAX_FIRST_FRAME_MS" ]; then
    echo "LIVELAT: FAIL firstframe — the worst rung took ${WORST:-?} ms to its first frame (budget ${MAX_FIRST_FRAME_MS} ms)"
    exit 5
fi
# awk prints fps with two decimals; compare with awk so a fractional floor works.
if [ -z "${TINY:-}" ] || ! awk -v f="$TINY" -v b="$MIN_FPS" 'BEGIN { exit !(f + 0 >= b + 0) }'; then
    echo "LIVELAT: FAIL fps — the lowest cadence was ${TINY:-?} fps (floor ${MIN_FPS} fps)"
    exit 6
fi

echo "LIVELAT: PASS cycles=$CYCLES worstFirstFrame=${WORST}ms minFps=${TINY} budget=${MAX_FIRST_FRAME_MS}ms/${MIN_FPS}fps"
exit 0
