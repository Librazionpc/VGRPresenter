#!/usr/bin/env bash
# ============================================================================
# tools/make_portable.sh — produce a RUNNABLE-FROM-ANY-PC distribution.
#
# The dev build needs C:/Qt/.../bin and C:/msys64/ucrt64/bin on PATH (that is
# how dev_cycle.sh launches it). Another PC has neither, so this script assembles
# everything the exe needs INTO one folder:
#
#   1. BUILD   dev build (default) or a real Release build (--release: a
#              separate build-release/ directory configured -DCMAKE_BUILD_TYPE=Release
#              — the dev build directory keeps its own configuration). Release is
#              what ships: optimized (-O2/-DNDEBUG) where the dev default is
#              unoptimized.
#   2. DEPLOY  windeployqt --release --qmldir qml into dist/VGRPresenter/
#              (Qt DLLs + platform/imageformats/multimedia plugins + the QML
#              plugin trees for every `import` the qml/ directory uses, plus
#              the FFmpeg backend DLLs Qt Multimedia loads). The app's own QML
#              module needs nothing extra: qt_add_qml_module baked it into the
#              exe as a resource.
#   3. RUNTIME explicitly copies the MinGW runtime the exe's import table needs
#              (libgcc_s_seh-1, libstdc++-6, libwinpthread-1, zlib1) — windeployqt
#              only guarantees the MSVC runtime, and on MinGW these come from
#              the msys2 tree, not the Qt dir.
#   4. SMOKE   launches dist/VGRPresenter/appVGRPresenterUI.exe with a SCRUBBED
#              PATH (System32 only — no Qt, no MinGW: exactly what another PC
#              sees), waits, then verifies the process is alive and the fresh
#              engine log has no QML errors. Any missing DLL fails here instead
#              of on the target machine.
#   5. ZIP     dist/VGRPresenter-win64.zip (powershell Compress-Archive).
#
# Usage:
#   tools/make_portable.sh                 # build + deploy + smoke + zip
#   tools/make_portable.sh --release       # Release build (build-release/), ships
#   tools/make_portable.sh --no-build      # deploy the existing build exe
#   tools/make_portable.sh --no-zip        # skip the archive (faster iteration)
#   tools/make_portable.sh --no-gdb        # do not bundle gdb/analyze.bat
#
# Forensics shipped with the dist: the app writes a minidump on a crash (SEH)
# AND on a detected hang, and the dist bundles gdb + analyze.bat so either dump
# is readable on the target PC with no dev toolchain. analyze.bat prints the
# crash log tail and all thread backtraces; run it after any failure.
#
# Machine paths can be overridden by env (same variables as dev_cycle.sh):
#   CMAKE_BIN, MINGW_BIN (/c/msys64/ucrt64/bin), QT_BIN (/c/Qt/6.11.1/mingw_64/bin)
#
# Verdict line (grep-able, same convention as dev_cycle.sh):
#   PORTABLE: PASS          exit 0
#   PORTABLE: FAIL <stage>  exit 1
#
# Notes for the target machine:
#   - Windows 10+ only (UCRT is in-box; we do not ship api-ms-win-crt-*).
#   - NDI features need the NDI runtime on the TARGET PC (the app detects its
#     absence and offers the download — by design, never a mock).
#   - User data (settings, libraries, Bibles) lives in %AppData%\VGR — a fresh
#     PC starts empty; copy that folder over to migrate.
# ============================================================================
set -u

APP="appVGRPresenterUI.exe"
CMAKE_BIN="${CMAKE_BIN:-C:/Qt/Tools/CMake_64/bin/cmake.exe}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
QT_PREFIX="${QT_PREFIX:-C:/Qt/6.11.1/mingw_64}"
# The MinGW runtime must match the toolchain that BUILT THE EXE: the app
# compiles with msys2's GCC (C++26 — its libstdc++ has the newer symbols the
# exe imports), and dev_cycle.sh launches it with MINGW_BIN FIRST on PATH —
# the running app already resolves libstdc++-6.dll there. Qt's own toolchain
# runtime (mingw1310_64, GCC 13.1) is OLDER: it lacks those symbols and trips
# 0xC0000139 entry-point-not-found. The reverse direction is safe: a newer
# libstdc++ keeps the older-GCC-built Qt DLLs working. So: msys2 first,
# Qt toolchain as fallback only.
MINGW_RUNTIME_BIN="${MINGW_RUNTIME_BIN:-$MINGW_BIN}"
QT_MINGW_BIN="${QT_MINGW_BIN:-C:/Qt/Tools/mingw1310_64/bin}"
DO_BUILD=1
DO_ZIP=1
DO_RELEASE=0
# Ship gdb + a dump helper so a crash/hang on a CUSTOMER's PC is diagnosable
# without a dev toolchain (see the GDB step below). ~35 MB; --no-gdb opts out.
DO_GDB=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-zip)   DO_ZIP=0 ;;
        --release)  DO_RELEASE=1 ;;
        --no-gdb)   DO_GDB=0 ;;
        *) echo "make_portable.sh: unknown argument '$1'"; exit 2 ;;
    esac
    shift
