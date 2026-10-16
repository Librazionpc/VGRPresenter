#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_show_io_test.sh — do the show document JOBS actually land?
#
# ShowService's document reads and writes are QUEUED (see its header): each answers
# with a token, and the verdict arrives later as documentJobFinished. That contract
# cannot be seen from a build: a save that never writes its file and a save whose
# verdict never arrives both look exactly like nothing happening. So this drives the
# whole chain in the REAL app, against REAL files, and checks both halves — the
# verdict each step reported AND the artifact on disk:
#
#   save   a new two-slide show saved to build/showio_probe.vgr: the verdict must
#          arrive (not just be requested), the show must read clean afterwards, and
#          the file must exist when the verdict says so;
#   open   that same file opened back through the call the show screen uses, and the
#          payload must carry its slides;
#   peek   the centre page's read: a MISS must answer `pending` (not "no such show"),
#          the answer must land on the published revision, and a WRITE to that show
#          (the timer) must DROP it so the next read is the new file — not the old
#          answer, which is what a published cache gets wrong;
#   timer  the next timer applied and then read back out of the peek;
#   copy   the recovery copy, the one write nobody asked for.
#
# The files are deleted before the launch, so their presence afterwards is this
# run's own work and nothing else.
#
# Two things this checks that a file on disk cannot: that publishing a peek answer
# MOVES `peekRevision` (the centre page binds to it, so a value that never changes
# leaves a binding nothing to re-evaluate — it sat at 0 before this harness existed),
# and that the scenario's own quit TAKES (main.cpp's `app event loop returned` line),
# because a finished scenario in a still-running app reads exactly like a pass.
#
# VERDICT (last line, greppable):
#   SHOWIO: PASS save=ok open=ok peek=ok timer=ok copy=ok steps=<n>
#   SHOWIO: FAIL launch | FAIL missing | FAIL save | FAIL nosavefile | FAIL open
#           | FAIL peek | FAIL pending | FAIL timer | FAIL copy | FAIL nocopyfile
#           | FAIL revision | FAIL quit
#
# Usage:  bash tools/selftest/run_show_io_test.sh [wait-seconds]  (default 60)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/showio_test.log"
WAIT_SECS="${1:-60}"
SHOW_FILE="showio_probe.vgr"
COPY_FILE="showio_probe_recovery.vgr"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "SHOWIO: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Nothing from an earlier run may be mistaken for this one's work.
rm -f "$BUILD/$SHOW_FILE" "$BUILD/$COPY_FILE"

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_SHOWIO_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SHOWIO_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-SHOWIO\] step=result" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done

# The scenario quits ITSELF, and that quit is checked, not assumed: "the scenario
# finished" and "the app is leaving" are different claims, and the verdict lines
# above are all printed while the app is still up. main.cpp writes
# `app event loop returned with code 0` the moment `app.exec()` returns — the line
# the heartbeat in engine.log stops on — so it is the evidence that the quit took.
# MEASURED because it once did not: with `SelfTest.quit()` called straight from the
# show-io job-verdict handler the engine logged that line never, and the interface
# thread's 5 s heartbeat kept ticking for the ~80 s this wait watched (Main.qml
# quits from its own Timer tick for that reason — see the comment there).
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

