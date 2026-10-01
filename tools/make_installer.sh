#!/usr/bin/env bash
# ============================================================================
# tools/make_installer.sh — build the USER-FACING Windows installer.
#
# The end user downloads ONE exe and runs it: everything is inside (app + Qt +
# MinGW runtime), it installs per-user with no admin prompt, adds Start-menu /
# desktop shortcuts and the .vgr file association, and can seed the operator's
# data (installer/seed-data/) onto a fresh machine without ever overwriting
# anything that exists.
#
# Steps:
#   1. tools/make_portable.sh --release   (Release build → deploy → smoke test
#                                          on a clean PATH → zip as a bonus)
#   2. clean transient state (logs/enginedata/crashes) out of dist/
#   3. compile with Inno Setup (ISCC) → dist/VGRPresenter-Setup-<version>.exe
#
# The version is read from the canonical source (SettingsService.cpp) — never
# pass it by hand; use tools/set_version.sh first when it should change.
#
# Prereqs (once): winget install JRSoftware.InnoSetup
#
# Verdict line:
#   INSTALLER: PASS          exit 0
#   INSTALLER: FAIL <stage>  exit 1
#
# NOTE: nothing here pushes to GitHub — publishing stays a manual decision.
# ============================================================================
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
DIST="$ROOT/dist/VGRPresenter"

ISCC=""
for candidate in \
    "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    [ -f "$candidate" ] && ISCC="$candidate" && break
done
if [ -z "$ISCC" ]; then
    echo "Inno Setup not found — install once:  winget install JRSoftware.InnoSetup"
    echo "INSTALLER: FAIL prereqs"
    exit 1
fi

# ---- 1. Release build + deploy (+ smoke test on a clean PATH) ----------------
if ! bash "$ROOT/tools/make_portable.sh" --release; then
    echo "INSTALLER: FAIL build"
    exit 1
fi

VER="$(grep -oE 'kAppVersion = "[^"]+"' "$ROOT/qml/services/SettingsService.cpp" \
    | head -1 | sed -E 's/.*"([^"]+)"/\1/')"
[ -n "$VER" ] || { echo "INSTALLER: FAIL version"; exit 1; }

# ---- 2. clean transient state the smoke test wrote into dist/ ----------------
rm -rf "$DIST/logs" "$DIST/enginedata" "$DIST/crashes"

# ---- 3. compile the installer -------------------------------------------------
echo "== compiling installer (Inno Setup) =="
if ! "$ISCC" "/DAPP_VERSION=$VER" "$(cygpath -w "$ROOT/tools/installer.iss")" \
        > /tmp/iscc.log 2>&1; then
    echo "ISCC FAILED:"
    tail -25 /tmp/iscc.log
    echo "INSTALLER: FAIL compile"
    exit 1
fi

SETUP="$ROOT/dist/VGRPresenter-Setup-$VER.exe"
if [ ! -f "$SETUP" ]; then
    echo "INSTALLER: FAIL compile (setup exe not produced)"
    exit 1
fi

SIZE_MB="$(( $(stat -c%s "$SETUP") / 1024 / 1024 ))"
echo "installer OK: dist/VGRPresenter-Setup-$VER.exe (${SIZE_MB} MB)"
echo "  zip (bonus artifact): dist/VGRPresenter-win64.zip"
echo "INSTALLER: PASS"
