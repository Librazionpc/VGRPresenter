#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_gpu_toggle_test.sh — does Settings · General's
# "Hardware rendering (GPU)" switch really change what the engine draws with?
#
# The report it answers: "can add a toggle in general so in case a user want to
# turn it by them self but it should [be] software by default as not all laptops
# have gpu". Two claims, and only the RUNNING app can show either:
#
#   1. the row is there, and its toggle really writes the setting;
#   2. the ENGINE really changes renderer — and, on a machine whose GPU cannot be
#      used, the row says so instead of echoing the switch back.
#
# What this drives (the `selfTestGpu*` block in qml/Main.qml, armed by
# VGR_GPU_TEST=1): open Settings · General through the app's own openSettings,
# scroll the Preferences card into view, click the row's real toggle, read back
# the stored value, the engine's own answer (SettingsService.rendererInUse /
# rendererOnGpu) and the sentence the ROW DRAWS under the switch, then click it
# back and confirm the operator's setting is where it was found.
#
# Judged:
#   * `restored=true`  — the borrow was returned (a run that left the GPU path on
#                        would hand the next launch the black picture this switch
#                        exists to avoid);
#   * `onEngine=<name>` — the engine ANSWERED (it is asked off the interface
#                        thread, so this also proves the round-trip landed);
#   * `onConsistent=true` — the row's own sentence agreed with the backend that
#                        is really installed;
#   * `row="<label>"` — the row drew its own name off the setting definition (an
#                        empty label is a switch on an unnamed line, so it fails);
#   * with the switch restored to OFF, the engine is back on `software` — plus,
#   * an engine.log DELTA showing the switch really re-installed a backend
#     (`render backend: requested '…' -> active '…'`), which is the one piece of
#     evidence the QML side cannot produce by itself.
#
# VERDICT (last line, greppable):
#   GPU: PASS restored=yes on=<engine> off=<engine> switched=yes by=<click|setValue> row="<label>"
#   GPU: FAIL launch | FAIL missing | FAIL notrun | FAIL switch | FAIL restore | FAIL engine
#
# Usage:  bash tools/selftest/run_gpu_toggle_test.sh [wait-seconds]   (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/gpu_toggle_test.log"
ENGINE_LOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-120}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "GPU: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

# The engine log is APPENDED to across runs: everything below is judged on the
# lines this run adds, so an earlier session's switch can never decide it.
BEFORE=0
[ -f "$ENGINE_LOG" ] && BEFORE="$(wc -l < "$ENGINE_LOG")"

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_GPU_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_GPU_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario opens Settings, flips the switch twice and quits the app itself.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-GPU\] result" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-GPU\]" "$LOG" 2>/dev/null
echo "== the engine's own switch lines (this run only) =="
AFTER=0
[ -f "$ENGINE_LOG" ] && AFTER="$(wc -l < "$ENGINE_LOG")"
SWITCH_LINES="$(tail -n "+$((BEFORE + 1))" "$ENGINE_LOG" 2>/dev/null | grep -a 'render backend: requested')"
echo "$SWITCH_LINES"
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-GPU\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-GPU result line — the scenario never finished; see $LOG)"
    echo "GPU: FAIL notrun"
    exit 3
fi
if echo "$RESULT" | grep -q "missing=yes"; then
    echo "GPU: FAIL missing — $RESULT"
    exit 4
fi

field() { echo "$RESULT" | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2; }
RESTORED="$(field restored)"
WAS="$(field was)"
VALUE="$(field value)"
ENGINE="$(field engine)"
GPU="$(field gpu)"
ON_ENGINE="$(field onEngine)"
ON_CONSISTENT="$(field onConsistent)"
BY="$(field by)"
# LAST field on the line, so it keeps its spaces ("Hardware rendering (GPU)").
LABEL="$(echo "$RESULT" | sed -n 's/.*label=//p')"

if [ "$RESTORED" != "true" ] || [ "$VALUE" != "$WAS" ]; then
    echo "GPU: FAIL restore — the switch was left at $VALUE (it was $WAS): $RESULT"
    exit 5
fi
if [ -z "$ON_ENGINE" ]; then
    echo "GPU: FAIL engine — the engine never answered what it is drawing with: $RESULT"
    exit 6
fi
if [ "$ON_CONSISTENT" != "true" ]; then
    echo "GPU: FAIL switch — the row's own line did not match the backend installed: $RESULT"
    exit 7
fi
# The row must be SHOWING, not merely clickable: a blank label would leave a
# switch on an unnamed line, which is the report this scenario exists to answer.
if [ -z "$LABEL" ]; then
    echo "GPU: FAIL missing — the row drew no label (the setting never reached the page): $RESULT"
    exit 9
fi
# The default direction (the switch was OFF, which is the shipped default):
# restoring it must leave the engine on the software rasterizer.
if [ "$WAS" = "false" ] && { [ "$GPU" != "false" ] || [ "$ENGINE" != "software" ]; }; then
    echo "GPU: FAIL restore — switch off but the engine is still $ENGINE (gpu=$GPU): $RESULT"
    exit 8
fi

SWITCHED="no"
if [ -n "$SWITCH_LINES" ]; then
    SWITCHED="yes"
fi
echo "GPU: PASS restored=yes on=$ON_ENGINE off=$ENGINE switched=$SWITCHED by=${BY:-?} row=\"$LABEL\""
exit 0
