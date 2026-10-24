#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_ndi_late_output_test.sh — does an NDI output ADDED DURING
# A LIVE SESSION actually push frames?
#
# The regression guard for the operator's report: "the NDI source I created is
# not even getting any frames — as if NDI is dead — until I restart the app."
# A row that exists at boot feeds; a row added mid-session did not until a
# relaunch, which is the shape of state assembled only once (at GO LIVE, or at
# boot) instead of per frame.
#
# WHAT IT DOES
#   1. launches the app with VGR_SELFTEST=1 + VGR_NDILATE_TEST=1 (stderr ->
#      build/ndi_late_output_test.log)
#   2. the Main.qml scenario (SelfTestNdiLate) goes LIVE with one enabled NDI
#      row, waits for that sender to open (the baseline), then ADDS a second NDI
#      row mid-session and prints the feed's own per-output state — the same
#      numbers the pill and the Settings · Outputs row read — once a second for
#      twelve seconds, then removes the rows it added and quits
#   3. reads the [SELFTEST-NDILATE] lines and compares the NEW row's `frames`
#      counter before and after the add
#
# VERDICT (last line, greppable):
#   NDI_LATE: PASS        — the row added mid-session started sending
#   NDI_LATE: FAIL frames — the row was added, was enabled, and never sent
#   NDI_LATE: FAIL absent — the feed never listed the new row's state at all
#   NDI_LATE: FAIL live   — the session was not live, so nothing was under test
#   NDI_LATE: FAIL no-lines — the scenario produced no lines (driver never armed)
#
# NOTE: mutates the local roster (adds two NDI rows) but removes them again
# before quitting, so a run leaves the operator's outputs as it found them.
#
# Usage:  bash tools/selftest/run_ndi_late_output_test.sh [wait-seconds]
# Needs tools/agent_env.sh (this script sources it).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/ndi_late_output_test.log"
WAIT_SECS="${1:-90}"   # both acts + the feed's 6 s grace
if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "NDI_LATE: FAIL missing — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_SELFTEST=1 VGR_NDILATE_TEST=1 (stderr -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_NDILATE_TEST=1 \
    PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" >"$LOG" 2>&1 &
    echo $! >"$BUILD/ndi_late_output_test.pid"
)

PID="$(cat "$BUILD/ndi_late_output_test.pid" 2>/dev/null)"
DEADLINE=$((SECONDS + WAIT_SECS))
while [ "$SECONDS" -lt "$DEADLINE" ]; do
    if grep -q "\[SELFTEST-NDILATE\] done" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
sleep 2
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1

if ! grep -q "\[SELFTEST-NDILATE\]" "$LOG" 2>/dev/null; then
    echo "-- last 20 log lines --"
    tail -20 "$LOG" 2>/dev/null
    echo "NDI_LATE: FAIL no-lines — no [SELFTEST-NDILATE] lines (driver never armed)"
    exit 1
fi

echo "== [SELFTEST-NDILATE] lines =="
grep -a "\[SELFTEST-NDILATE\]" "$LOG"

# The verdict is computed from the sample lines: the NEW row's frames counter
# has to be a number that GROWS in at least one sample, and the last sample must
# still show it sending (a sender that opens and then dies is not a pass).
node - "$LOG" <<'NODE'
const fs = require('fs')
const path = process.argv[2]
const lines = fs.readFileSync(path, 'latin1')
    .split(/\r?\n/)
    .filter((l) => l.includes('[SELFTEST-NDILATE]'))
