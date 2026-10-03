#!/usr/bin/env bash
# ============================================================================
# tools/ndi_golive_probe.sh — run the GO LIVE + NDI crash repro and report.
#
# The regression check for the 2026-10-01 crash: five byte-identical records
# in %AppData%\VGR\VGRPresenter\crashes\crash.log.* reading
#
#   signal=0 (filesystem error: cannot make absolute path: Invalid argument [])
#
# Cause: std::filesystem::absolute() throws on an EMPTY path on Windows
# (GetFullPathNameW -> ERROR_INVALID_PARAMETER), and the throw escaped into
# std::terminate. Fixed in platform/common/Filesystem.cpp. This script proves
# the flow that triggered it now survives.
#
# WHAT IT DOES
#   1. records the crash-log / dump counts BEFORE the run
#   2. launches the app with VGR_SELFTEST=1 (which arms the Timers in
#      qml/Main.qml) and stderr captured to build/ndi_golive_probe.log
#   3. waits for the run to finish (the last stage quits the app)
#   4. compares the crash-log / dump counts AFTER
#
# VERDICT (last line, greppable):
#   NDI_GOLIVE: PASS   — reached "survived", no new crash log, no new dump
#   NDI_GOLIVE: FAIL crash     — a new crash.log.<epoch> appeared
#   NDI_GOLIVE: FAIL dump      — a new .dmp appeared
#   NDI_GOLIVE: FAIL incomplete— the run never reached its last stage
#
# Needs tools/agent_env.sh on PATH (this script sources it).
# ============================================================================
set -u
# busybox ash has no $SECONDS — keep our own counter.
TICK=0

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
PROBE_LOG="$BUILD/ndi_golive_probe.log"
CRASH_DIR="$HOME/AppData/Roaming/VGR/VGRPresenter/crashes"
DUMP_DIR="$HOME/AppData/Local/CrashDumps"
WAIT_SECS="${1:-45}"

. "$HERE/agent_env.sh"

count_crashes() { ls "$CRASH_DIR" 2>/dev/null | grep -c '^crash\.log\.' ; }
count_dumps()   { ls "$DUMP_DIR"  2>/dev/null | grep -c 'appVGRPresenterUI' ; }

BEFORE_CRASH="$(count_crashes)"
BEFORE_DUMP="$(count_dumps)"
echo "== before: $BEFORE_CRASH crash log(s), $BEFORE_DUMP dump(s) =="

if [ ! -f "$BUILD/$APP" ]; then
    echo "NDI_GOLIVE: FAIL incomplete — $BUILD/$APP missing (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 (stderr -> $PROBE_LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 PATH="C:/msys64/ucrt64/bin:C:/Qt/6.11.1/mingw_64/bin:$PATH" \
        "./$APP" > "$PROBE_LOG" 2>&1
) &
RUN_PID=$!

# The run quits itself at the last stage; poll for that rather than sleeping
# the full budget when it finishes early.
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "survived GO LIVE + NDI" "$PROBE_LOG" 2>/dev/null; then
        sleep 3   # let the app finish quitting and flush the crash handler
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait $RUN_PID 2>/dev/null

echo "== probe output =="
grep -a "SELFTEST-NDI" "$PROBE_LOG" 2>/dev/null || echo "(no SELFTEST-NDI lines — the driver never armed)"

AFTER_CRASH="$(count_crashes)"
AFTER_DUMP="$(count_dumps)"

if [ "$AFTER_CRASH" -gt "$BEFORE_CRASH" ]; then
    NEWEST="$(ls -t "$CRASH_DIR"/crash.log.* 2>/dev/null | head -1)"
    echo "== NEW CRASH RECORD: $NEWEST =="
    cat "$NEWEST" 2>/dev/null | head -12
    echo "NDI_GOLIVE: FAIL crash"
    exit 3
fi
if [ "$AFTER_DUMP" -gt "$BEFORE_DUMP" ]; then
    echo "== NEW DUMP (crash log empty but Windows caught it) =="
    ls -t "$DUMP_DIR" 2>/dev/null | grep appVGRPresenterUI | head -2
    echo "NDI_GOLIVE: FAIL dump"
    exit 4
fi
if ! grep -qa "survived GO LIVE + NDI" "$PROBE_LOG" 2>/dev/null; then
    echo "NDI_GOLIVE: FAIL incomplete — never reached the final stage"
    exit 5
fi

echo "== after: $AFTER_CRASH crash log(s), $AFTER_DUMP dump(s) — unchanged =="
echo "NDI_GOLIVE: PASS"
exit 0