# Bounded exit wait, the shape run_canvas_cursor_test.sh uses: the loop has already
# returned, and the PROCESS teardown after that can outlast the scenario itself
# (Qt's DXGI vsync service and the Win32 dispatcher shutting down — measured here at
# ~150 s). An unbounded `wait` blocks the REPORTING half of this script on a
# teardown that has nothing left to prove, so the grace period is short and the
# force-kill is stated rather than hidden.
WAITED=0
while kill -0 "$RUN_PID" 2>/dev/null && [ "$WAITED" -lt 10 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
done
FORCED=0
if kill -0 "$RUN_PID" 2>/dev/null; then
    FORCED=1
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
fi
wait "$RUN_PID" 2>/dev/null
if [ "$FORCED" = "1" ]; then
    echo "== the event loop had returned; the process was still tearing down ${WAITED}s later (force-killed, expected) =="
fi

echo "== every step's verdict, as the app reported it =="
grep -a "\[SELFTEST-SHOWIO\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-SHOWIO\] step=result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-SHOWIO result line — the scenario never finished; see $LOG)"
    echo "SHOWIO: FAIL missing"
    exit 3
fi
if [ "$QUIT" = "0" ]; then
    echo "SHOWIO: FAIL quit — the scenario finished and the app's event loop never returned (no 'app event loop returned' line in $LOG after 20s)"
    exit 14
fi

SAVE="$(echo "$RESULT" | grep -oE 'save=[^ ]*' | cut -d= -f2)"
OPEN="$(echo "$RESULT" | grep -oE 'open=[^ ]*' | cut -d= -f2)"
PEEK="$(echo "$RESULT" | grep -oE 'peek=[^ ]*' | cut -d= -f2)"
TIMER="$(echo "$RESULT" | grep -oE 'timer=[^ ]*' | cut -d= -f2)"
COPY="$(echo "$RESULT" | grep -oE 'copy=[^ ]*' | cut -d= -f2)"
STEPS="$(grep -ac "\[SELFTEST-SHOWIO\] step=" "$LOG" 2>/dev/null)"

if [ "$SAVE" != "true" ]; then
    echo "SHOWIO: FAIL save — the save's verdict did not report success (see the step lines above)"
    exit 4
fi
if [ ! -s "$BUILD/$SHOW_FILE" ]; then
    echo "SHOWIO: FAIL nosavefile — the save reported success and $BUILD/$SHOW_FILE is not there"
    exit 5
fi
if [ "$OPEN" != "true" ]; then
    echo "SHOWIO: FAIL open — opening the file back through ShowService did not report success"
    exit 6
fi
if [ "$PEEK" != "true" ]; then
    echo "SHOWIO: FAIL peek — the centre page's read never answered with the show's slides"
    exit 7
fi
# The published-peek contract: the FIRST read of a file nobody has read yet must say
# `pending`, not "there is no such show" — ShowCenter distinguishes the two, and a
# miss that answers like a failure would blank the centre page.
if ! grep -qa "\[SELFTEST-SHOWIO\] step=peek1 ok=false pending=true" "$LOG"; then
    echo "SHOWIO: FAIL pending — a first peek of an unread show did not answer 'pending' (see the peek1 line)"
    exit 8
fi
# The timer is a WRITE to that show: the published answer must go, and the read after
# it must be the new file. Reading the OLD answer here is exactly the bug a published
# cache gets wrong, so it is checked rather than assumed.
if [ "$TIMER" != "true" ]; then
    echo "SHOWIO: FAIL timer — the next timer was not applied (see the timer and peek4 lines)"
    exit 9
fi
if ! grep -qa "\[SELFTEST-SHOWIO\] step=peek4 .*timerApplied=true" "$LOG"; then
    echo "SHOWIO: FAIL timer — the peek after the timer did not read the new file (see the peek3/peek4 lines)"
    exit 10
fi
# The published peek's revision must MOVE when an answer is published: ShowCenter
# binds to it, so a "changed" signal whose value never moves is not something a
# binding can be trusted to re-evaluate. It never moved before this harness existed
# (publishPeek only emitted), which is exactly why it is asserted and not just
# printed.
REV="$(grep -aoE 'step=peek2 .*revision=[0-9]+' "$LOG" 2>/dev/null | grep -oE 'revision=[0-9]+' | head -1 | cut -d= -f2)"
if [ -z "$REV" ] || [ "$REV" -eq 0 ]; then
    echo "SHOWIO: FAIL revision — the published peek answer left peekRevision at '${REV:-missing}' (ShowCenter reads it to re-read)"
    exit 13
fi
if [ "$COPY" != "true" ]; then
    echo "SHOWIO: FAIL copy — the recovery copy's verdict did not report success"
    exit 11
fi
if [ ! -s "$BUILD/$COPY_FILE" ]; then
    echo "SHOWIO: FAIL nocopyfile — the recovery copy reported success and $BUILD/$COPY_FILE is not there"
    exit 12
fi

echo "SHOWIO: PASS save=ok open=ok peek=ok timer=ok copy=ok steps=$STEPS"
exit 0