done

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$ROOT" ]; then
    ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fi
DIST="$ROOT/dist/VGRPresenter"
LOG="$DIST/logs/engine.log"

# ---- 1. BUILD ----------------------------------------------------------------
RELEASE_BUILD_DIR="$ROOT/build-release"
if [ "$DO_BUILD" = 1 ]; then
    if [ "$DO_RELEASE" = 1 ]; then
        echo "== building (Release → build-release/) =="
        if [ ! -f "$RELEASE_BUILD_DIR/CMakeCache.txt" ]; then
            echo "== configuring build-release/ (one-time) =="
            # Ninja is NOT on the default PATH (msys2 doesn't ship it) — Qt
            # installs it under Tools/Ninja. Pin it explicitly.
            NINJA_BIN="${NINJA_BIN:-C:/Qt/Tools/Ninja/ninja.exe}"
            if [ ! -f "$NINJA_BIN" ]; then
                echo "ninja not found at $NINJA_BIN (set NINJA_BIN)"
                echo "PORTABLE: FAIL configure"
                exit 1
            fi
            if ! PATH="$MINGW_BIN:$PATH" "$CMAKE_BIN" -S "$ROOT" -B "$RELEASE_BUILD_DIR" -G Ninja \
                    -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
                    -DCMAKE_BUILD_TYPE=Release \
                    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
                    -DCMAKE_C_COMPILER="C:/msys64/ucrt64/bin/gcc.exe" \
                    -DCMAKE_CXX_COMPILER="C:/msys64/ucrt64/bin/g++.exe"; then
                echo "PORTABLE: FAIL configure"
                exit 1
            fi
        fi
        if ! PATH="$MINGW_BIN:$PATH" "$CMAKE_BIN" --build "$RELEASE_BUILD_DIR"; then
            echo "PORTABLE: FAIL build"
            exit 1
        fi
    else
        echo "== building (dev default) =="
        if ! bash "$ROOT/tools/dev_cycle.sh" --build-only; then
            echo "PORTABLE: FAIL build"
            exit 1
        fi
    fi
fi

EXE_SRC="$ROOT/build/$APP"
[ "$DO_RELEASE" = 1 ] && EXE_SRC="$RELEASE_BUILD_DIR/$APP"
if [ ! -f "$EXE_SRC" ]; then
    echo "deploy FAILED — $EXE_SRC missing (build first)"
    echo "PORTABLE: FAIL deploy"
    exit 1
fi

# ---- 2. DEPLOY ---------------------------------------------------------------
echo "== deploying to dist/VGRPresenter/ =="
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$EXE_SRC" "$DIST/"

# The default style background is referenced by filename so an existing
# style roster can attach it on a different machine. Ship the image beside
# the app for the renderer's ExecutableDir()/assets fallback.
mkdir -p "$DIST/assets"
cp "$ROOT/assets/VGRPresenter background.png" "$DIST/assets/"

WINDEPLOYQT="$QT_BIN/windeployqt.exe"
if [ ! -f "$WINDEPLOYQT" ]; then
    echo "windeployqt not found at $WINDEPLOYQT"
    echo "PORTABLE: FAIL deploy"
    exit 1
fi
if ! PATH="$MINGW_BIN:$QT_BIN:$PATH" "$WINDEPLOYQT" \
        --release --no-translations --qmldir "$ROOT/qml" \
        "$DIST/$APP" > /tmp/windeployqt.log 2>&1; then
    echo "windeployqt FAILED:"
    tail -20 /tmp/windeployqt.log
    echo "PORTABLE: FAIL deploy"
    exit 1
