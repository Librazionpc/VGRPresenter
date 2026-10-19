#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_input_rate_test.sh — how many frames a second does a HELD
# CAMERA actually reach the compositor with?
#
# The report this guards: "the video bus is lagging both hdmi and ndi". Both
# outputs render ONE compositor, and a camera bus fills it through the service's
# pump (LiveOutputService) — so the pump's frame rate IS the ceiling every
# output inherits. It used to be capped twice over: the PAL's preview tap
# published one JPEG every 66 ms (~15 fps) and the pump ticked at 66 ms as well,
# which made a 30 fps camera reach the screens at 15 fps and a 60 fps one at 15.
# Neither cap was visible anywhere: no log line, no counter, nothing to measure.
#
# THE APP MEASURES, THIS SCRIPT JUDGES — same split as run_live_latency_test.sh.
# The scenario (Main.qml, VGR_INPUTRATE_TEST=1) takes the first camera on the
# roster and reports the service's own frame revisions over a 5 s window:
#
#     [SELFTEST-INPUTRATE] measured label="..." revs=N fps=X live=bool
#
# inputRev advances once per frame that actually moved, so X is the feed's real
# rate (a still camera still delivers fresh frames — the encode of a static
# frame is not byte-identical, and a live tap's revision counts the store).
#
# BUDGET
#   INPUTRATE_MIN_FPS   default 20   under the 30 fps every capture stack here
#                                   produces, and comfortably over the 15 fps
#                                   ceiling the defect imposed (a 5 s window with
#                                   ±1 frame of jitter still fails if it regressed)
#
# VERDICT (last line, greppable):
#   INPUTRATE: PASS fps=<fps> revs=<n> label=<label> budget=<fps>fps
#   INPUTRATE: FAIL launch | FAIL missing | FAIL refused | FAIL nomedia
#             | FAIL fps
#
# Usage:  bash tools/selftest/run_input_rate_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/input_rate_test.log"
WAIT_SECS="${1:-120}"
MIN_FPS="${INPUTRATE_MIN_FPS:-20}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "INPUTRATE: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_INPUTRATE_TEST=1 (log -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_INPUTRATE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-INPUTRATE\] step=done\|\[SELFTEST-INPUTRATE\] done\|\[SELFTEST-INPUTRATE\] nomedia" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done

# The scenario quits itself; wait for the event loop, then make sure it is gone.
WAITED=0
while kill -0 "$RUN_PID" 2>/dev/null && [ "$WAITED" -lt 15 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
done
if kill -0 "$RUN_PID" 2>/dev/null; then
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
fi
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
grep -a "\[SELFTEST-INPUTRATE\]" "$LOG" 2>/dev/null

if grep -qa "\[SELFTEST-INPUTRATE\] nomedia" "$LOG" 2>/dev/null; then
    echo "INPUTRATE: FAIL nomedia — no camera on the roster, nothing was measured"
    exit 3
fi

LINE="$(grep -a "\[SELFTEST-INPUTRATE\] measured" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$LINE" ]; then
    if grep -qa "\[SELFTEST-INPUTRATE\] take" "$LOG" 2>/dev/null; then
        echo "(the take was asked for but no measurement came back — see $LOG)"
        echo "INPUTRATE: FAIL refused — the camera never produced frames"
        exit 4
    fi
    echo "(the scenario never ran — see $LOG)"
    echo "INPUTRATE: FAIL missing"
    exit 5
fi

FPS="$(printf '%s\n' "$LINE" | grep -o 'fps=[0-9.]*' | cut -d= -f2)"
REVS="$(printf '%s\n' "$LINE" | grep -o 'revs=[0-9]*' | cut -d= -f2)"
LABEL="$(printf '%s\n' "$LINE" | sed -n 's/.*label="\([^"]*\)".*/\1/p')"

if [ -z "${FPS:-}" ]; then
    echo "INPUTRATE: FAIL missing — the measured line carried no fps (${LINE})"
    exit 5
fi
if ! awk -v f="$FPS" -v b="$MIN_FPS" 'BEGIN { exit !(f >= b) }'; then
    echo "INPUTRATE: FAIL fps — '${LABEL}' reached the compositor at ${FPS} fps (floor ${MIN_FPS} fps)"
    echo "            (both outputs render that feed, so this is the rate HDMI and NDI would show)"
    exit 6
fi

echo "INPUTRATE: PASS fps=${FPS} revs=${REVS} label=${LABEL} budget=${MIN_FPS}fps"
exit 0
