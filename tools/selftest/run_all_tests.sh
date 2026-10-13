#!/usr/bin/env bash
# Run EVERY tools/selftest scenario, one after another, and print a verdict line
# per scenario plus one final greppable one.
#
# Why a runner: each scenario is its own script (it stops any running instance,
# launches the app under VGR_SELFTEST=1, drives one area of the UI, greps the
# app's own log and prints its own verdict). Running them by hand means
# remembering which exist and reading nineteen tails; run in one loop, a failure
# is visible next to the scripts that passed instead of scrolling away behind
# the next banner. Each script keeps its own timeout default (the table in
# tools/selftest/README.md), so nothing here has to second-guess it.
#
# Usage:  bash tools/selftest/run_all_tests.sh [name-fragment ...]
#   bash tools/selftest/run_all_tests.sh                  # every scenario
#   bash tools/selftest/run_all_tests.sh style split      # only those two
#
# Each scenario's full output is appended to build/logs/selftest_all.log.
#
# VERDICT (last line, greppable):  SELFTEST_ALL: PASS | FAIL (n of m failed)
# Exit: 0 every scenario passed, 1 at least one failed, 2 the app is not built.
#
# NOTE: the scenarios MUTATE state (a style's background image, a project, a
# splitter width). Each restores what it changed, but they are meant to be run
# against a scratch profile, not a session you are working in.
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="$BUILD/appVGRPresenterUI.exe"
LOG="$BUILD/logs/selftest_all.log"

if [ ! -f "$APP" ]; then
    echo "SELFTEST_ALL: FAIL — $APP not found (build first: tools/dev_cycle.sh --build-only)"
    exit 2
fi

# Name fragments to run; none = every scenario.
FILTER=()
while [ "$#" -gt 0 ]; do
    FILTER+=("$1")
    shift
done

mkdir -p "$(dirname "$LOG")"
: >"$LOG"

ran=0
failed=0
for script in "$HERE"/run_*_test.sh; do
    name="$(basename "$script")"
    [ "$name" = "run_all_tests.sh" ] && continue
    if [ "${#FILTER[@]}" -gt 0 ]; then
        wanted=0
        for frag in "${FILTER[@]}"; do
            case "$name" in *"$frag"*) wanted=1 ;; esac
        done
        [ "$wanted" -eq 1 ] || continue
    fi

    ran=$((ran + 1))
    start=$(date +%s)
    # No argument: each scenario's own documented wait (they differ per area,
    # and run_media_meter_test.sh takes an audio file first).
    out="$(bash "$script" 2>&1)"
    rc=$?
    took=$(( $(date +%s) - start ))
    # The scenario's own verdict line, which every one of them prints last.
    verdict="$(printf '%s\n' "$out" | grep -E '^[A-Za-z_]+: (PASS|FAIL)' | tail -1)"
    [ -z "$verdict" ] && verdict="$(printf '%s\n' "$out" | tail -1)"
    if [ "$rc" -eq 0 ]; then
        state="PASS"
    else
        state="FAIL rc=$rc"
        failed=$((failed + 1))
    fi
    { printf '===== %s (%ss) rc=%s =====\n' "$name" "$took" "$rc"
      printf '%s\n' "$out"; } >>"$LOG"
    printf '%-34s %6ss  %-9s %s\n' "$name" "$took" "$state" "$verdict"
done

if [ "$ran" -eq 0 ]; then
    echo "SELFTEST_ALL: FAIL — no scenario matched"
    exit 1
fi
if [ "$failed" -eq 0 ]; then
    echo "SELFTEST_ALL: PASS ($ran scenario(s), full output: $LOG)"
    exit 0
fi
echo "SELFTEST_ALL: FAIL ($failed of $ran failed — full output: $LOG)"
exit 1
