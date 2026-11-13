#!/usr/bin/env bash
# ============================================================================
# tools/dev_cycle.sh — stop → build → launch → health-check, as one command.
#
# Encodes the launch workflow proven on this machine (MSYS2/MinGW + Qt 6.11):
#
#   1. STOP   any running appVGRPresenterUI.exe (PowerShell, taskkill fallback)
#             then waits ~2 s so the exe file lock is released for linking.
#   2. BUILD  `cmake --build build` — fails fast, printing only error lines.
#   3. TESTS  `build/bps_unit_tests.exe` with the same MinGW+Qt PATH the launch
#             needs (without it the binary exits 127 with NO OUTPUT, which reads
#             exactly like a quiet pass if the status is not checked — it is).
#             The verdict is its "N checks, M failures" line. `--no-tests` skips
#             this step, and `--launch-only` implies it: that mode is the fast path
#             to a running app, and paying for the suite there defeated the point.
#   4. LAUNCH build/appVGRPresenterUI.exe with the MinGW + Qt bin dirs on PATH
#             (the app must be launched with CWD=build/ — it writes
#             logs/engine.log relative to its working directory; launching
#             without the Qt bin on PATH produces the DLL-missing popups).
#   5. HEALTH after --health-secs: process alive + zero QML errors in the
#             engine-log window that STARTED at launch (log-line delta, so
#             errors from previous runs cannot false-positive).
#   6. SELFTESTS (opt-in, --with-selftests): tools/selftest/run_all_tests.sh —
#             the acceptance suite that drives the running app. Slow, and it
#             restarts the app itself, so the hang watcher is skipped when it
#             runs.
#
# Usage (run from anywhere; the script resolves the repo root):
#   tools/dev_cycle.sh                    # full cycle: stop, build, launch, check
#   tools/dev_cycle.sh --launch-only      # stop, launch, check — the FAST path:
#                                         # skips the build AND the unit-test step
#                                         # (run the cycle itself, or
#                                         # build/bps_unit_tests.exe directly, when
#                                         # the suite is what you are after)
#   tools/dev_cycle.sh --build-only       # stop, build (no launch, NO unit tests —
#                                         # a build-only run is the fast inner loop;
#                                         # run build/bps_unit_tests.exe directly
#                                         # when the suite is what you are after)
#   tools/dev_cycle.sh --health-secs 20   # wait longer before the health check
#   tools/dev_cycle.sh --no-tests         # skip the engine unit-test step (it runs
#                                         # by default, as step 3)
#   tools/dev_cycle.sh --with-selftests   # also run tools/selftest/run_all_tests.sh
#                                         # (slow: drives the running app)
#   tools/dev_cycle.sh --no-watch         # skip the background hang watcher
#   tools/dev_cycle.sh --watch-secs 3600  # how long the hang watcher lives
#
# After a healthy launch this script backgrounds a HANG WATCHER
# (tools/gdb_watch_hang.sh): it tails build/logs/engine.log for the app's own
# "HangWatchdog" line and attaches gdb to the live process the moment one
# appears, saving every thread's stack to build/gdb_capture_<pid>_<stamp>.txt.
# A hang is the app's dominant failure and — unlike a crash — leaves no record,
# so this is what makes it attributable. See tools/gdb_capture.sh too (attach on
# demand: `tools/gdb_capture.sh`). Disable per-run with --no-watch.
#
# Machine-specific paths can be overridden by env if a checkout differs:
#   CMAKE_BIN (default C:/Qt/Tools/CMake_64/bin/cmake.exe)
#   MINGW_BIN (default /c/msys64/ucrt64/bin)
#   QT_BIN    (default /c/Qt/6.11.1/mingw_64/bin)
#
# The last line is a verdict other agents (and humans) can grep:
#   DEV_CYCLE: PASS             exit 0
#   DEV_CYCLE: FAIL build       exit 1
#   DEV_CYCLE: FAIL launch      exit 2  (exe missing or process not alive)
#   DEV_CYCLE: FAIL qml-errors  exit 3  (engine log gained QML errors)
#   DEV_CYCLE: FAIL tests       exit 4  (build/bps_unit_tests.exe failed or was
#                                        missing — see build/logs/unit_tests.log)
#   DEV_CYCLE: FAIL selftests   exit 5  (tools/selftest/run_all_tests.sh failed —
#                                        only with --with-selftests; see
#                                        build/logs/selftests.log)
# ============================================================================
set -u

