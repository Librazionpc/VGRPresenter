#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_soak_test.sh — the LONG-RUN LOAD TEST: does the app stay fast when it has been working for
# hours on a laptop whose CPU is already busy?
#
# The report behind it: "the main laptop that was pressing was running for hours, its CPU got stressed, the app started
# stalling and recovering, the UI was not fast again". A unit test cannot see that; a soak can.
#
# WHAT IT DOES
#   1. launches the app (debug logging, VGR_SELFTEST) with the "~soak<minutes>" scenario (qml/Main.qml, twSoak): a
#      busy Sunday in miniature — a ‹ › press every 700 ms, a new pick every ~14 s, Slide/Scroll flips, blackout
#      on/off, clear + go live again — while a 25 ms timer measures how late the interface thread runs it;
#   2. starts N CPU burners (node busy loops, normal priority) so the app competes for the machine like it did in
#      church, and samples the process every 15 s (private memory, working set, handles, threads, CPU seconds);
#   3. tools/selftest/soak_report.mjs reads the samples, the scenario's own lag lines and engine.log's GuiStall lines
#      and prints one row per minute, then a verdict.
#
# Usage:  bash tools/selftest/run_soak_test.sh [minutes=5] [burners=<cores/2>] [query=47-1100X]
#         burners=0 is the quiet baseline; the default loads half the cores, 16 loads all of a 16-thread machine.
#
# VERDICT (last line):
#   SOAK: PASS  memory / handles / threads flat, lag under the limits
#   SOAK: FAIL memory|handles|threads|lag|stalls|notrun — with the numbers above it
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${SELFTEST_BUILD_DIR:-$ROOT/build}"
APP="appVGRPresenterUI.exe"
MINUTES="${1:-5}"
CORES="$(nproc 2>/dev/null || echo 4)"
BURNERS="${2:-$((CORES / 2))}"
QUERY="${3:-47-1100X}"
LOG="$BUILD/soak_test.log"
CSV="$BUILD/soak_samples.csv"
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN=""; QT_BIN=""
fi
NODE_BIN="${NODE_BIN:-node}"

BUILD_WIN="$(cygpath -w "$BUILD" 2>/dev/null || echo "$BUILD")"
app_pids() {
    local out
    out="$(mktemp)"
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | ForEach-Object { \$_.Id }" \
        > "$out" 2>/dev/null
    tr -d '\r' < "$out"
    rm -f "$out"
}

echo "== stopping this build's instances =="
for _pid in $(app_pids); do
    powershell -NoProfile -Command "Stop-Process -Id $_pid -Force" >/dev/null 2>&1
done
sleep 2
rm -f "$LOG" "$CSV" "$BUILD/logs/engine.log" 2>/dev/null

echo "== soak: ${MINUTES} min, ${BURNERS} CPU burner(s) of ${CORES} cores, query '${QUERY}' =="
cd "$BUILD" || exit 2
SOAK_KIND="soak"
[ -n "${SOAK_MIX:-}" ] && SOAK_KIND="soakmix"   # SOAK_MIX=1: rotate Table row / Scripture via search / Show via search
VGR_SELFTEST=1 VGR_TABLEWALK_TEST=1 VGR_TABLEWALK_QUERY="live:${QUERY}~${SOAK_KIND}${MINUTES}" \
    PATH="$MINGW_BIN:$QT_BIN:$PATH" "./$APP" > "$LOG" 2>&1 < /dev/null &
APP_PID=$!

# A HANG IS CAUGHT RED-HANDED: tools/gdb_watch_hang.sh attaches the moment the app's own watchdog says the interface thread
# stopped pumping, and saves its native stack (build/gdb_capture_*.txt). The run then stops instead of sampling a frozen app.
WATCH_LOG="$BUILD/soak_gdbwatch.txt"
: > "$WATCH_LOG"
( sleep 20; bash "$HERE/../gdb_watch_hang.sh" $((MINUTES * 60 + 600)) > "$WATCH_LOG" 2>&1 ) &
WATCH_PID=$!
BURNER_PIDS=()
for _i in $(seq 1 "$BURNERS"); do
    "$NODE_BIN" -e 'for(;;){}' >/dev/null 2>&1 &
    BURNER_PIDS+=($!)
done
cleanup() {
    for b in "${BURNER_PIDS[@]:-}"; do [ -n "$b" ] && kill "$b" 2>/dev/null; done
    for _pid in $(app_pids); do powershell -NoProfile -Command "Stop-Process -Id $_pid -Force" >/dev/null 2>&1; done
}
trap cleanup EXIT

echo "t_s,private_mb,workingset_mb,handles,threads,cpu_s" > "$CSV"
START=$(date +%s)
DEADLINE=$((START + MINUTES * 60 + 240))
# A freshly linked exe can take a while to appear (the first launch is scanned): wait for the process before sampling,
# or the first empty answer ends the run as "notrun".
for _w in $(seq 1 60); do
    [ -n "$(app_pids)" ] && break
    sleep 1
done
while :; do
    NOW=$(date +%s)
    [ "$NOW" -ge "$DEADLINE" ] && break
    PIDS="$(app_pids)"
    [ -z "$PIDS" ] && break
    if grep -aq "GDB_WATCH: DONE" "$WATCH_LOG" 2>/dev/null; then
        echo "== THE INTERFACE THREAD HUNG — stack captured: $(grep -a 'GDB_WATCH: DONE' "$WATCH_LOG" | tail -1) =="
        HUNG=1
        break
    fi
    SAMPLE="$(powershell -NoProfile -Command "\$p = Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | Select-Object -First 1; if (\$p) { '{0:F1},{1:F1},{2},{3},{4:F1}' -f (\$p.PrivateMemorySize64/1MB), (\$p.WorkingSet64/1MB), \$p.HandleCount, \$p.Threads.Count, \$p.TotalProcessorTime.TotalSeconds }" 2>/dev/null | tr -d '\r')"
    [ -n "$SAMPLE" ] && echo "$((NOW - START)),$SAMPLE" >> "$CSV"
    sleep 15
done
cleanup
kill "$WATCH_PID" 2>/dev/null
trap - EXIT

echo "== scenario lines =="
if [ -n "${HUNG:-}" ]; then
    echo "SOAK: FAIL hang — the interface thread stopped responding; see the capture named above"
    exit 1
fi
grep -a "\[SELFTEST-SOAK\]" "$LOG" | tail -6 | cut -c1-220
"$NODE_BIN" "$HERE/soak_report.mjs" "$CSV" "$LOG" "$BUILD/logs/engine.log" "$BURNERS"
exit $?
