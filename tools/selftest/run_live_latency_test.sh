#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_live_latency_test.sh — how long does GO LIVE take to put a
# frame on the monitor, and how many frames a second does the live loop then
# deliver — as slides, layers and outputs grow?
#
# The report this guards is a latency report with a SHAPE: "when I go live there's
# a delay, and it gets worse while the service runs / as the show grows". A single
# measurement cannot see that shape, so the app drives a LADDER of live sessions
# (Main.qml, VGR_LIVELAT_TEST=1) and MEASURES EACH ONE ITSELF:
#
#     first frame   GO LIVE -> the live preview's first new frame
#     cadence       frames the loop delivers per second with the window LOADED (see
#                   HOW THE WINDOW IS LOADED below for what that means, and why the
#                   load is not optional)
#
# and prints them as machine-readable `[SELFTEST-LIVELAT] step=measured ...` lines.
#
# THE APP MEASURES, THIS SCRIPT JUDGES — and the reason is worth keeping: the
# engine's own frame line (`output frame sanity`) is emitted when the presented
# state CHANGES, not per frame, and the live loop only rasterizes when the on-air
# scene changes (change-driven rendering, see LiveOutputController), so neither the
# log nor a static session carries a frame rate at all. The scenario reads the loop's
# own publish points instead — LiveOutputService.previewFramesNow() for the first
# frame (the main pass, the first thing the operator can see) and framesSentNow()
# for the cadence (the whole iteration, once every per-output pass has landed).
#
# HOW THE WINDOW IS LOADED (the app prints which, as `mode=`, on every rung):
#
#   overlay  an overlay taken on air — the live loop's change-driven skip and its
#            1Hz heartbeat are BOTH bypassed while the compositor has live content
#            (LiveOutputController::RenderOnce), so the loop rasterizes every
#            iteration it can and the delivered rate IS its throughput.
#   slides   the fallback when the overlay library is empty: the scenario advances
#            the on-air slide far faster than the loop can serve, so the frames that
#            come back are again the iterations it completed.
#
# BUDGETS (override by env). Both halves are gated, and both are set from the
# measured baseline on this machine (recorded with its own run in
# tools/selftest/README.md) as a REGRESSION gate, not as a target:
#
#   baseline (2026-10-16, three runs; 2 outputs enabled + 2 more added by the last
#   rung) — the SPREAD is what the budgets have to survive:
#     worst first frame 998 .. 1455 ms (the FIRST rung is always the worst: a cold
#                                      first raster, and the number that moves most)
#     cadence           0.68 .. 1.28 fps     drop 44 .. 47%
#
#   LIVELAT_MAX_FIRST_FRAME_MS  default 2000  the worst rung's first frame
#   LIVELAT_MIN_FPS             default 0.5   the lowest rung's cadence
#                                             (~35% under the lowest measured)
#   LIVELAT_MAX_FPS_DROP_PCT    default 60    how far the slowest rung may fall
#                                             below the fastest one, in percent
#                                             (the load-shape half: the last rung
#                                             already spends 44-47% by adding outputs,
#                                             and an absolute floor alone would pass
#                                             on a fast machine with a broken trend)
#
# Frames are counted on the whole live iteration (framesSentNow — main pass plus
# every per-output pass), while the first frame is timed on the EARLIER publish
# (previewFramesNow — the main pass, the first thing the operator can see).
#
# VERDICT (last line, greppable):
#   LIVELAT: PASS rungs=<n> worstFirstFrame=<ms>ms minFps=<fps> drop=<pct>%
#           budget=<ms>ms/<fps>fps/<pct>%
#   LIVELAT: FAIL launch | FAIL missing | FAIL cycles | FAIL firstframe
#           | FAIL nomove | FAIL fps | FAIL fpsdrop | FAIL roster
#
# Usage:  bash tools/selftest/run_live_latency_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/livelat_test.log"
WAIT_SECS="${1:-240}"
MAX_FIRST_FRAME_MS="${LIVELAT_MAX_FIRST_FRAME_MS:-2000}"
MIN_FPS="${LIVELAT_MIN_FPS:-0.5}"
MAX_DROP_PCT="${LIVELAT_MAX_FPS_DROP_PCT:-60}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "LIVELAT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_LIVELAT_TEST=1 (output -> $LOG) =="
echo "   ladder=${VGR_LIVELAT_LADDER:-2:2:1,8:3:1,20:5:1,20:5:4}" \
     "drive=${VGR_LIVELAT_DRIVE_MS:-150}ms window=${VGR_LIVELAT_WINDOW_MS:-8000}ms"
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
# a pass otherwise), so its exit is waited for and then the process is killed
# rather than waited out — the exit path is run_quit_test.sh's subject, not this
# harness's.
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

echo "== the ladder the app drove =="
grep -a "\[SELFTEST-LIVELAT\]" "$LOG" 2>/dev/null

if ! grep -qa "\[SELFTEST-LIVELAT\] step=done" "$LOG" 2>/dev/null; then
    echo "(the ladder never finished — see $LOG)"
    echo "LIVELAT: FAIL missing"
    exit 3
fi

# ---- the rows the app measured -------------------------------------------
# One row per rung:
#   n slides blocks outputs firstFrameMs frames changes windowMs fps mode
ROWS="$(awk '/\[SELFTEST-LIVELAT\] step=measured/ {
    for (i = 1; i <= NF; i++) {
        split($i, kv, "=")
        v[kv[1]] = kv[2]
    }
    # mode last on purpose: the columns before it keep their positions for the
    # verdict below.
    printf "%s %s %s %s %s %s %s %s %s %s\n", v["n"], v["slides"], v["blocks"],
           v["outputs"], v["firstFrameMs"], v["frames"], v["changes"],
           v["windowMs"], v["fps"], v["mode"]
}' "$LOG" 2>/dev/null)"