APP="appVGRPresenterUI.exe"
CMAKE_BIN="${CMAKE_BIN:-C:/Qt/Tools/CMake_64/bin/cmake.exe}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
HEALTH_SECS=12
MODE="full"
# Hang watch: after a healthy launch, background tools/gdb_watch_hang.sh so an
# interactive HANG is captured with a live gdb stack. A hang is this app's
# dominant failure and leaves no other artifact (see the section below).
WATCH=1
WATCH_SECS=900
# Unit tests run by DEFAULT (see step 3); the acceptance suite is opt-in.
TESTS=1
SELFTESTS=0

while [ $# -gt 0 ]; do
    case "$1" in
        # --launch-only is the FAST path to a running app: it skips the build and
        # the unit-test step. The suite used to run here anyway (the tests block is
        # guarded by TESTS alone), so "just relaunch it" paid for all 7307 checks
        # after every edit — which is the opposite of what this mode is for. Run
        # the full cycle (or build/bps_unit_tests.exe) when the suite is the point.
        --launch-only) MODE="launch" ; TESTS=0 ;;
        --build-only)  MODE="build" ;;
        --health-secs) shift; HEALTH_SECS="${1:-12}" ;;
        --watch-secs)  shift; WATCH_SECS="${1:-900}" ;;
        --no-tests)    TESTS=0 ;;
        --with-selftests) SELFTESTS=1 ;;
        --no-watch)    WATCH=0 ;;
        *) echo "dev_cycle.sh: unknown argument '$1'"; exit 2 ;;
    esac
    shift
done

# Repo root: prefer git, fall back to the script's parent directory.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$ROOT" ]; then
    ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fi
BUILD_DIR="$ROOT/build"
LOG="$BUILD_DIR/logs/engine.log"

# ---- 1. STOP ----------------------------------------------------------------
echo "== stopping any running $APP =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
taskkill //IM "$APP" //F >/dev/null 2>&1   # fallback if PowerShell is unavailable
sleep 2                                     # release the exe lock before linking

if [ "$MODE" = "launch" ]; then
    echo "== skipping build (--launch-only) =="
else
    # ---- 2. BUILD -----------------------------------------------------------
    echo "== building (build/) =="
    # Some Windows/MSYS installations map /tmp to a protected system path.
    # Keep the captured build output in the writable build tree instead.
    mkdir -p "$BUILD_DIR/tmp"
    export TMPDIR="$BUILD_DIR/tmp" TMP="$BUILD_DIR/tmp" TEMP="$BUILD_DIR/tmp"
    BUILD_OUT="$(mktemp)"
    # Show progress in real-time while capturing to file for error extraction.
    # The BUILD's OWN status has to be read separately: this is a pipeline (no
    # `set -o pipefail` here), so `if ! ... | tee` tested TEE's status — the
    # failure branch below never ran and a failed build printed "build OK", after
    # which the cycle launched the STALE binary and reported PASS. An agent then
    # verified a binary that did not contain its edits. PIPESTATUS[0] is the
    # subshell, i.e. cmake.
    (cd "$ROOT" && PATH="$MINGW_BIN:$PATH" "$CMAKE_BIN" --build build) 2>&1 | tee "$BUILD_OUT"
    BUILD_STATUS="${PIPESTATUS[0]}"
    if [ "$BUILD_STATUS" -ne 0 ]; then
        echo "BUILD FAILED — error lines:"
        grep -aiE "error|FAILED" "$BUILD_OUT" | head -30
        rm -f "$BUILD_OUT"
        echo "DEV_CYCLE: FAIL build"
        exit 1
    fi
    rm -f "$BUILD_OUT"
    echo "build OK"
fi

