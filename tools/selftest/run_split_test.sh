#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_split_test.sh — is a dragged sidebar width really REMEMBERED?
#
# The regression guard for the main screen's resizable side columns. Those widths
# ride the settings store (session.projectsColumnWidth / outputsColumnWidth /
# templateColumnWidth / dockHeightTop) and are written by an idle timer in
# VGRPresenterMainScreen.qml. Both halves of that save path have been wrong before:
#
#   * the timer wrote ALL FOUR keys on every fire, so values nobody had touched were
#     stored — and because the overrides are BOUND to the store, that happened on every
#     LAUNCH too (four `[Settings] ... = -1` lines and a settings.json rewrite on a
#     clean start with no drag at all);
#   * a per-name key that was never declared ("shows.categoryTemplate.<name>") made
#     every write refuse with a toast instead of being remembered.
#
# The scenario presses the Projects divider, drags it, releases it, and then reads the
# value back out of the store — so it fails if the drag is not saved, if it is saved
# under the wrong key, or if the save is a no-op.
#
# VERDICT (last line, greppable):
#   SPLIT: PASS                    — the dragged width is in the store
#   SPLIT: FAIL not-saved          — the drag did not stick
#   SPLIT: FAIL launch             — the app never came up (see build/split_test.log)
#   SPLIT: FAIL missing            — the scenario never reported (see the log)
#
# Usage:  bash tools/selftest/run_split_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/split_test.log"
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
    echo "SPLIT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_SPLIT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SPLIT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself once it has read the store back.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-SPLIT\]" "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
LINE="$(grep -a "\[SELFTEST-SPLIT\]" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$LINE" ]; then
    echo "(no SELFTEST-SPLIT line — the driver never armed; see $LOG)"
    echo "SPLIT: FAIL launch"
    exit 3
fi
echo "$LINE"

if echo "$LINE" | grep -q "saved=true"; then
    echo "SPLIT: PASS"
    exit 0
fi
echo "SPLIT: FAIL not-saved"
exit 1
