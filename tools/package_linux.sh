#!/usr/bin/env bash
# The packaging step of the Linux release: AppImage + tar.gz of the AppDir. Normally run BY
# tools/make_installer.sh (the one release command); it can also be run alone on a finished build:
#
#   tools/package_linux.sh [build-dir] [out-dir]
#
# Needs: appVGRPresenterUI in the build dir, Qt's qmake (QT_ROOT_DIR or on PATH), curl, and optionally
# ImageMagick's `convert` for the icon. Qt's multimedia backend (FFmpeg) is a PLUGIN, so it is deployed
# explicitly — without it the camera, screen capture and media audio the Linux build relies on would be
# missing from the package.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/unix_release.sh
. "$ROOT/tools/lib/unix_release.sh"

BUILD="${1:-$ROOT/build-release}"
OUT="${2:-$ROOT/dist}"
APP=appVGRPresenterUI
VERSION="${RELEASE_VERSION:-$(release_version "$ROOT")}"
ARCH="$(uname -m)"

[ -x "$BUILD/$APP" ] || { echo "package_linux: $BUILD/$APP not found — build first" >&2; exit 1; }
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/VGRPresenter.AppDir"
SHARE="$APPDIR/usr/share/vgrpresenter"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" "$SHARE"

step "[1/6] laying out the AppDir"
cp "$BUILD/$APP" "$APPDIR/usr/bin/"

# THE SHIPPED CONFIGURATION lands beside the executable (the app reads assets/settings.shipped.json from
# its own folder), and the seed data travels in share/ — the app lays it down once, on a fresh profile
# (see seedFirstRun in src/main.cpp), never over anything that exists.
step "[2/6] bundling the shipped configuration and seed data"
seed_config "$ROOT" "$APPDIR/usr/bin"
if [ -d "$ROOT/installer/seed-data" ]; then
    cp -R "$ROOT/installer/seed-data" "$SHARE/seed-data"
fi

cat >"$APPDIR/usr/share/applications/vgrpresenter.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=VGR Presenter
Comment=Live presentation and production
Exec=$APP
Icon=vgrpresenter
Categories=AudioVideo;Video;
Terminal=false
EOF

step "[3/6] desktop entry and icon"
ICON="$APPDIR/usr/share/icons/hicolor/256x256/apps/vgrpresenter.png"
if command -v convert >/dev/null 2>&1 && convert "$ROOT/assets/app-icon.ico[0]" -resize 256x256 "$ICON" 2>/dev/null; then
    :
else
    cp "$ROOT/assets/brand-lockup.png" "$ICON"   # not square, but a valid icon for the desktop entry
fi

QMAKE="${QT_ROOT_DIR:+$QT_ROOT_DIR/bin/}qmake"
command -v "$QMAKE" >/dev/null 2>&1 || QMAKE="$(command -v qmake6 || command -v qmake)"
export QMAKE
export QML_SOURCES_PATHS="$ROOT/qml"
export EXTRA_QT_PLUGINS="${EXTRA_QT_PLUGINS:-multimedia;imageformats;platforms;xcbglintegrations;platformthemes}"
export VERSION

TOOLS="$WORK/tools"; mkdir -p "$TOOLS"
LD_URL="${LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage}"
LDQT_URL="${LINUXDEPLOY_QT_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage}"
step "[4/6] fetching linuxdeploy + its Qt plugin (~30 MB)"
# A progress bar on a terminal; quiet-but-errors in CI logs (a bar there is thousands of lines).
CURL_PROGRESS=(-sS); [ -t 2 ] && CURL_PROGRESS=(-#)
curl -fL "${CURL_PROGRESS[@]}" "$LD_URL" -o "$TOOLS/linuxdeploy" && chmod +x "$TOOLS/linuxdeploy"
curl -fL "${CURL_PROGRESS[@]}" "$LDQT_URL" -o "$TOOLS/linuxdeploy-plugin-qt" && chmod +x "$TOOLS/linuxdeploy-plugin-qt"

# CI runners have no FUSE: let the tools unpack themselves instead of mounting.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
step "[5/6] deploying Qt (libraries, QML modules, multimedia plugins) and building the AppImage — the slow part, a few minutes"
with_heartbeat "linuxdeploy" bash -c 'cd "$1" && shift && linuxdeploy "$@"' _ "$WORK" \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/vgrpresenter.desktop" \
    --icon-file "$ICON" --plugin qt --output appimage

step "[6/6] writing the outputs to ${OUT#$ROOT/}"
APPIMAGE="$(release_artifact "$OUT" "$VERSION" linux AppImage)"
mv "$WORK"/VGR*.AppImage "$APPIMAGE"
TARBALL="$(release_artifact "$OUT" "$VERSION" linux tar.gz)"
with_heartbeat "compressing the AppDir" tar -C "$WORK" -czf "$TARBALL" VGRPresenter.AppDir
echo "package_linux: wrote ${APPIMAGE#$ROOT/} (+ .tar.gz)"