# ---- 3. UNIT TESTS ---------------------------------------------------------
# The engine suite (build/bps_unit_tests.exe) is the ONLY check in this repo that
# exercises behaviour rather than shape: the two guard targets above prove the
# source obeys a rule, and the health check only proves the app boots. Without
# this step a regression in the engine is caught by a human noticing, so it runs
# by default here (skip with --no-tests).
#
# NOT in --build-only: that mode exists to answer "does it compile" fast (often
# for one QML file), and a suite that takes a minute turned it into a full cycle
# whose failures were indistinguishable from build failures. The PASS line says
# the tests did not run, so a green build-only can never be mistaken for a green
# suite.
#
# It needs the SAME PATH the launch does: it links the MinGW/Qt runtime, and
# without those dirs Windows cannot resolve its DLLs — the binary then exits 127
# with NO output at all, which reads exactly like "the tests passed quietly" if
# the status is not checked (it is, below).
if [ "$TESTS" = 1 ] && [ "$MODE" = "build" ]; then
    echo "== unit tests: skipped (--build-only; run build/bps_unit_tests.exe directly) =="
    TESTS=0
fi

if [ "$TESTS" = 1 ]; then
    TESTS_BIN="$BUILD_DIR/bps_unit_tests.exe"
    if [ ! -f "$TESTS_BIN" ]; then
        echo "tests FAILED — $TESTS_BIN does not exist (build first)"
        echo "DEV_CYCLE: FAIL tests"
        exit 4
    fi
    echo "== unit tests =="
    TESTS_LOG="$BUILD_DIR/logs/unit_tests.log"
    mkdir -p "$BUILD_DIR/logs"
    PATH="$MINGW_BIN:$QT_BIN:$PATH" "$TESTS_BIN" >"$TESTS_LOG" 2>&1
    TESTS_STATUS="$?"
    TESTS_SUMMARY="$(grep -aE "[0-9]+ checks, [0-9]+ failures" "$TESTS_LOG" | tail -1)"
    if [ "$TESTS_STATUS" -ne 0 ] || [ -z "$TESTS_SUMMARY" ]; then
        echo "tests FAILED (status $TESTS_STATUS${TESTS_SUMMARY:+, $TESTS_SUMMARY}) — tail:"
        tail -n 20 "$TESTS_LOG"
        echo "DEV_CYCLE: FAIL tests"
        exit 4
    fi
    echo "tests OK — $TESTS_SUMMARY"
fi

if [ "$MODE" = "build" ]; then
    echo "DEV_CYCLE: PASS (build-only, tests not run)"
    exit 0
fi

# ---- 4. LAUNCH --------------------------------------------------------------
if [ ! -f "$BUILD_DIR/$APP" ]; then
    echo "launch FAILED — $BUILD_DIR/$APP does not exist (build first)"
    echo "DEV_CYCLE: FAIL launch"
    exit 2
fi

# Log-line delta: everything AFTER this mark belongs to the new instance.
LOG_BEFORE=0
[ -f "$LOG" ] && LOG_BEFORE="$(wc -l < "$LOG")"

echo "== launching (CWD=$BUILD_DIR, health check in ${HEALTH_SECS}s) =="
(
    cd "$BUILD_DIR" || exit 1
    # VGR_DEPLOY=0: a developer launch keeps the full log, the stall sampler and the technical toasts
    # (the shipped app runs in deployment mode, see qml/services/include/services/DeploymentMode.h).
    VGR_DEPLOY=0 PATH="$MINGW_BIN:$QT_BIN:$PATH" nohup "./$APP" >/dev/null 2>&1 \
        < /dev/null &
    disown
) >/dev/null 2>&1

sleep "$HEALTH_SECS"

# ---- 5. HEALTH CHECK --------------------------------------------------------
PID="$(powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Id" \
    2>/dev/null | tr -d '\r')"
if [ -z "$PID" ]; then
    echo "health FAILED — process is not running. Last log lines:"
    tail -n 20 "$LOG" 2>/dev/null
    echo "DEV_CYCLE: FAIL launch"
    exit 2
fi

