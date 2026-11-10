#!/usr/bin/env bash
# ============================================================================
# tools/make_installer_unix.sh — the Linux and macOS half of tools/make_installer.sh.
#
# You do not run this directly: `tools/make_installer.sh` is the ONE release command on every OS and
# hands over to this on Linux and macOS (as it builds the Setup .exe on Windows). Same steps, same
# version source, same verdict lines:
#
#   1. Prerequisites            version, Qt, cmake, node (each printed)
#   2. Release build            build-release/  (-DCMAKE_BUILD_TYPE=Release; appVGRPresenterUI + bps_cli)
#   3. Seed                     design catalogs from the engine (bps_cli --dump-catalogs); the machine's
#                               configuration snapshot is taken during packaging — never hand-copied
#   4. Package                  Linux:  AppImage (+ tar.gz of the AppDir)      via package_linux.sh
#                               macOS:  signed .app inside a .dmg              via package_macos.sh
#
# Progress: each stage prints "[n/4] <title> (elapsed …)", the build streams cmake/ninja's own counter,
# slow silent tools print a "still working" line every 15 s, and a failure prints how long it ran.
#
# Output (the version is read from kAppVersion in qml/services/SettingsService.cpp — never passed by
# hand; the name follows the Windows pattern VGRPresenter-Setup-<version>…):
#   dist/VGRPresenter-Setup-<version>-linux-<arch>.AppImage
#   dist/VGRPresenter-Setup-<version>-macos-<arch>.dmg
#
# Prereqs (once): Qt 6.11 with Multimedia (QT_ROOT_DIR, or qmake on PATH), cmake, a C++26 compiler,
# ninja (optional), node. Linux also: curl (fetches linuxdeploy). macOS also: Xcode command line tools.
#
# Verdict line:   INSTALLER: PASS    exit 0     |     INSTALLER: FAIL <stage>   exit 1
# NOTE: nothing here pushes to GitHub — publishing stays a manual decision.
# ============================================================================
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
# shellcheck source=lib/unix_release.sh
. "$ROOT/tools/lib/unix_release.sh"

# Four stages, each announced as [n/4] with the elapsed time (see stage_begin in lib/unix_release.sh).
RELEASE_STAGE_TOTAL=4; export RELEASE_STAGE_TOTAL RELEASE_T0

OS="$(uname -s)"
case "$OS" in
    Linux)  PLATFORM=linux ;;
    Darwin) PLATFORM=macos ;;
    *) echo "make_installer_unix: $OS is not a Linux/macOS host"; installer_fail prereqs ;;
esac

# ---- 1. Prerequisites ---------------------------------------------------------------------------------
stage_begin "Checking prerequisites"
VER="$(release_version "$ROOT")"
[ -n "$VER" ] || installer_fail version
step "version $VER (kAppVersion in qml/services/SettingsService.cpp)"

QT_PREFIX="$(find_qt_prefix)" || { echo "Qt not found — set QT_ROOT_DIR to the Qt 6.11 kit (the folder holding bin/qmake)"; installer_fail prereqs; }
step "Qt:    $QT_PREFIX"
command -v cmake >/dev/null 2>&1 || { echo "cmake not found"; installer_fail prereqs; }
step "cmake: $(cmake --version | head -1)"
NODE_BIN="${NODE_BIN:-node}"; export NODE_BIN
command -v "$NODE_BIN" >/dev/null 2>&1 || { echo "node not found (set NODE_BIN) — the source guards and the seed step need it"; installer_fail prereqs; }
step "node:  $("$NODE_BIN" --version)"

BUILD="$ROOT/build-release"
DIST_OUT="$ROOT/dist"
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); step "ninja: found (fast incremental builds)"; else step "ninja: not found — using the default generator"; fi
stage_done

# ---- 2. Release build ---------------------------------------------------------------------------------
# cmake/ninja/make print their own running counter ([123/860] or [ 42%]) — streamed straight through,
# not captured, so the build is visible as it happens.
stage_begin "Building Release (build-release/) — the first build of a tree takes several minutes"
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    step "configuring (first run in this tree)"
    cmake -S "$ROOT" -B "$BUILD" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DBPS_NODE_EXE="$(command -v "$NODE_BIN")" \
        || installer_fail configure
else
    step "reusing the existing configuration (only changed files rebuild)"
fi
step "compiling appVGRPresenterUI + bps_cli on $(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?') cores"
cmake --build "$BUILD" --target appVGRPresenterUI bps_cli --parallel || installer_fail build
stage_done

# ---- 3. SEED ------------------------------------------------------------------------------------------
stage_begin "Seeding the shipped data"
seed_catalogs "$ROOT" "$BUILD/bps_cli" || installer_fail seed
stage_done

# ---- 4. Package (this also snapshots the configuration next to the executable) --------------------------
export RELEASE_VERSION="$VER" QT_ROOT_DIR="$QT_PREFIX"
mkdir -p "$DIST_OUT"
stage_begin "Packaging ($PLATFORM)"
if [ "$PLATFORM" = linux ]; then
    bash "$ROOT/tools/package_linux.sh" "$BUILD" "$DIST_OUT" || installer_fail package
    ARTIFACT="$(release_artifact "$DIST_OUT" "$VER" linux AppImage)"
else
    bash "$ROOT/tools/package_macos.sh" "$BUILD" "$DIST_OUT" || installer_fail package
    ARTIFACT="$(release_artifact "$DIST_OUT" "$VER" macos dmg)"
fi
[ -f "$ARTIFACT" ] || { echo "expected $ARTIFACT was not produced"; installer_fail package; }

SIZE_MB="$(( $(wc -c <"$ARTIFACT") / 1024 / 1024 ))"
stage_done
echo
echo "installer OK: ${ARTIFACT#$ROOT/} (${SIZE_MB} MB) — total $(fmt_elapsed $((SECONDS - RELEASE_T0)))"
echo "INSTALLER: PASS"
