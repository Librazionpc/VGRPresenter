#!/usr/bin/env bash
# ============================================================================
# tools/gdb_watch_hang.sh — catch the hang red-handed.
#
# The app already watches itself: tools/../CallTrace installs a HangWatchdog
# that writes a "HangWatchdog" line to build/logs/engine.log when the GUI
# thread stops pumping for ~5 s. This script tails that log and, the moment the
# line appears, runs tools/gdb_capture.sh against the live (hung) process —
# so we get the GUI thread's native stack AT the stall, not a guess.
#
# The watchdog writes from its own thread, so it still fires while the GUI
# thread is blocked (which is exactly the case we care about).
#
# Usage:
#   tools/gdb_watch_hang.sh [timeout-seconds]     # default 300
#
# Reproduce the bug while this runs (GO LIVE with the transparent template,
# then Edit template). On a stall it prints GDB_WATCH: HANG and the capture
# path. Greppable last line:
#   GDB_WATCH: DONE <file>     exit 0   (hang seen + captured)
#   GDB_WATCH: TIMEOUT         exit 4   (no hang within the window)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LOG="$ROOT/build/logs/engine.log"
TIMEOUT="${1:-300}"

if [ ! -f "$LOG" ]; then
    echo "GDB_WATCH: engine.log not found at $LOG — launch the app first"
    exit 2
fi

# Only lines written after we start count (an old stall must not fire us).
START_LINE="$(wc -l < "$LOG")"
echo "== watching $LOG for a new HangWatchdog line (within ${TIMEOUT}s) =="
echo "   (reproduce the hang now: GO LIVE with the template, then Edit template)"

ELAPSED=0
while [ "$ELAPSED" -lt "$TIMEOUT" ]; do
    # Stop when the app is gone: a hang that Windows already closed (Event Log
    # Id 1002) can no longer be attached to, and idling until the timeout would
    # leave a stray background process behind.
    if ! tasklist //FI "IMAGENAME eq appVGRPresenterUI.exe" //NH 2>/dev/null \
            | grep -qi "appVGRPresenterUI"; then
        echo "GDB_WATCH: app is no longer running — nothing to attach to"
        exit 3
    fi
    CUR_LINE="$(wc -l < "$LOG" 2>/dev/null || echo "$START_LINE")"
    if [ "$CUR_LINE" -gt "$START_LINE" ]; then
        if tail -n +$((START_LINE + 1)) "$LOG" | grep -aq "HangWatchdog"; then
            echo "GDB_WATCH: HANG — watchdog fired, attaching gdb immediately"
            echo "--- watchdog line ---"
            tail -n +$((START_LINE + 1)) "$LOG" | grep -a "HangWatchdog" | tail -1
            echo "---------------------"
            bash "$HERE/gdb_capture.sh"
            echo "GDB_WATCH: DONE — see the gdb_capture_*.txt named above"
            exit 0
        fi
        START_LINE="$CUR_LINE"
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

echo "GDB_WATCH: TIMEOUT — no hang within ${TIMEOUT}s"
exit 4