QML_ERRORS=0
if [ -f "$LOG" ]; then
    QML_ERRORS="$(tail -n +$((LOG_BEFORE + 1)) "$LOG" \
        | grep -icE "\\[(WARN|ERROR|FATAL)\\].*\\[QML\\]|RangeError|Cannot read|TypeError")"
fi

if [ "$QML_ERRORS" -gt 0 ]; then
    echo "health FAILED — $QML_ERRORS QML error line(s) since launch:"
    tail -n +$((LOG_BEFORE + 1)) "$LOG" \
        | grep -aiE "\\[(WARN|ERROR|FATAL)\\].*\\[QML\\]|RangeError|Cannot read|TypeError" | head -10
    echo "DEV_CYCLE: FAIL qml-errors"
    exit 3
fi

echo "health OK — $APP running (pid $PID), log: $LOG"

# ---- 6. ACCEPTANCE SELFTESTS (opt-in) ---------------------------------------
# tools/selftest/run_all_tests.sh is the app's own acceptance suite: it drives the
# RUNNING app (row counts, dividers, modal fit, palettes, the pill, the bus
# light, forensics, insets) and prints its own verdict. It is slow and it starts
# and stops the app itself, which is why it is opt-in (--with-selftests) and why
# the hang watcher below is skipped when it runs — the watcher attaches to the
# instance this step would be restarting.
if [ "$SELFTESTS" = 1 ]; then
    SELFTEST_RUNNER="$ROOT/tools/selftest/run_all_tests.sh"
    if [ ! -f "$SELFTEST_RUNNER" ]; then
        echo "selftests FAILED — $SELFTEST_RUNNER is missing"
        echo "DEV_CYCLE: FAIL selftests"
        exit 5
    fi
    echo "== acceptance selftests (this takes a while) =="
    SELFTEST_LOG="$BUILD_DIR/logs/selftests.log"
    mkdir -p "$BUILD_DIR/logs"
    (cd "$ROOT" && PATH="$MINGW_BIN:$QT_BIN:$PATH" bash "$SELFTEST_RUNNER") \
        >"$SELFTEST_LOG" 2>&1
    SELFTEST_STATUS="$?"
    SELFTEST_SUMMARY="$(grep -aiE "PASS|FAIL" "$SELFTEST_LOG" | tail -1)"
    if [ "$SELFTEST_STATUS" -ne 0 ]; then
        echo "selftests FAILED (status $SELFTEST_STATUS) — failing lines:"
        grep -aiE "FAIL" "$SELFTEST_LOG" | head -20
        echo "DEV_CYCLE: FAIL selftests"
        exit 5
    fi
    echo "selftests OK${SELFTEST_SUMMARY:+ — $SELFTEST_SUMMARY}"
    WATCH=0
fi

# ---- 7. HANG WATCH (gdb attach on stall) ------------------------------------
# A HANG is this app's dominant failure mode, and it leaves NO artifact on its
# own: nothing faults, so neither the SEH handler nor the crash log records it,
# and Windows simply ghosts the window and closes the process (Event Log Id
# 1002 "Application hang"). The app watches itself (CallTrace's HangWatchdog
# writes a "HangWatchdog" line when the GUI thread stops ticking — and
# CrashHandler::WriteHangDump drops a hang-<pid>.dmp beside the crash log), but
# a LIVE gdb attach gives the native stack directly.
#
# The app keeps running after this script returns, so the watcher must outlive
# the cycle: nohup + disown. It saves build/gdb_capture_<pid>_<stamp>.txt and
# exits on the first capture, when the app exits, or at --watch-secs.
if [ "$WATCH" = 1 ]; then
    WATCH_LOG="$BUILD_DIR/logs/gdb_watch.log"
    mkdir -p "$BUILD_DIR/logs"
    echo "== hang watch: gdb auto-attach on stall, up to ${WATCH_SECS}s (log: $WATCH_LOG) =="
    nohup bash "$ROOT/tools/gdb_watch_hang.sh" "$WATCH_SECS" \
        >"$WATCH_LOG" 2>&1 </dev/null &
    disown 2>/dev/null || true
fi

echo "DEV_CYCLE: PASS"
exit 0