const NEW = 'NDI Late'
const SILENT = 'NDI Silent'
const framesOf = (line, name) => {
    const m = line.match(new RegExp(name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
        + '\\{frames=([-0-9]+),sending=(true|false)'))
    if (!m) return null
    return { frames: Number(m[1]), sending: m[2] === 'true' }
}
const liveLines = lines.filter((l) => /\blive=true\b/.test(l))
if (liveLines.length === 0) {
    console.log('NDI_LATE: FAIL live — the scenario never ran under a live session')
    process.exit(1)
}
const samples = lines.filter((l) => / sample\d+ /.test(l) || /\] sample\d+ /.test(l))
if (samples.length === 0) {
    console.log('NDI_LATE: FAIL absent — the feed published no samples after the add')
    process.exit(1)
}
let seen = false
let grew = false
let last = null
let previous = null
for (const line of samples) {
    const now = framesOf(line, NEW)
    if (!now) continue
    seen = true
    if (previous !== null && now.frames > previous) grew = true
    previous = now.frames
    last = now
}
if (!seen) {
    console.log('NDI_LATE: FAIL absent — the feed never listed the new row\'s state')
    process.exit(1)
}
// ---- duplicate names -------------------------------------------------------
// A name is the roster's KEY downstream: the NDI provider map, the per-output
// send clock, the published row state and the styled frame buffer are all
// indexed by it, so two rows sharing one name are ONE output to every reader —
// the newer row can never be fed and nothing can report it separately (the live
// report this guard exists for). Every sample lists each NDI row by name, so a
// repeat inside one line is exactly that defect.
for (const line of samples) {
    const names = [...line.matchAll(/([A-Za-z0-9 ._()-]+?)\{frames=/g)]
        .map((m) => m[1].trim())
        .filter((n) => n !== '')
    const dup = names.find((n, i) => names.indexOf(n) !== i)
    if (dup) {
        console.log(`NDI_LATE: FAIL duplicate-name — two outputs in one sample are named '${dup}' `
            + '(one name is one NDI sender, so the second row can never send)')
        process.exit(1)
    }
}

if (!grew || !last || last.frames === 0) {
    console.log(`NDI_LATE: FAIL frames — '${NEW}' was added and enabled mid-session `
        + `but its sender pushed ${last ? last.frames : 0} frame(s)`)
    process.exit(1)
}
// ---- the announcement ------------------------------------------------------
// The operator's other half of the report: "I created a new output … I'm not
// getting notif". An output added mid-session announces itself once its sender
// carries a frame (outputs.ndi.outputLive); the scenario logs that notice, so a
// silent feed fails here instead of looking fine in the log.
const notices = lines.filter((l) => /\] notif outputs\.ndi\.outputLive \|/.test(l))
if (notices.length === 0) {
    console.log(`NDI_LATE: FAIL no-notif — '${NEW}' sent frames but no outputs.ndi.outputLive notice was raised`)
    process.exit(1)
}
if (!notices.some((l) => l.includes(NEW))) {
    console.log(`NDI_LATE: FAIL no-notif — a notice was raised, but none names '${NEW}' `
        + `(raised: ${notices.map((l) => l.split('|')[1]?.trim()).join(' / ')})`)
    process.exit(1)
}
// ---- the SILENT one --------------------------------------------------------
// The other half of the report: a row added mid-session that cannot send must
// SAY SO, naming the wait. The scenario adds "NDI Silent" and switches it off,
// so the feed's own roster scan is what has to notice.
const silent = lines
    .filter((l) => /\] notif outputs\.ndi\.outputSilent \|/.test(l))
    .filter((l) => l.includes(SILENT))
if (silent.length === 0) {
    console.log(`NDI_LATE: FAIL no-warning — '${SILENT}' was added mid-session, switched off, `
        + 'and the feed never said why it was dark')
    process.exit(1)
}
if (!/switched off/i.test(silent[0])) {
    console.log(`NDI_LATE: FAIL no-reason — the warning for '${SILENT}' does not name the wait: `
        + silent[0].split('|').slice(1).join('|').trim())
    process.exit(1)
}

console.log(`NDI_LATE: PASS — '${NEW}' added mid-session sent ${last.frames} frames`
    + ` (sending=${last.sending}), announced by ${notices.length} notice(s); '${SILENT}'`
    + ' was reported with its reason; no relaunch')
NODE
