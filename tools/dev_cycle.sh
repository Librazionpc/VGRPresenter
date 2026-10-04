#!/usr/bin/env bash
# ============================================================================
# tools/dev_cycle.sh — stop → build → launch → health-check, as one command.
#
# Encodes the launch workflow proven on this machine (MSYS2/MinGW + Qt 6.11):
#
#   1. STOP   any running appVGRPresenterUI.exe (PowerShell, taskkill fallback)
#             then waits ~2 s so the exe file lock is released for linking.
#   2. BUILD  `cmake --build build` — fails fast, printing only error lines.
#   3. LAUNCH build/appVGRPresenterUI.exe with the MinGW + Qt bin dirs on PATH
#             (the app must be launched with CWD=build/ — it writes
#             logs/engine.log relative to its working directory; launching
#             without the Qt bin on PATH produces the DLL-missing popups).
#   4. HEALTH after --health-secs: process alive + zero QML errors in the
#             engine-log window that STARTED at launch (log-line delta, so
#             errors from previous runs cannot false-positive).
#
# Usage (run from anywhere; the script resolves the repo root):
#   tools/dev_cycle.sh                    # full cycle: stop, build, launch, check
#   tools/dev_cycle.sh --launch-only      # stop, launch, check (skip build)
#   tools/dev_cycle.sh --build-only       # stop, build (no launch)
#   tools/dev_cycle.sh --health-secs 20   # wait longer before the health check
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
# ============================================================================
set -u

APP="appVGRPresenterUI.exe"
CMAKE_BIN="${CMAKE_BIN:-C:/Qt/Tools/CMake_64/bin/cmake.exe}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
HEALTH_SECS=12
MODE="full"

while [ $# -gt 0 ]; do
    case "$1" in
        --launch-only) MODE="launch" ;;
        --build-only)  MODE="build" ;;
        --health-secs) shift; HEALTH_SECS="${1:-12}" ;;
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
    BUILD_OUT="$(mktemp)"
    if ! (cd "$ROOT" && PATH="$MINGW_BIN:$PATH" "$CMAKE_BIN" --build build) \
            >"$BUILD_OUT" 2>&1; then
        echo "BUILD FAILED — error lines:"
        grep -aiE "error|FAILED" "$BUILD_OUT" | head -30
        rm -f "$BUILD_OUT"
        echo "DEV_CYCLE: FAIL build"
        exit 1
    fi
    rm -f "$BUILD_OUT"
    echo "build OK"
fi

if [ "$MODE" = "build" ]; then
    echo "DEV_CYCLE: PASS (build-only)"
    exit 0
fi

# ---- 3. LAUNCH --------------------------------------------------------------
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
    PATH="$MINGW_BIN:$QT_BIN:$PATH" nohup "./$APP" >/dev/null 2>&1 \
        < /dev/null &
    disown
) >/dev/null 2>&1

sleep "$HEALTH_SECS"

# ---- 4. HEALTH CHECK --------------------------------------------------------
PID_LINE="$(tasklist //FI "IMAGENAME eq $APP" //FO CSV 2>/dev/null | grep -i "$APP" | head -1)"
if [ -z "$PID_LINE" ]; then
    echo "health FAILED — process is not running. Last log lines:"
    tail -n 20 "$LOG" 2>/dev/null
    echo "DEV_CYCLE: FAIL launch"
    exit 2
fi
PID="$(echo "$PID_LINE" | cut -d'"' -f4)"

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
echo "DEV_CYCLE: PASS"
exit 0
