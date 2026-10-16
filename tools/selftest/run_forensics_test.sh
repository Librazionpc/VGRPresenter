#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_forensics_test.sh — is the forensics verdict visible to the
# user, and is it visible only when it MATTERS?
#
# The startup probe (CrashHandler.h's VerifyForensicsTooling) proves at every
# launch that this machine can actually capture a crash, and it used to say so
# in engine.log and nowhere else — so the operator who most needed to know (the
# one whose crash left no evidence) was the one who would never read it. This
# script drives the two surfaces the verdict now reaches, through the REAL app:
#
#   * Settings · General · "Crash reports" — the row's own pill text and detail
#     line, looked up in the live item tree (so a row bound to the wrong
#     property fails rather than passing on the raw property value);
#   * the note that rides the crash-recovery notice — delivered at all, and
#     present in the notification centre, which is where the user reads it.
#
# TWO cases, because a verdict that is always loud is as wrong as one that is
# always quiet:
#
#   broken  — VGR_FORENSICS_DIR points at a scratch folder holding an EMPTY
#             analyze.bat: a packaged-looking layout that cannot read a report.
#             Required: the row says Incomplete, its detail is the probe's own
#             reason, and the note about the crash report IS delivered.
#   healthy — VGR_FORENSICS_DIR points at an empty folder: a build that ships no
#             capture tools, which is a WORKING configuration (the text record
#             and the minidump remain). Required: the row says Ready, and with a
#             recovered crash reported in the same session NO note is delivered.
#
# Both cases need a previous run that went down (the note only exists beside a
# crash report), so each seeds ONE crash record into the app's own crash log —
# the exact file the handlers append to. That record is consumed by the launch
# under test (reported once, then rotated aside); the script says so.
#
# VERDICT (last line, greppable):
#   FORENSICS: PASS broken=Incomplete+noted healthy=Ready+quiet
#   FORENSICS: FAIL launch | FAIL missing | FAIL verdict | FAIL row | FAIL note
#
# Usage:  bash tools/selftest/run_forensics_test.sh [wait-seconds-per-case]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
WAIT_SECS="${1:-150}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "FORENSICS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

CRASH_LOG="${APPDATA:-$HOME/AppData/Roaming}/VGR/VGRPresenter/crashes/crash.log"
SCRATCH="${TMPDIR:-/tmp}/vgr_forensics_selftest"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/broken" "$SCRATCH/healthy"
# An empty analyze.bat is enough for the broken case: the probe treats a layout
# as packaged when the gdb folder OR analyze.bat is there, and an empty one is
# exactly \"a report could not be read on a machine with no developer tools\".
# (No tools/gdb beside it, so whether the gdb half also reports depends on this
# machine's PATH — the judgement rides the half this script created.)
: > "$SCRATCH/broken/analyze.bat"
echo "== broken tooling layout:  $SCRATCH/broken (analyze.bat present and empty) =="
echo "== healthy tooling layout: $SCRATCH/healthy (empty — no capture tools shipped) =="

stop_app() {
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
    sleep 2
}

seed_crash() {
    mkdir -p "$(dirname "$CRASH_LOG")"
    {
        echo "---- UNHANDLED SIGNAL ----"
        echo "signal=11 (segv) at=$(date +%Y-%m-%dT%H:%M:%S)"
        echo "  last-call: selftest seeded record"
    } >> "$CRASH_LOG"
}

