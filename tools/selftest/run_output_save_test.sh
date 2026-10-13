#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_output_save_test.sh — does the Edit Output dialog's Save
# actually save (and close) once styles exist?
#
# The regression guard for "the moment i add styles that is when the save button
# doesn't respond": the dialog stayed open over Settings with a "Style not saved"
# notice, so a press of Save read as dead. Everything else in the form HAD been
# written by then, which is what made it look like nothing happened at all.
#
# The dialog's Save writes the row it was OPENED for (Settings · Outputs · Edit),
# and only two paths can end it without closing:
#   * the row it was opened for moved / went away, so the write hit nothing; or
#   * the style id it carried is no longer in the style roster, so the model
#     refused the assignment.
# Both were silent apart from the toast, and both are reachable exactly when a
# style roster exists — an output with no style never touches the style write.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_OUTSAVE_TEST=1 (stderr ->
#      build/output_save_test.log)
#   2. the Main.qml scenario (SelfTestOutputSave) walks the operator's own steps:
#      add a style from Settings · Styles, open Settings · Outputs, open the first
#      output's Edit dialog, press the REAL Save button — twice, in the two shapes
#      the report showed: A) the output already wears the style (the pick from an
#      earlier session, "New Style 2" in the field) and Save is a no-change save;
#      B) the style is changed in the dialog and then saved
#   3. reads the [SELFTEST-OUTSAVE] lines (pressA/resultA, pressB/resultB) and
#      checks, for BOTH passes, that the dialog closed (closed=true), the output
#      ended up wearing the wanted style (styled=true), and the dialog is really
#      gone (dialog=false)
#
# VERDICT (last line, greppable):
#   OUTPUT_SAVE: PASS         — Save closed the dialog and the style landed, both passes
#   OUTPUT_SAVE: FAIL stuck   — a Save left the dialog open (the reported bug)
#   OUTPUT_SAVE: FAIL style   — the dialog closed but the style never landed
#   OUTPUT_SAVE: FAIL missing — the scenario produced no lines (the driver never armed)
#
# NOTE: not a read-only probe. The scenario MUTATES the local roster (adds a
# style, assigns it to the first output) — same as the operator's own steps.
#
# Usage:  bash tools/selftest/run_output_save_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/output_save_test.log"
WAIT_SECS="${1:-40}"
# The app needs the MSYS2 UCRT64 + Qt bin dirs on PATH or it dies with a
# DLL-missing popup before any scenario could arm. Same overridable names as
# dev_cycle.sh; the POSIX form works in the developer's MSYS shell, the native
# form in the agent shell (busybox ash), so both are tried.
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "OUTPUT_SAVE: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_OUTSAVE_TEST=1 (stderr -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_OUTSAVE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app when it is done; poll for that rather than always
# sleeping the full budget.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-OUTSAVE\] resultB" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario output =="
LINES="$(grep -a "\[SELFTEST-OUTSAVE\]" "$LOG" 2>/dev/null)"
if [ -z "$LINES" ]; then
    echo "(no SELFTEST-OUTSAVE lines — the driver never armed; see $LOG)"
    echo "OUTPUT_SAVE: FAIL missing"
    exit 3
fi
echo "$LINES"

RESULT_A="$(printf '%s\n' "$LINES" | grep " resultA " | tail -1)"
RESULT_B="$(printf '%s\n' "$LINES" | grep " resultB " | tail -1)"
if [ -z "$RESULT_A" ] || [ -z "$RESULT_B" ]; then
    echo "OUTPUT_SAVE: FAIL missing — a pass never reported (A='$RESULT_A' B='$RESULT_B'; see $LOG)"
    exit 3
fi

# The dialog must be GONE afterwards in both passes. A Save that ran to the end
# of saveEdit() closes it; one that returned early leaves it up, which is exactly
# what the operator saw as an unresponsive button.
STUCK="$(printf '%s\n%s\n' "$RESULT_A" "$RESULT_B" | grep -v "closed=true")"
if [ -n "$STUCK" ]; then
    echo "== a Save left the dialog open =="
    echo "$STUCK"
    echo "OUTPUT_SAVE: FAIL stuck"
    exit 4
fi
SHOWN="$(printf '%s\n%s\n' "$RESULT_A" "$RESULT_B" | grep -v "dialog=false")"
if [ -n "$SHOWN" ]; then
    echo "== the dialog is still on screen after Save =="
    echo "$SHOWN"
    echo "OUTPUT_SAVE: FAIL stuck"
    exit 5
fi

# ...and the style it carried must be on the output. A closed dialog that dropped
# the pick is the second half of the same bug (the write silently refused).
UNSTYLED="$(printf '%s\n%s\n' "$RESULT_A" "$RESULT_B" | grep -v "styled=true")"
if [ -n "$UNSTYLED" ]; then
    echo "== the style never landed on the output =="
    echo "$UNSTYLED"
    echo "OUTPUT_SAVE: FAIL style"
    exit 6
fi

echo "OUTPUT_SAVE: PASS"
exit 0