fi

# MinGW runtime: the exe's own import-table needs (see the header). COPIED
# FROM THE QT TOOLCHAIN (see the note above) — not msys2.
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll zlib1.dll; do
    src=""
    for dir in "$MINGW_RUNTIME_BIN" "$QT_MINGW_BIN"; do
        [ -f "$dir/$dll" ] && src="$dir/$dll" && break
    done
    if [ -z "$src" ]; then
        echo "MinGW runtime $dll not found (looked in $MINGW_RUNTIME_BIN, $QT_MINGW_BIN, $MINGW_BIN)"
        echo "PORTABLE: FAIL deploy"
        exit 1
    fi
    cp "$src" "$DIST/"
done

# ---- 2b. GDB (crash/hang forensics, shipped) --------------------------------
# The app writes a minidump on a crash (SEH) and, since this change, on a HANG
# (CrashHandler::WriteHangDump). A minidump is unreadable without a debugger,
# and the exe's MinGW/DWARF symbols are exactly what gdb resolves — Windows'
# own dbghelp cannot read DWARF, which is why the offline symbolizer uses nm.
# So ship gdb beside the app plus a one-command helper that opens the newest
# dump. This is what makes a crash on a target PC diagnosable with no dev kit.
#
# --no-gdb skips this (the zip stays ~35 MB smaller; the dumps still get
# written, they just need a gdb from elsewhere to read).
if [ "$DO_GDB" = 1 ]; then
    echo "== bundling gdb for post-mortem analysis =="
    GDB_SRC="${GDB_BIN:-$MINGW_BIN/gdb.exe}"
    if [ ! -f "$GDB_SRC" ]; then
        echo "gdb not found at $GDB_SRC (set GDB_BIN)"
        echo "PORTABLE: FAIL deploy"
        exit 1
    fi
    GDB_DIR="$DIST/tools/gdb"
    mkdir -p "$GDB_DIR"
    cp "$GDB_SRC" "$GDB_DIR/"
    # gdb's own runtime deps the app does NOT already ship beside the exe.
    # (libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1 / zlib1 are copied in the
    # step above; these are gdb-only.)
    for dll in libexpat-1.dll libgmp-10.dll libiconv-2.dll libintl-8.dll \
               liblzma-5.dll libmpfr-6.dll libncursesw6.dll libpython3.12.dll \
               libreadline8.dll libxxhash.dll libzstd.dll; do
        if [ -f "$MINGW_BIN/$dll" ]; then
            cp "$MINGW_BIN/$dll" "$GDB_DIR/"
        else
            echo "  warning: gdb dependency $dll not found in $MINGW_BIN"
        fi
    done

    # analyze.bat — what a customer double-clicks after a failure. It reads the
    # app's OWN artifacts: the crash log, and the gdb self-attach capture that
    # WriteHangDump leaves on a hang (hang-<pid>-gdb.txt). It does NOT try to
    # open the .dmp in gdb — MinGW gdb reads ELF cores, not Windows minidumps.
    cat > "$DIST/analyze.bat" <<'BAT'
@echo off
setlocal enabledelayedexpansion
set "CRASHES=%APPDATA%\VGR\VGRPresenter\crashes"
echo === VGR crash / hang report ===
if not exist "%CRASHES%" ( echo No crash folder yet: "%CRASHES%" & pause & exit /b 1 )
echo
echo --- crash log (last 60 lines) ---
if exist "%CRASHES%\crash.log" powershell -NoProfile -Command "Get-Content -Tail 60 '%CRASHES%\crash.log'"
echo
set "GDBTXT="
for /f "delims=" %%f in ('dir /b /o-d "%CRASHES%\hang-*-gdb.txt" 2^>nul') do (
    if not defined GDBTXT set "GDBTXT=%CRASHES%\%%f"
)
if defined GDBTXT (
    echo --- symbolized backtraces: !GDBTXT! ---
    type "!GDBTXT!"
) else (
    echo --- no gdb capture present ---
    echo A hang writes hang-^<pid^>-gdb.txt only when a gdb was resolvable at the
    echo time ^(the dist ships one at tools\gdb\gdb.exe^); a crash writes raw frames
    echo plus dbghelp symbols into crash.log instead.
)
echo
set "DMP="
for /f "delims=" %%f in ('dir /b /o-d "%CRASHES%\*.dmp" 2^>nul') do (
    if not defined DMP set "DMP=%CRASHES%\%%f"
)
if defined DMP (
    echo --- newest minidump: !DMP! ---
    echo ^(Windows minidump: analyse with WinDbg or LLDB, not MinGW gdb.^)
)
pause
BAT
fi

