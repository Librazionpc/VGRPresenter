#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_quit_test.sh — does closing the window with unsaved work and
# a live output actually end the app, and does the teardown say what it cost?
#
# Closing the window is the one user path that ends the process, and none of it is
# visible to a build. The chain has four links, and each one fails in a way that
# looks like something else:
#
#   guard   the unsaved-changes guard must REFUSE the first close. If it does not,
#           the operator loses the show — silently, because the window just goes;
#   answer  the dialog's Save must actually reach the file. The save is a QUEUED
#           document job (ShowService), so its VERDICT is what sets closeConfirmed
#           and closes the window: a verdict that never arrives leaves the app
#           running with a visible window (the user's "it won't close");
#   quit    the event loop must return. A finished dialog in a window that never
#           closed reads exactly like a pass, so main.cpp's
#           `app event loop returned with code 0` is asserted, not assumed;
#   teardown the process must then GO. Measured before this harness existed: the
#           loop returns within a second and the process took ~150 s more. Each
#           aboutToQuit step is now timed into engine.log (`Exit` /
#           `aboutToQuit: <step> took <n> ms`), so this reads those lines and
#           reports how much of the tail they account for — the difference is the
#           stack teardown after main (the QML engine, the windows, Qt), which is
#           named in the report instead of guessed at.
#
# An output is LIVE for the whole of it: that is the case every "it hangs when I
# close it" report is about, and the NDI/encoder joins in the exit path only have
# anything to do when something is on air.
#
# The show is saved to a REAL file twice — once to give it a path at all (so the
# close-time save needs no native save-path picker, which cannot be driven from
# here) and once through the dialog — and the file's presence afterwards is this
# run's own work, not an earlier one's: it is deleted before the launch.
#
# VERDICT (last line, greppable):
#   QUIT: PASS guard=refused save=ok quit=yes teardown=<s>s handlers=<n>steps
#   QUIT: FAIL launch | FAIL missing | FAIL seed | FAIL armed | FAIL guard
#         | FAIL dialog | FAIL savefile | FAIL quit | FAIL noquitstep
#
# Usage:  bash tools/selftest/run_quit_test.sh [wait-seconds] [teardown-seconds]
#         (defaults: 90 for the scenario, 240 for the teardown measurement)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/quit_test.log"
# The aboutToQuit step timings are written by the engine's own logger, which
# appends to engine.log (the console copy of a line is not guaranteed — only some
# tags reach stdout). engine.log ACCUMULATES across runs, so the line count is
# taken before the launch and only what follows belongs to this run.
ELOG="$BUILD/logs/engine.log"
ELOG_START=0
[ -f "$ELOG" ] && ELOG_START="$(wc -l < "$ELOG")"
WAIT_SECS="${1:-90}"
# The teardown is the thing being MEASURED, so this is not a grace period to hide
# a slow exit behind: it is the window the measurement covers, and a run that
# exceeds it is reported as still-alive rather than silently killed at 10 s the
# way the other harnesses can afford to.
QUIT_WAIT="${2:-240}"
SHOW_FILE="quit_probe.vgr"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "QUIT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Nothing from an earlier run may be mistaken for this one's work.
rm -f "$BUILD/$SHOW_FILE"

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_QUIT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_QUIT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-QUIT\] step=answer" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done

# The quit is the point of this harness, so it is waited for (unlike the other
# harnesses, which can kill as soon as their scenario's verdict is in). The loop
# returning is main.cpp's own line, written the moment `app.exec()` returns.
QUIT=0
while [ "$QUIT" -lt 30 ]; do
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

# How long the PROCESS takes to go after its loop returned — the measurement the
# audit left open, in seconds, taken from the moment the line appeared.
TEARDOWN=0
GONE=0
if [ "$QUIT" = "1" ]; then
    while [ "$TEARDOWN" -lt "$QUIT_WAIT" ]; do
        if ! kill -0 "$RUN_PID" 2>/dev/null; then
            GONE=1
            break
        fi
        sleep 1
        TEARDOWN=$((TEARDOWN + 1))
    done
fi

FORCED=0
if kill -0 "$RUN_PID" 2>/dev/null; then
    FORCED=1
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
fi
wait "$RUN_PID" 2>/dev/null
if [ "$FORCED" = "1" ]; then
    echo "== the process was still alive ${TEARDOWN}s after the event loop returned (force-killed) =="
fi

echo "== every step the scenario reported =="
grep -a "\[SELFTEST-QUIT\]" "$LOG" 2>/dev/null
exit_lines() { tail -n +$((ELOG_START + 1)) "$ELOG" 2>/dev/null; }
# The two files carry different halves of the exit path, and the reason is worth
# knowing: until EngineBridge.shutdown runs, a line goes to engine.log (the
# engine's own logger) and NOT to the console; after it, the engine's logger is
# gone and the same call reaches stdout only. So the step TIMINGS are in
# engine.log while the two markers that come after the engine is down — `main:
# leaving` and the post-routine line — are in the console log. Reading one file for
# both is what made the first fixed run report that the tail's marker "never
# landed" when the line was right there in the other file.
echo "== what each aboutToQuit step cost, and where the tail after them goes =="
echo "   (timings from $ELOG — this run's lines only; markers from the console log)"
exit_lines | grep -a "aboutToQuit\|NDI worker teardown" 2>/dev/null
grep -a "app event loop returned\|main: leaving\|QCoreApplication teardown reached" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

