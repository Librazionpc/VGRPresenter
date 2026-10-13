#!/usr/bin/env bash
# ============================================================================
# tools/gdb_capture.sh — attach gdb to the running VGR app and dump the native
# stack of every thread. This is the tool for the "edit a live transparent
# template and the app hangs" bug: when the GUI thread stalls, the stack tells
# us whether it is BLOCKED (deadlock: a lock/join/syscall) or merely SLOW
# (race: spinning in a rebuild), which decides the fix.
#
# Usage:
#   tools/gdb_capture.sh              # find appVGRPresenterUI.exe and dump
#   tools/gdb_capture.sh <pid>        # attach to a specific pid
#
# Output: build/gdb_capture_<pid>_<stamp>.txt (all threads, `bt`).
# The last line is greppable:
#   GDB_CAPTURE: DONE <file>          exit 0
#   GDB_CAPTURE: FAIL no-process      exit 2
#   GDB_CAPTURE: FAIL gdb rc=<n>      exit 3
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
GDB="${GDB_BIN:-/c/msys64/ucrt64/bin/gdb.exe}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
APP="appVGRPresenterUI.exe"

[ -x "$GDB" ] || GDB="$(command -v gdb 2>/dev/null || echo "$GDB")"
if [ ! -x "$GDB" ] && [ ! -f "$GDB" ]; then
    echo "GDB_CAPTURE: FAIL gdb — not found at $GDB (set GDB_BIN)"
    exit 3
fi

PID="${1:-}"
if [ -z "$PID" ]; then
    PID="$(powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Id" \
        2>/dev/null | tr -d '\r')"
fi
if [ -z "$PID" ]; then
    echo "GDB_CAPTURE: FAIL no-process — $APP is not running"
    exit 2
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="$ROOT/build/gdb_capture_${PID}_${STAMP}.txt"
echo "== attaching gdb to pid $PID -> $OUT =="

# -batch: read commands, run, exit. We detach (not kill) so a still-useful
# process is left as we found it.
PATH="$MINGW_BIN:$QT_BIN:$PATH" "$GDB" -p "$PID" -batch \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex "set width 0" \
    -ex "set height 0" \
    -ex "info threads" \
    -ex "thread apply all bt" \
    -ex "detach" \
    >"$OUT" 2>&1
RC=$?

if [ "$RC" -ne 0 ]; then
    echo "gdb rc=$RC — tail of capture:"
    tail -n 20 "$OUT"
    echo "GDB_CAPTURE: FAIL gdb rc=$RC"
    exit 3
fi

THREADS="$(grep -cE '^Thread [0-9]+' "$OUT" 2>/dev/null || echo 0)"
echo "threads dumped: $THREADS"
echo "GDB_CAPTURE: DONE $OUT"
exit 0
