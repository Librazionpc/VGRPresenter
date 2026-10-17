#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_newbus_test.sh — is a bus created WITH its source?
#
# The rule: Media · Inputs' "New bus" must not be able to make a bus that feeds
# nothing. A bus with no source is a card that can never light, a monitor that
# has to refuse the click, and — the user's own concern — a signal path the app
# might open a device for / keep alive for nothing.
#
# This drives the REAL controls (a real synthetic click on the FAB, on Create, on
# the first source card and on Cancel) and judges six facts:
#   * Create with NOTHING picked makes no bus (the roster does not grow), and the
#     dialog says why;
#   * Create with a card picked makes exactly ONE bus;
#   * that bus comes up with its route already made (routedVideoSources /
#     routedAudioInputs non-empty, and its type follows the source);
#   * creating it opened NO device: no card-preview tap, no meter, no output take;
#   * every source card's LIGHT agrees with the probe behind it (inputAvailable /
#     inputDelivery) — the half a click-through flow cannot see;
#   * Cancel CLOSES the picker (Esc goes through the same cancelled()), so the
#     modal cannot be left on screen with dead buttons.
# It also writes build/newbus_dialog.png and build/newbus_dialog_picked.png so the
# card grid can be looked at, and removes the row it added.
#
# VERDICT (last line, greppable):
#   NEWBUS: PASS blocked-empty created routed no-tap pick=video#0
#   NEWBUS: FAIL launch | missing | dialogs
#
# Usage:  bash tools/selftest/run_newbus_test.sh [wait-seconds]   (default 90)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/newbus_test.log"
WAIT_SECS="${1:-90}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "NEWBUS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG" "$BUILD/newbus_dialog.png" "$BUILD/newbus_dialog_picked.png"
echo "== launching with VGR_SELFTEST=1 VGR_NEWBUS_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_NEWBUS_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario opens the pane, opens the dialog, creates one bus and quits.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-NEWBUS\] result" "$LOG" 2>/dev/null; then
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
grep -a "\[SELFTEST-NEWBUS\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
# The log's own INFO lines mention QML (teardown notices), so the app's severity
# prefix is part of the pattern — a bare "QML" matched them and read as errors.
grep -aE "\[(WARN|ERROR|CRITICAL)\]|RangeError|Cannot read|TypeError|is not defined" "$LOG" 2>/dev/null | head -8
echo "== grabs =="
ls -l "$BUILD/newbus_dialog.png" "$BUILD/newbus_dialog_picked.png" 2>/dev/null

RESULT="$(grep -a "\[SELFTEST-NEWBUS\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-NEWBUS result line — the scenario never finished; see $LOG)"
    echo "NEWBUS: FAIL missing"
    exit 3
fi
if grep -qa "pane=missing" "$LOG"; then
    echo "NEWBUS: FAIL launch — the Media pane was never found"
    exit 4
fi

DETAIL="$(grep -a "\[SELFTEST-NEWBUS\] blockedEmpty=" "$LOG" 2>/dev/null | tail -1)"
if echo "$RESULT" | grep -q "PASS"; then
    echo "NEWBUS: PASS $(echo "$DETAIL" | sed -e 's/.*blockedEmpty=/blocked-empty /' \
        -e 's/ created=/ created /' -e 's/ routed=/ routed /' -e 's/ noTapOpened=/ no-tap /' \
        -e 's/ pick=/ pick=/' | tr -s ' ')"
    exit 0
fi
echo "NEWBUS: FAIL dialogs — a bus was made without a source, came up unrouted, a light disagreed with its probe, or Cancel did not close the picker ($DETAIL)"
exit 5
