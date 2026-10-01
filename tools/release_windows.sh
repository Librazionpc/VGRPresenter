#!/usr/bin/env bash
# ============================================================================
# tools/release_windows.sh [version] — cut a Windows release.
#
# Release format: version tags, ✨ New features / 🔧 Tweaks / 🐞 Bugfixes
# note sections, Pre-release flag for betas, and the downloadable zip
# attached as the release asset.
#
# Steps:
#   1. (optional) bump the version everywhere — tools/set_version.sh <version>
#   2. build the portable dist + zip     — tools/make_portable.sh
#   3. write dist/RELEASE_NOTES.md (a skeleton with the note sections, if
#      one does not exist yet) and create the GitHub release with the zip
#      attached — via the gh CLI. When gh is missing or not authenticated the
#      exact manual steps are printed instead.
#
# Usage:
#   tools/release_windows.sh                  # release the CURRENT version
#   tools/release_windows.sh 1.0.5-beta.3     # bump, then release
#   tools/release_windows.sh --notes-only     # just (re)write the notes skeleton
#
# Pre-releases: a version containing "-" (1.0.5-beta.3) is published with
# --prerelease, exactly like FreeShow's betas. A plain 1.0.5 is a full release.
# ============================================================================
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
SETTINGS="$ROOT/qml/services/SettingsService.cpp"
ZIP="$ROOT/dist/VGRPresenter-win64.zip"
NOTES="$ROOT/dist/RELEASE_NOTES.md"

# ---- version -----------------------------------------------------------------
if [ "${1:-}" = "--notes-only" ]; then
    NOTES_ONLY=1
else
    NOTES_ONLY=0
    if [ -n "${1:-}" ]; then
        bash "$ROOT/tools/set_version.sh" "$1"
    fi
fi

VER="$(grep -oE 'kAppVersion = "[^"]+"' "$SETTINGS" | head -1 | sed -E 's/.*"([^"]+)"/\1/')"
if [ -z "$VER" ]; then
    echo "release_windows.sh: could not read kAppVersion from $SETTINGS"
    exit 1
fi
TAG="v$VER"

PRERELEASE=""
case "$VER" in *-*) PRERELEASE="--prerelease" ;; esac

# ---- notes skeleton (the FreeShow section format) ----------------------------
if [ ! -f "$NOTES" ]; then
    mkdir -p "$ROOT/dist"
    cat > "$NOTES" <<'EOF'
## ✨ New features:

## 🔧 Tweaks:

## 🐞 Bugfixes:

EOF
    echo "release_windows.sh: wrote a notes skeleton to dist/RELEASE_NOTES.md"
    echo "  Fill in the three sections, then run this script again to publish."
fi

if [ "$NOTES_ONLY" = 1 ]; then
    echo "release_windows.sh: notes skeleton ready at $NOTES"
    exit 0
fi

# ---- build the downloadable zip ---------------------------------------------
bash "$ROOT/tools/make_portable.sh"
[ -f "$ZIP" ] || { echo "release_windows.sh: $ZIP missing after make_portable.sh"; exit 1; }

# ---- publish -----------------------------------------------------------------
if command -v gh >/dev/null 2>&1; then
    if gh auth status >/dev/null 2>&1; then
        echo "== creating GitHub release $TAG =="
        gh release create "$TAG" "$ZIP" \
            --title "$TAG" $PRERELEASE \
            --notes-file "$NOTES"
        echo "release_windows.sh: $TAG published (asset: $(basename "$ZIP"))"
        exit 0
    fi
    echo "release_windows.sh: gh is installed but not authenticated."
    echo "  Run once:  gh auth login"
else
    echo "release_windows.sh: gh CLI is not installed."
    echo "  Install once (any of):  winget install GitHub.cli   |   scoop install gh"
fi

cat <<EOF

Manual release steps (once gh is ready, or from the GitHub web UI):
  1. Push the version bump first:      git push origin main
  2. Create the tag + release:         gh release create $TAG "$ZIP" \\
                                           --title "$TAG" $PRERELEASE \\
                                           --notes-file "$NOTES"
     (web UI: Releases -> Draft a new release -> tag "$TAG" ->
      attach $(basename "$ZIP") -> paste the notes)
  3. Beta versions: tick "Set as a pre-release" (the script's --prerelease does this).

The zip is self-contained: Windows 10+, no Qt/MinGW installs needed on the target PC.
EOF