# The measurement is reported BEFORE any verdict below, so a failure still carries
# the number it was run to produce (the first run of this harness reported
# `FAIL missing` and said nothing about the teardown it had just measured).
STEPS="$(exit_lines | grep -ac "aboutToQuit: .* took .* ms")"
echo "== measured: the event loop returned, and the process took ${TEARDOWN}s more to go"
echo "==           the aboutToQuit steps account for $STEPS timing(s); the rest is the teardown after main =="
if [ "$TEARDOWN" -gt 10 ]; then
    echo "== NOTE: ${TEARDOWN}s after the loop returned is the teardown this harness measures —"
    echo "==       compare it against the Exit timings above to see who owns it =="
fi

ANSWER="$(grep -a "\[SELFTEST-QUIT\] step=answer" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$ANSWER" ]; then
    echo "(no SELFTEST-QUIT answer line — the scenario never got as far as the dialog; see $LOG)"
    echo "QUIT: FAIL missing"
    exit 3
fi
if ! grep -qa "\[SELFTEST-QUIT\] step=seeded ok=true" "$LOG"; then
    echo "QUIT: FAIL seed — the show was never given a file, so the close-time save could not be judged"
    exit 4
fi
if ! grep -qa "\[SELFTEST-QUIT\] step=armed .*dirty=true" "$LOG"; then
    echo "QUIT: FAIL armed — the show was not dirty when the window was closed (the guard would not speak)"
    exit 5
fi
if ! grep -qa "\[SELFTEST-QUIT\] step=armed .*live=true" "$LOG"; then
    echo "QUIT: FAIL armed — no output was live when the window was closed (the case this harness exists for)"
    exit 6
fi
# The guard: the first close must be refused, and the dialog must be up. A close
# that went straight through is the silent-data-loss shape.
if ! grep -qa "\[SELFTEST-QUIT\] step=close1 .*refused=true" "$LOG"; then
    echo "QUIT: FAIL guard — closing with unsaved changes was NOT refused (see the close1 line)"
    exit 7
fi
if ! grep -qa "\[SELFTEST-QUIT\] step=close1 .*dialog=true" "$LOG"; then
    echo "QUIT: FAIL dialog — the close was refused and no 'save changes?' dialog appeared"
    exit 8
fi
# The save that gates the close must have reached the file. This is the queued
# job's verdict doing its work: window closed, file written — both, not either.
# The save that gates the close must have reached the file, and its VERDICT must
# have said so — the window closing is that verdict's work, not a coincidence of
# the click landing.
if ! grep -qa "\[SELFTEST-QUIT\] step=saved ok=true file=true" "$LOG"; then
    echo "QUIT: FAIL savefile — the close-time save never reported ok=true with the file on disk"
    exit 9
fi
if [ ! -s "$BUILD/$SHOW_FILE" ]; then
    echo "QUIT: FAIL savefile — the close-time save did not write $BUILD/$SHOW_FILE"
    exit 10
fi
if [ "$QUIT" = "0" ]; then
    echo "QUIT: FAIL quit — the window was closed and the app's event loop never returned (no 'app event loop returned' line after 30s)"
    exit 11
fi
# Every aboutToQuit step reports its cost; a missing line means the exit path ran
# something that cannot be attributed (or the timing was lost with the logger).
if [ "$STEPS" -lt 6 ]; then
    echo "QUIT: FAIL noquitstep — only $STEPS aboutToQuit step timing(s) in $ELOG (expected at least 6)"
    exit 12
fi
if ! grep -qa "main: leaving" "$LOG"; then
    echo "QUIT: FAIL noquitstep — the 'main: leaving' marker never landed (the tail cannot be measured at all)"
    exit 13
fi
if ! grep -qa "QCoreApplication teardown reached" "$LOG"; then
    echo "QUIT: FAIL noquitstep — the post-routine marker never landed, so the QML/window half of the tail cannot be told from Qt's"
    exit 14
fi
# The wrong-thread destruction Qt used to report on every quit after a live
# session — `QObject::killTimer: Timers cannot be stopped from another thread` /
# `QObject::~QObject: Timers cannot be stopped from another thread` — must be
# gone. The NDI worker owns the feed's QTimer, so it is now destroyed on its own
# thread (and that teardown says so, which is the line checked below). Asserted
# rather than eyeballed: it is a WARNING, so nothing else in this harness would
# notice it coming back.
# Checked in BOTH files: Qt's warning reaches the console log and the engine log
# depending on what is still alive when it fires.
if grep -qa "cannot be stopped from another thread\|Cannot stop timers" "$LOG" \
   || exit_lines | grep -qa "cannot be stopped from another thread\|Cannot stop timers"; then
    echo "QUIT: FAIL wrongthread — a QObject was destroyed from a thread that does not own it (see the QObject warning above)"
    exit 16
fi
# The fix's own evidence line, from the feed thread as its loop ends (a teardown
# whose only proof is an ABSENT warning is one nobody can check later — and the
# first version of this fix, which queued the teardown to a loop that was about to
# exit, really did never run).
# (engine.log, not the console: it is logged while the engine's logger is still
# alive, and this file's own lines only — see exit_lines above.)
if ! exit_lines | grep -qa "NDI worker teardown on the feed thread"; then
    echo "QUIT: FAIL wrongthread — the NDI worker's teardown never reported running on the feed thread (see the teardown line)"
    exit 17
fi
if [ "$GONE" = "0" ] && [ "$FORCED" = "0" ]; then
    # Cannot happen (the wait above ends either on exit or with the process still
    # alive → FORCED), kept so a future edit to the waits cannot report a pass for
    # a process that never went.
    echo "QUIT: FAIL noquitstep — the process state after the loop is unknown"
    exit 15
fi

# The teardown number is DATA, not a pass/fail: it is what this harness exists to
# show, and a threshold would only encode today's measurement as a rule.
echo "QUIT: PASS guard=refused save=ok quit=yes teardown=${TEARDOWN}s handlers=${STEPS}steps"
exit 0
