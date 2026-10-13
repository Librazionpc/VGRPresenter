#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_ui_warn_test.sh — are the two input-only QML warnings gone?
#
# Two warnings the app logged under real input, and that no property trace could
# see because they are produced by the input path itself:
#   * AppMenuBar — "Binding loop detected for property \"settingsFlyoutOpen\"",
#     raised while hovering the logo menu's "Settings" row (the flyout flies out
#     under the pointer and re-enters the flyout recompute).
#   * ReferencePane — "Cannot read property 'playPicked' of undefined", raised by
#     a verse double-click landing while the pane is being rebuilt (the reporting
#     session had just toggled table.splitLongVerses).
#
# The scenario drives both with the driver's REAL cursor moves and REAL
# synthetic clicks (see SelfTestDriver.h): click the logo, hover the Settings
# row, cross onto the flyout, switch to The Table tab, flip splitLongVerses,
# double-click a verse row. It prints its own evidence lines ([SELFTEST-UIWARN])
# including the flyout's own `visible` and whether a verse row existed at all.
#
# VERDICT (last line, greppable):
#   UIWARN: PASS                     — the sequence ran and neither warning appeared
#   UIWARN: FAIL bindingloop         — AppMenuBar still re-enters its recompute
#   UIWARN: FAIL playpicked          — ReferencePane still calls through a dead owner
#   UIWARN: FAIL notrun              — the scenario never reported (app never came up,
#                                      or the sequence died before its first step)
#   UIWARN: SKIP verses              — no warning, but no verse row was ever present,
#                                      so the double-click half was NOT exercised
#
# Usage:  bash tools/selftest/run_ui_warn_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/ui_warn_test.log"
ELOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-55}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "UIWARN: FAIL notrun — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Log-line DELTA, not the whole file: engine.log is appended across runs, so the
# warnings from earlier sessions must not decide this run.
BEFORE=$(wc -l < "$ELOG" 2>/dev/null || echo 0)
rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_UIWARN_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_UIWARN_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
APP_PID=$!

DEADLINE=$((SECONDS + WAIT_SECS))
while [ $SECONDS -lt $DEADLINE ]; do
    if grep -q "\[SELFTEST-UIWARN\] done" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 2
done
sleep 3   # let the last grabs/log flushes land
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
wait $APP_PID 2>/dev/null

echo "== scenario output =="
grep -a "\[SELFTEST-UIWARN\]" "$LOG" 2>/dev/null || echo "(none)"

echo "== warnings logged SINCE launch =="
tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -a "Binding loop detected for property \"settingsFlyoutOpen\"\|playPicked' of undefined" \
    | sed 's/^/  /' || true

if ! grep -qa "\[SELFTEST-UIWARN\] start" "$LOG" 2>/dev/null; then
    echo "UIWARN: FAIL notrun — the scenario never reported (check $LOG)"
    exit 2
fi

BINDING=$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -ac "Binding loop detected for property \"settingsFlyoutOpen\"")
PICKED=$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -ac "playPicked' of undefined")

if [ "${BINDING:-0}" != "0" ]; then
    echo "UIWARN: FAIL bindingloop — the menu bar still re-enters its settings-flyout recompute ($BINDING line(s))"
    exit 1
fi
if [ "${PICKED:-0}" != "0" ]; then
    echo "UIWARN: FAIL playpicked — a verse double-click still calls through a dead pane owner ($PICKED line(s))"
    exit 1
fi
if grep -qa "\[SELFTEST-UIWARN\] done" "$LOG" && ! grep -qa "verses=true" "$LOG"; then
    echo "UIWARN: SKIP verses — no verse row was present, so the double-click half was not exercised"
    exit 0
fi
if ! grep -qa "flyoutOpened=true" "$LOG"; then
    echo "UIWARN: FAIL notrun — the hover sequence never opened the settings flyout (the binding-loop half proves nothing)"
    exit 2
fi

echo "UIWARN: PASS"
