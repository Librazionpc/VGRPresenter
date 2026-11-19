#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_ndi_still_test.sh — does a STILL picture keep reaching a real NDI receiver?
#
# The app sends a picture that has not changed for a second at a slower "still" beat instead of 60 times a second
# (LiveOutputService::kNdiStillFps): one NDI sender cost ~2 CPU cores for a lower third that was doing nothing. This
# test puts a passage on air, leaves it alone (`~soakhold`), and receives the Lower Third sender with the real NDI
# runtime (tools/selftest/ndirate.cpp, built on first use), second by second, for a while after the picture has gone still.
#
# VERDICT (last line):
#   NDI_STILL: PASS  frames kept arriving every second; the longest gap stayed under the limit
#   NDI_STILL: FAIL dropout (a second with no frame) | gap (a gap over 400 ms) | notrun
# Usage: bash tools/selftest/run_ndi_still_test.sh [seconds=30]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
SECS="${1:-30}"
LOG="$BUILD/ndi_still_test.log"
OUT="$BUILD/ndirate.exe"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"

if [ ! -f "$OUT" ] || [ "$HERE/ndirate.cpp" -nt "$OUT" ]; then
    echo "== building ndirate =="
    PATH="$MINGW_BIN:$PATH" g++ -O1 -o "$OUT" "$HERE/ndirate.cpp" || { echo "NDI_STILL: FAIL notrun (ndirate build)"; exit 2; }
fi

BUILD_WIN="$(cygpath -w "$BUILD")"
stop_app() {
    for _pid in $(powershell -NoProfile -Command "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$BUILD_WIN') } | ForEach-Object { \$_.Id }" | tr -d '\r'); do
        powershell -NoProfile -Command "Stop-Process -Id $_pid -Force" >/dev/null 2>&1
    done
}
stop_app; sleep 2
rm -f "$LOG"
cd "$BUILD" || exit 2
# 3 minutes is far longer than the probe: the app is stopped when the receiver is done.
VGR_SELFTEST=1 VGR_TABLEWALK_TEST=1 VGR_TABLEWALK_QUERY="live:47-1100X~soakhold3" \
    PATH="$MINGW_BIN:$QT_BIN:$PATH" "./$APP" > "$LOG" 2>&1 < /dev/null &
APP_PID=$!
echo "== waiting for the passage to go on air =="
for _i in $(seq 1 90); do
    grep -aq "SELFTEST-SOAK\] hold" "$LOG" 2>/dev/null && break
    sleep 2
done
grep -aq "SELFTEST-SOAK\] hold" "$LOG" 2>/dev/null || { stop_app; echo "NDI_STILL: FAIL notrun (the passage never went on air)"; exit 3; }
echo "== the picture is on air and left alone; receiving the Lower Third for ${SECS}s =="
sleep 3
RESULT="$(PATH="$MINGW_BIN:$PATH" "$OUT" "Lower Third" "$SECS" 2>&1)"
echo "$RESULT" | tail -$((SECS + 3))
stop_app
LINE="$(echo "$RESULT" | grep -a "NDIRATE:" | tail -1)"
[ -n "$LINE" ] || { echo "NDI_STILL: FAIL notrun (no receiver summary)"; exit 3; }
DROP="$(echo "$LINE" | sed -n 's/.*dropouts=\([0-9]*\).*/\1/p')"
GAP="$(echo "$LINE" | sed -n 's/.*maxGapMs=\([0-9]*\).*/\1/p')"
[ "${DROP:-1}" -gt 0 ] && { echo "NDI_STILL: FAIL dropout ($DROP second(s) with no frame) — $LINE"; exit 1; }
[ "${GAP:-99999}" -gt 400 ] && { echo "NDI_STILL: FAIL gap (${GAP} ms between two frames) — $LINE"; exit 1; }
echo "NDI_STILL: PASS — $LINE"
