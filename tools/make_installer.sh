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
#  1b. SEED: generate the shipped design catalogs from the engine itself
#      (bps_cli --dump-catalogs) and bundle this machine's configuration
#      (tools/make_seed_config.mjs → assets/settings.shipped.json + the seed
#      data). A catalog is never hand-copied: that is how the shipped templates
#      once carried the fonts of the PC they were copied from.
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
# Same override as dev_cycle/make_portable: bps_cli needs the msys2 runtime on PATH.
MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
NODE_BIN="${NODE_BIN:-node}"

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

# ---- 1b. SEED: catalogs FROM THE CODE, configuration FROM THIS MACHINE ------
# Two different jobs, both here so the installer can never ship a stale copy:
#
#   * the design catalogs are GENERATED (bps_cli --dump-catalogs): they are the
#     engine's own shipped designs, with the font-free blocks and the catalog
#     revision this build carries. Hand-copying them is what put one machine's
#     fonts into every other machine's templates.
#   * the CONFIGURATION is snapshotted from this machine (make_seed_config.mjs),
#     with this PC's absolute paths stripped. The app adopts a shipped value only
#     where an install has none of its own, so an update arrives configured and
#     nobody has to set the app up again.
SEED_DIR="$ROOT/installer/seed-data"
CLI="$ROOT/build-release/bps_cli.exe"
[ -f "$CLI" ] || CLI="$ROOT/build/bps_cli.exe"
if [ ! -f "$CLI" ]; then
    echo "bps_cli not found (build first) — cannot generate the shipped catalogs"
    echo "INSTALLER: FAIL seed"
    exit 1
fi
SEED_RUN="$ROOT/build/.seed-dump"
rm -rf "$SEED_RUN"
mkdir -p "$SEED_RUN"
echo "== seeding catalogs from the engine (bps_cli --dump-catalogs) =="
if ! ( cd "$SEED_RUN" && PATH="$MINGW_BIN:$PATH" "$CLI" \
        --data "$(cygpath -m "$SEED_RUN")/data" \
        --dump-catalogs "$(cygpath -m "$SEED_DIR/local")" ); then
    echo "catalog dump FAILED"
    rm -rf "$SEED_RUN"
    echo "INSTALLER: FAIL seed"
    exit 1
fi
rm -rf "$SEED_RUN"

echo "== bundling this machine's configuration (settings + kernel) =="
if ! "$NODE_BIN" "$ROOT/tools/make_seed_config.mjs"; then
    echo "INSTALLER: FAIL seed"
    exit 1
fi

# ---- 2. clean transient state the smoke test wrote into dist/ ----------------
rm -rf "$DIST/logs" "$DIST/enginedata" "$DIST/crashes"

# ---- 3. compile the installer -------------------------------------------------
echo "== compiling installer (Inno Setup) =="
# MSYS2_ARG_CONV_EXCL: without it bash rewrites the /D define into a Windows
# path and ISCC sees "more than one script filename".
if ! MSYS2_ARG_CONV_EXCL="*" "$ISCC" "/DAPP_VERSION=$VER" "$(cygpath -w "$ROOT/tools/installer.iss")" \
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