# run_case <name> <forensics-dir> <log>
run_case() {
    local name="$1" dir="$2" log="$3" expect_state="$4"
    stop_app
    echo "== [$name] seeding one crash record into $CRASH_LOG (consumed by this launch) =="
    seed_crash
    rm -f "$log" "$BUILD/forensics_card_${expect_state}.png"
    echo "== [$name] launching with VGR_SELFTEST=1 VGR_FORENSICS_TEST=1 (output -> $log) =="
    (
        cd "$BUILD" || exit 1
        VGR_SELFTEST=1 VGR_FORENSICS_TEST=1 VGR_FORENSICS_DIR="$dir" \
            PATH="$MINGW_BIN:$QT_BIN:$PATH" \
            "./$APP" > "$log" 2>&1
    ) &
    RUN_PID=$!

    # The scenario waits out the probe, opens Settings · General, reads the row
    # and quits the app itself.
    local tick=0
    while [ "$tick" -lt "$WAIT_SECS" ]; do
        if grep -qa "\[SELFTEST-FORENSICS\] state=" "$log" 2>/dev/null; then
            sleep 1
            break
        fi
        if ! kill -0 "$RUN_PID" 2>/dev/null; then
            break
        fi
        sleep 1
        tick=$((tick + 1))
    done
    wait "$RUN_PID" 2>/dev/null

    echo "== [$name] the verdict's own lines (probe -> engine log) =="
    grep -a "Forensics" "$BUILD/logs/engine.log" 2>/dev/null | tail -2
    echo "== [$name] scenario =="
    grep -a "\[SELFTEST-FORENSICS\]" "$log" 2>/dev/null
    echo "== [$name] QML errors (should be none) =="
    grep -aE "QML|RangeError|Cannot read|TypeError" "$log" 2>/dev/null | head -8
    # The card itself, grabbed by the scenario and named after the state it drew:
    # a passing run leaves a picture of the row it just read, not only a line
    # about it, and both cases' pictures survive side by side.
    local card="$BUILD/forensics_card_${4}.png"
    echo "== [$name] the Settings card it read: $card =="
    [ -f "$card" ] || fail grab "the scenario produced no card grab ($card)" 7
}

fail() {
    echo "FORENSICS: FAIL $1 — $2"
    exit "$3"
}

# ---- Case 1: a machine that cannot capture a crash must SAY so -------------
run_case broken "$SCRATCH/broken" "$BUILD/forensics_broken.log" incomplete
RESULT="$(grep -a "\[SELFTEST-FORENSICS\] state=" "$BUILD/forensics_broken.log" 2>/dev/null | tail -1)"
[ -n "$RESULT" ] || fail missing "the broken case never finished its scenario" 3
echo "$RESULT" | grep -q "state=incomplete" \
    || fail verdict "the broken layout's verdict never reached QML as incomplete ($RESULT)" 4
echo "$RESULT" | grep -q 'pill="Incomplete"' \
    || fail row "the Settings row does not draw the verdict ($RESULT)" 5
echo "$RESULT" | grep -q "analyze.bat" \
    || fail row "the row's detail line is not the probe's own reason ($RESULT)" 5
echo "$RESULT" | grep -q "recovered=true" \
    || fail note "the seeded crash record was not reported as a recovered crash ($RESULT)" 6
echo "$RESULT" | grep -q "noted=true" \
    || fail note "the note was never delivered beside the crash report ($RESULT)" 6
echo "$RESULT" | grep -q "inToast=true" \
    || fail note "the note never reached the notification centre ($RESULT)" 6

# ---- Case 2: a working configuration must stay quiet -----------------------
# Same seeded crash, same session shape — the only difference is tooling that
# works. The row says Ready and NO note is pushed: a machine that can report a
# crash must not be told anything about being unable to.
run_case healthy "$SCRATCH/healthy" "$BUILD/forensics_healthy.log" ready
RESULT="$(grep -a "\[SELFTEST-FORENSICS\] state=" "$BUILD/forensics_healthy.log" 2>/dev/null | tail -1)"
[ -n "$RESULT" ] || fail missing "the healthy case never finished its scenario" 3
echo "$RESULT" | grep -q "state=ready" \
    || fail verdict "a working tooling layout did not read as ready ($RESULT)" 4
echo "$RESULT" | grep -q 'pill="Ready"' \
    || fail row "the Settings row does not draw the ready verdict ($RESULT)" 5
echo "$RESULT" | grep -q "recovered=true" \
    || fail note "the healthy case's crash record was not reported ($RESULT)" 6
echo "$RESULT" | grep -q "noted=false" \
    || fail note "a working configuration pushed the note anyway ($RESULT)" 6
echo "$RESULT" | grep -q "inToast=false" \
    || fail note "a working configuration's note reached the notification centre ($RESULT)" 6

echo "FORENSICS: PASS broken=Incomplete+noted healthy=Ready+quiet"
exit 0
