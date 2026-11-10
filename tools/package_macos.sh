#!/usr/bin/env bash
# The packaging step of the macOS release: a signed .app inside a .dmg. Normally run BY
# tools/make_installer.sh (the one release command); it can also be run alone on a finished build:
#
#   tools/package_macos.sh [build-dir] [out-dir]
#
# Needs: appVGRPresenterUI.app in the build dir and Qt's macdeployqt (QT_ROOT_DIR/bin or on PATH).
#   MACOS_SIGN_IDENTITY  a "Developer ID Application: …" identity. Unset = ad-hoc signing ("-"), which is
#                        what lets the app launch on the machine that built it (not notarised, so other
#                        Macs will warn). Notarisation is a separate, credentialed step.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/unix_release.sh
. "$ROOT/tools/lib/unix_release.sh"

BUILD="${1:-$ROOT/build-release}"
OUT="${2:-$ROOT/dist}"
APP_BUNDLE="$BUILD/appVGRPresenterUI.app"
VERSION="${RELEASE_VERSION:-$(release_version "$ROOT")}"

[ -d "$APP_BUNDLE" ] || { echo "package_macos: $APP_BUNDLE not found — build first" >&2; exit 1; }
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/VGR Presenter.app"
step "[1/5] copying the app bundle"
cp -R "$APP_BUNDLE" "$STAGE"

MACDEPLOYQT="${QT_ROOT_DIR:+$QT_ROOT_DIR/bin/}macdeployqt"
command -v "$MACDEPLOYQT" >/dev/null 2>&1 || MACDEPLOYQT="$(command -v macdeployqt6 || command -v macdeployqt)"

# -qmldir pulls in every QML module the interface imports (Qt Multimedia's VideoOutput among them);
# macdeployqt also deploys the multimedia plugin folder that holds the FFmpeg backend.
step "[2/5] deploying Qt frameworks, QML modules and multimedia plugins (macdeployqt) — a few minutes"
with_heartbeat "macdeployqt" "$MACDEPLOYQT" "$STAGE" -qmldir="$ROOT/qml" -always-overwrite

# THE SHIPPED CONFIGURATION lands beside the executable (the app reads assets/settings.shipped.json from
# its own folder), and the seed data travels in Resources/ — the app lays it down once, on a fresh
# profile (see seedFirstRun in src/main.cpp), never over anything that exists. Done BEFORE signing: a
# signature covers the bundle's contents.
step "[3/5] bundling the shipped configuration and seed data"
seed_config "$ROOT" "$STAGE/Contents/MacOS"
if [ -d "$ROOT/installer/seed-data" ]; then
    cp -R "$ROOT/installer/seed-data" "$STAGE/Contents/Resources/seed-data"
fi

if [ -n "${MACOS_SIGN_IDENTITY:-}" ]; then SIGN_NOTE="Developer ID identity"
else SIGN_NOTE="ad-hoc — not notarised, other Macs will warn"; fi
step "[4/5] signing ($SIGN_NOTE)"
with_heartbeat "codesign" codesign --force --deep --options runtime --timestamp=none -s "${MACOS_SIGN_IDENTITY:--}" "$STAGE"

step "[5/5] building the disk image"
DMG="$(release_artifact "$OUT" "$VERSION" macos dmg)"
rm -f "$DMG"
ln -s /Applications "$WORK/Applications"
with_heartbeat "hdiutil" hdiutil create -volname "VGR Presenter $VERSION" -srcfolder "$WORK" -ov -format UDZO "$DMG"
echo "package_macos: wrote ${DMG#$ROOT/}"