# ---- 3. SMOKE TEST (clean PATH — another PC's environment) -------------------
echo "== smoke test (no Qt/MinGW on PATH) =="
LOG_BEFORE=0
[ -f "$LOG" ] && LOG_BEFORE="$(wc -l < "$LOG")"

(
    cd "$DIST" || exit 1
    # System32-only PATH: OS DLLs resolve, ours must come from the dist folder.
    # Exec the exe DIRECTLY (no nohup — with PATH replaced, the shell itself
    # could not find /usr/bin/nohup and the launch silently did nothing).
    PATH="/c/Windows/System32:/c/Windows" "./$APP" >/dev/null 2>&1 < /dev/null &
) >/dev/null 2>&1

sleep 10
PID_LINE="$(tasklist //FI "IMAGENAME eq $APP" //FO CSV 2>/dev/null | grep -i "$APP" | head -1)"
if [ -z "$PID_LINE" ]; then
    # Name the failure mode: the exit code distinguishes a missing DLL
    # (0xC0000135) from an entry-point mismatch (0xC0000139) from a crash.
    EXIT_CODE="$(powershell -NoProfile -Command '
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = "appVGRPresenterUI.exe"
        $psi.WorkingDirectory = (Get-Location).Path
        $psi.UseShellExecute = $false
        $psi.EnvironmentVariables["PATH"] = "C:\Windows\System32;C:\Windows"
        $p = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Seconds 6
        if ($p.HasExited) { "0x{0:X}" -f $p.ExitCode } else { "RUNNING"; Stop-Process -Id $p.Id -Force }
    ' 2>/dev/null | tail -1)"
    echo "smoke FAILED — process is not running on a clean PATH (relaunch exit code: ${EXIT_CODE:-unknown})."
    echo "  0xC0000135 = DLL missing; 0xC0000139 = entry point missing (runtime version mismatch)."
    echo "Last log lines:"
    tail -n 20 "$LOG" 2>/dev/null
    echo "PORTABLE: FAIL smoke"
    exit 1
fi

QML_ERRORS=0
if [ -f "$LOG" ]; then
    QML_ERRORS="$(tail -n +$((LOG_BEFORE + 1)) "$LOG" \
        | grep -icE "\\[(WARN|ERROR|FATAL)\\].*\\[QML\\]|RangeError|Cannot read|TypeError|cannot find|cannot load")"
fi
if [ "$QML_ERRORS" -gt 0 ]; then
    echo "smoke FAILED — $QML_ERRORS error line(s) on a clean PATH:"
    tail -n +$((LOG_BEFORE + 1)) "$LOG" \
        | grep -aiE "\\[(WARN|ERROR|FATAL)\\].*\\[QML\\]|RangeError|Cannot read|TypeError|cannot find|cannot load" | head -10
    taskkill //IM "$APP" //F >/dev/null 2>&1
    echo "PORTABLE: FAIL smoke"
    exit 1
fi
echo "smoke OK — $APP runs from the dist folder with no Qt/MinGW on PATH (pid $(echo "$PID_LINE" | cut -d'"' -f4))"
taskkill //IM "$APP" //F >/dev/null 2>&1
sleep 2

# ---- 4. ZIP ------------------------------------------------------------------
if [ "$DO_ZIP" = 1 ]; then
    echo "== zipping dist/VGRPresenter-win64.zip =="
    rm -f "$ROOT/dist/VGRPresenter-win64.zip"
    if ! powershell -NoProfile -Command \
        "Compress-Archive -Path '$(cygpath -w "$DIST")' -DestinationPath '$(cygpath -w "$ROOT/dist/VGRPresenter-win64.zip")' -Force" \
        > /dev/null 2>&1; then
        echo "zip FAILED"
        echo "PORTABLE: FAIL zip"
        exit 1
    fi
    SIZE_MB="$(( $(stat -c%s "$ROOT/dist/VGRPresenter-win64.zip") / 1024 / 1024 ))"
    echo "zip OK (${SIZE_MB} MB)"
fi

echo "PORTABLE: PASS"
exit 0
