#!/usr/bin/env bash
# ============================================================================
# tools/set_version.sh <version> — single-source the app version.
#
# The version lives in exactly TWO files (everything else READS them):
#
#   qml/services/SettingsService.cpp   kAppVersion — the canonical value; the
#                                      QML binds it live via the exposed
#                                      `SettingsService.appVersion` property
#                                      (NavRail, AppMenuBar, MainScreen,
#                                      GeneralScreen), so the UI can never
#                                      drift from the shipped build.
#   assets/app.rc.in                   Windows FILEVERSION/PRODUCTVERSION
#                                      (numeric quad + the string form).
#
# The script also GUARDS the QML: if a hardcoded version literal ever
# reappears there (the drift that started this), the run fails loudly.
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

# ---- 1. the canonical definition --------------------------------------------
# SettingsService.cpp is the source of truth: replace its value whatever it is.
sed -i -E "s/(kAppVersion = )\"[^\"]*\"/\1\"$NEW\"/" "$SETTINGS"

# ---- 2. the Windows resource file -------------------------------------------
RC="$ROOT/assets/app.rc.in"
[ -f "$RC" ] || { echo "set_version.sh: missing $RC"; exit 1; }
sed -i -E \
    -e "s/^ FILEVERSION.*/ FILEVERSION    ${QUAD}/" \
    -e "s/^ PRODUCTVERSION.*/ PRODUCTVERSION ${QUAD}/" \
    -e "s/(VALUE \"FileVersion\",[[:space:]]*)\"[^\"]*\"/\1\"$NEW\"/" \
    -e "s/(VALUE \"ProductVersion\",[[:space:]]*)\"[^\"]*\"/\1\"$NEW\"/" \
    "$RC"

# ---- 3. verify ----------------------------------------------------------------
# (a) no OTHER version string may survive in the two owned files;
# (b) no hardcoded version literal may reappear in the QML — the UI binds
#     SettingsService.appVersion, and a literal means somebody reintroduced
#     the drift this script exists to prevent.
BAD=""
for f in "$SETTINGS" "$RC"; do
    if grep -nE "\"v?[0-9]+\.[0-9]+\.[0-9]+" "$f" | grep -vq "v\?$NEW"; then
        BAD="$BAD $f"
    fi
done
for f in \
    "$ROOT/qml/components/NavRail.qml" \
    "$ROOT/qml/screens/main/AppMenuBar.qml" \
    "$ROOT/qml/screens/main/VGRPresenterMainScreen.qml"; do
    [ -f "$f" ] || { echo "set_version.sh: missing $f"; exit 1; }
    if grep -qE '\"v?[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z]+(\.[0-9]+)?)?\"' "$f"; then
        BAD="$BAD $f (hardcoded version literal — bind SettingsService.appVersion instead)"
    fi
done
if [ -n "$BAD" ]; then
    echo "set_version.sh: version drift detected in:$BAD"
    exit 1
fi

echo "set_version.sh: OK — $NEW is the single version (QML reads it live)"
echo "  (rebuild to bake it into the exe: the resource + all UI screens read it at build/run time)"