echo "== per-rung measurement (the app's own counters) =="
printf '%-5s %-7s %-7s %-8s %-13s %-9s %-8s %-9s %-9s %s\n' \
       rung slides blocks outputs firstFrameMs frames/reps windowMs fps mode comment
printf '%s\n' "$ROWS" | while IFS=' ' read -r n sl bl out ff fr ch win fps mode; do
    [ -z "${n:-}" ] && continue
    note=""
    [ "$ff" = "-1" ] && note="NEVER PUT A FRAME ON AIR"
    [ "${mode:-}" = "slides" ] && [ "$ch" = "0" ] && note="no change was ever asked for"
    printf '%-5s %-7s %-7s %-8s %-13s %-9s %-8s %-9s %-9s %s\n' \
           "$n" "$sl" "$bl" "$out" "$ff" "$fr/$ch" "$win" "$fps" "${mode:-?}" "$note"
done

RUNGS="$(printf '%s\n' "$ROWS" | grep -c '[0-9]')"
# The ladder's own count (a rung it skipped — a one-slide rung — is not expected to
# be measured at all): the app prints its plan, so the harness never has to
# re-derive it from the env string.
EXPECTED="$(grep -a '\[SELFTEST-LIVELAT\] step=plan' "$LOG" 2>/dev/null \
            | grep -o 'rungs=[0-9]*' | head -1 | cut -d= -f2)"
EXPECTED="${EXPECTED:-0}"
if [ "$RUNGS" -lt 1 ]; then
    echo "LIVELAT: FAIL cycles — no rung produced a measurement (nothing was measured)"
    exit 4
fi
if [ "$RUNGS" -lt "$EXPECTED" ]; then
    echo "LIVELAT: FAIL cycles — ${RUNGS} rung(s) measured, the ladder has ${EXPECTED}"
    exit 4
fi

# A rung in SLIDES mode whose driver never asked for a change measured nothing (its
# fps would be the static heartbeat, not the loop's cost) — that is a broken
# measurement, not a slow app, so it fails on its own. Overlay mode needs no driver:
# an overlay on air is itself the work.
if printf '%s\n' "$ROWS" | awk '{ if ($10 == "slides" && $7 + 0 <= 0) exit 1 }'; then :; else
    echo "LIVELAT: FAIL nomove — a rung asked for no change, so its cadence means nothing"
    exit 5
fi

# ---- the verdict ---------------------------------------------------------
# worst first frame (a rung that never put a frame on air is -1 and is the worst
# case by definition), lowest and highest cadence, and the drop between them.
VERDICT="$(printf '%s\n' "$ROWS" | awk -v maxff="$MAX_FIRST_FRAME_MS" \
                                      -v minfps="$MIN_FPS" \
                                      -v droppct="$MAX_DROP_PCT" '
BEGIN { worst = -1; low = -1; high = -1; never = 0 }
{
    ff = $5 + 0
    fps = $9 + 0
    if (ff < 0) { never = 1 } else if (ff > worst) worst = ff
    if (low < 0 || fps < low) low = fps
    if (fps > high) high = fps
}
END {
    if (never) worst = -1
    drop = (high > 0) ? (high - low) * 100.0 / high : 0
    printf "%s %s %.2f %.2f %.1f\n", worst, never, low, high, drop
}')"
read -r WORST NEVER LOW HIGH DROP <<< "$VERDICT"

if [ "$NEVER" = "1" ] || [ "$WORST" -lt 0 ] || [ "$WORST" -gt "$MAX_FIRST_FRAME_MS" ]; then
    if [ "$NEVER" = "1" ]; then
        echo "LIVELAT: FAIL firstframe — a rung never put a frame on air at all"
    else
        echo "LIVELAT: FAIL firstframe — the worst rung took ${WORST} ms to its first frame (budget ${MAX_FIRST_FRAME_MS} ms)"
    fi
    exit 6
fi
if ! awk -v f="$LOW" -v b="$MIN_FPS" 'BEGIN { exit !(f >= b) }'; then
    echo "LIVELAT: FAIL fps — the lowest cadence was ${LOW} fps (floor ${MIN_FPS} fps)"
    exit 7
fi
if ! awk -v d="$DROP" -v b="$MAX_DROP_PCT" 'BEGIN { exit !(d <= b) }'; then
    echo "LIVELAT: FAIL fpsdrop — the slowest rung kept only ${HIGH}→${LOW} fps (${DROP}% below the fastest, budget ${MAX_DROP_PCT}%)"
    exit 8
fi

# The roster the ladder grew has to be back: the outputs it added persist in the
# operator's store otherwise, and a selftest must not leave junk outputs behind.
if ! grep -qa "\[SELFTEST-LIVELAT\] step=restored" "$LOG" 2>/dev/null; then
    echo "LIVELAT: FAIL roster — the ladder grew the output roster and never put it back"
    exit 9
fi

echo "LIVELAT: PASS rungs=$RUNGS worstFirstFrame=${WORST}ms minFps=${LOW} maxFps=${HIGH} drop=${DROP}% budget=${MAX_FIRST_FRAME_MS}ms/${MIN_FPS}fps/${MAX_DROP_PCT}%"
exit 0
