#!/usr/bin/env bash
# Run the unit suite ONE PHASE AT A TIME, each under its own timeout, and print a
# per-phase verdict.
#
# Why: the suite is a single process, so one wedged test (a deadlock, an
# unbounded spin) hangs every phase after it and hides the results of the ones
# that already passed. Running per phase puts a bound around each group instead,
# and the log names the phase that wedged.
#
# Usage: tools/run_unit_phases.sh [seconds-per-phase] [phase ...]
#   tools/run_unit_phases.sh            # every phase, 180 s each
#   tools/run_unit_phases.sh 60 core    # just `core`, 60 s
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/bps_unit_tests.exe"
LOG="${BPS_PHASE_LOG:-$ROOT/build/logs/unit_phases.log}"
LIMIT="${1:-180}"
shift || true

# The same PATH the app needs (see tools/dev_cycle.sh): the test binary links the
# engine, which links the MinGW/Qt runtimes.
export PATH="${MINGW_BIN:-/c/msys64/ucrt64/bin}:${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}:$PATH"

# Kept in step with the phase table in src/PresentationEngine/tests/unit/main.cpp.
PHASES=(
    adaptive bible broadcast cams core display flow import library media modules
    notify presentation production project recording rendering scene search
    settings song styles telemetry vgr
)
if [ "$#" -gt 0 ]; then PHASES=("$@"); fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"
failed=0
for phase in "${PHASES[@]}"; do
    start=$(date +%s)
    # Each phase gets its OWN file, appended to the shared log under a header:
    # a failing phase must be attributable, not just visible (grepping one big
    # log reports the wrong phase's failures).
    out="$LOG.$phase"
    ( cd "$ROOT/build" && timeout "$LIMIT" ./bps_unit_tests.exe "$phase" ) >"$out" 2>&1
    rc=$?
    took=$(( $(date +%s) - start ))
    case "$rc" in
    0) verdict="PASS" ;;
    124) verdict="TIMEOUT (${LIMIT}s) — wedged"; failed=1 ;;
    *) verdict="FAIL rc=$rc"; failed=1 ;;
    esac
    { printf '===== PHASE %s (%ss) %s =====\n' "$phase" "$took" "$verdict"; cat "$out"; } >>"$LOG"
    printf 'PHASE %-12s %7ss  %s\n' "$phase" "$took" "$verdict"
    if [ "$verdict" != "PASS" ]; then
        grep -c "^FAIL" "$out" | sed 's/^/    failures: /'
        grep -m 5 "^FAIL" "$out" | sed 's/^/    /'
    fi
done
printf 'UNIT_PHASES: %s (log: %s)\n' "$([ $failed -eq 0 ] && echo PASS || echo FAIL)" "$LOG"
exit $failed
