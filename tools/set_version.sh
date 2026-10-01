#!/usr/bin/env bash
# ============================================================================
# tools/set_version.sh <version> — single-source the app version.
#
# The version used to live in FIVE places that drifted independently (the
# About menu said "v1.0.5-beta" while everywhere else said "v1.0.5-beta.2").
# This script rewrites them all to one value:
#
#   qml/services/SettingsService.cpp              kAppVersion (canonical)
#   qml/components/NavRail.qml                    appVersion
#   qml/screens/main/AppMenuBar.qml               About row + header subtitle
#   qml/screens/main/VGRPresenterMainScreen.qml   the version line
#   assets/app.rc.in                              Windows FILEVERSION/PRODUCTVERSION
#                                                 (numeric quad + the string form)
#
# The canonical version is read back from SettingsService.cpp, so the script
# never needs to know what it is changing FROM.
#
# Usage:
#   tools/set_version.sh 1.0.5-beta.3
#   tools/set_version.sh 1.0.6
#
# The Windows numeric quad: 1.0.5-beta.3 -> 1,0,5,3 (the beta number is the
# 4th field, 0 for a final release) — Explorer's Properties reads the string
# form, the loader cares about the quad's ordering only.
# ============================================================================
set -euo pipefail

NEW="${1:?usage: set_version.sh <version>  (e.g. 1.0.5-beta.3 or 1.0.6)}"
if ! [[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z]+\.[0-9]+)?$ ]]; then
    echo "set_version.sh: '$NEW' is not a version (expected 1.2.3 or 1.2.3-beta.4)"
    exit 2
fi

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
SETTINGS="$ROOT/qml/services/SettingsService.cpp"

# The canonical CURRENT version, read from the one file that defines it.
OLD="$(grep -oE 'kAppVersion = "[^"]+"' "$SETTINGS" | head -1 | sed -E 's/.*"([^"]+)"/\1/')"
if [ -z "$OLD" ]; then
    echo "set_version.sh: could not read kAppVersion from $SETTINGS"
    exit 1
fi


# Numeric quad for the Windows resource: 1.0.5-beta.3 -> 1,0,5,3
NUM="${NEW%%-*}"                    # 1.0.5
BETA=0
case "$NEW" in *-*.*) BETA="${NEW##*.}" ;; esac   # beta number, else 0
IFS=. read -r MA MI PA <<< "$NUM"
QUAD="${MA},${MI},${PA},${BETA}"

echo "set_version.sh: $OLD -> $NEW (Windows quad $QUAD)"

# ---- 1. the canonical definition + every QML display string -----------------
# SettingsService.cpp is the source of truth: replace its value whatever it is.
sed -i -E "s/(kAppVersion = )\"[^\"]*\"/\1\"$NEW\"/" "$SETTINGS"

# The QML files show the version with a "v" prefix, and one of them had
# DRIFTED to a truncated "v1.0.5-beta" (suffix WITHOUT a number) that an
# exact-string replace cannot catch (that drift is the reason this script
# exists). So: normalize ANY quoted version-shaped string — the -suffix part
# may or may not carry .N — to "v$NEW".
VERSION_RE='"v?[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z]+(\.[0-9]+)?)?"'
for f in \
    "$ROOT/qml/components/NavRail.qml" \
    "$ROOT/qml/screens/main/AppMenuBar.qml" \
    "$ROOT/qml/screens/main/VGRPresenterMainScreen.qml"; do
    if [ ! -f "$f" ]; then
        echo "set_version.sh: missing $f"
        exit 1
    fi
    sed -i -E "s/$VERSION_RE/\"v$NEW\"/g" "$f"
done

# ---- 2. the Windows resource file -------------------------------------------
RC="$ROOT/assets/app.rc.in"
[ -f "$RC" ] || { echo "set_version.sh: missing $RC"; exit 1; }
sed -i -E \
    -e "s/^ FILEVERSION.*/ FILEVERSION    ${QUAD}/" \
    -e "s/^ PRODUCTVERSION.*/ PRODUCTVERSION ${QUAD}/" \
    -e "s/(VALUE \"FileVersion\",[[:space:]]*)\"[^\"]*\"/\1\"$NEW\"/" \
    -e "s/(VALUE \"ProductVersion\",[[:space:]]*)\"[^\"]*\"/\1\"$NEW\"/" \
    "$RC"

# ---- 3. verify: no OTHER version string may survive --------------------------
# Not just OLD — ANY remaining version-shaped string (a drifted variant like
# "v1.0.5-beta" is exactly what this guards against).
LEFT="$(grep -rlnE "[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z]+\.[0-9]+)?" \
    "$SETTINGS" \
    "$ROOT/qml/components/NavRail.qml" \
    "$ROOT/qml/screens/main/AppMenuBar.qml" \
    "$ROOT/qml/screens/main/VGRPresenterMainScreen.qml" 2>/dev/null \
    | xargs -r grep -lnE "v?[0-9]+\.[0-9]+\.[0-9]+" 2>/dev/null || true)"
BAD=""
for f in $LEFT; do
    # A line is only a problem when it holds a version string that is NOT $NEW.
    if grep -nE "\"v?[0-9]+\.[0-9]+\.[0-9]+" "$f" | grep -vq "v\?$NEW"; then
        BAD="$BAD $f"
    fi
done
if [ -n "$BAD" ]; then
    echo "set_version.sh: a different version string survived in:$BAD"
    exit 1
fi

echo "set_version.sh: OK — $NEW is now the single version"
echo "  (rebuild to bake it into the exe: the resource + About screens read it at build/run time)"
