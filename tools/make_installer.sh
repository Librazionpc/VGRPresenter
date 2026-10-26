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

# ---- 1c. SEED: this machine's Shows library, Bibles and The Table -----------
# installer/README.md's own framing: "your show files, sermon library, Bibles,
# settings... so nobody starts from an empty app" — but until now nothing
# actually copied the library/Bibles/Table content itself, only the catalogs
# and configuration above. A fresh install's Scripture and The Table tabs came
# up EMPTY (user report), because seed-data/local never had the_table.json
# and seed-data/roaming/enginedata never had bibles.json. Runs on EVERY
# release build — the content on whichever machine builds the release is what
# ships, not a one-off manual copy — and still only ever lands on a FRESH
# install (installer.iss's onlyifdoesntexist): an existing user's own shows,
# Bibles and Table are never touched.
echo "== bundling this machine's Shows library, Bibles and The Table =="
DOCS="$(cygpath -u "$(cygpath -F 0x05)")"         # Documents (OneDrive-redirect-safe)
ROAMING_SRC="$(cygpath -u "$APPDATA")/VGR/VGRPresenter"
LOCAL_SRC="$(cygpath -u "$LOCALAPPDATA")/bps"

# Shows library (the song/sermon categories a show-list screenshot showed
# empty on a fresh install) -> its OWN seed path, not roaming/local:
# ShowService's default library lives in Documents\VGR Presenter\Shows (see
# its own header comment), a different folder than either of those two.
# .deleted is the library's own recycle bin — never starter content.
SHOWS_SRC="$DOCS/VGR Presenter/Shows"
if [ -d "$SHOWS_SRC" ]; then
    rm -rf "$SEED_DIR/shows"
    mkdir -p "$SEED_DIR/shows"
    find "$SHOWS_SRC" -mindepth 1 -maxdepth 1 ! -name '.deleted' \
        -exec cp -r {} "$SEED_DIR/shows/" \;
    echo "  shows library: $(du -sh "$SEED_DIR/shows" 2>/dev/null | cut -f1)"
fi

# Bibles -> seed-data/roaming/enginedata (an existing seed path — kernel.json
# already lives there). bibles.userdata.json is personal marks/notes, not
# starter content, and is deliberately left out.
if [ -f "$ROAMING_SRC/enginedata/bibles.json" ]; then
    mkdir -p "$SEED_DIR/roaming/enginedata"
    cp "$ROAMING_SRC/enginedata/bibles.json" "$SEED_DIR/roaming/enginedata/bibles.json"
    echo "  bibles: $(du -sh "$SEED_DIR/roaming/enginedata/bibles.json" | cut -f1)"
fi

# The Table (sermon library) -> seed-data/local (an existing seed path).
# Only the live json + its converted-texts cache ship; the .bak* variants and
# the cache/backups/recovery folders are this machine's own safety nets, not
# content for a new user.
if [ -f "$LOCAL_SRC/the_table.json" ]; then
    cp "$LOCAL_SRC/the_table.json" "$SEED_DIR/local/the_table.json"
    rm -rf "$SEED_DIR/local/the_table_texts"
    if [ -d "$LOCAL_SRC/the_table_texts" ]; then
        cp -r "$LOCAL_SRC/the_table_texts" "$SEED_DIR/local/the_table_texts"
    fi
    echo "  the table: $(du -sh "$SEED_DIR/local/the_table.json" | cut -f1)"
fi

# The user's OWN templates -> seed-data/local (an existing seed path, same
# file name bps_cli --dump-catalogs would otherwise write). Templates ship
# EMPTY from the engine's own built-in catalog by design (see
# DesignCatalogs.hpp: "ships nothing: the user's own layouts and categories
# only") — unlike overlays, there is no shipped starter set to dump, so the
# only way a fresh install gets any templates at all is bundling the
# operator's own live file, same as The Table above. This DOES carry
# whatever font names the templates reference (the exact risk
# SeedStoreJson's header comment warns about for the built-in catalogs) —
# deliberately accepted here per an explicit user call, not the accidental
# machine-fonts leak that comment describes. A font genuinely MISSING on a
# recipient's machine just falls back to a default font; it does not break
# anything.
if [ -f "$LOCAL_SRC/templates.json" ]; then
    cp "$LOCAL_SRC/templates.json" "$SEED_DIR/local/templates.json"
    echo "  templates: $(du -sh "$SEED_DIR/local/templates.json" | cut -f1)"
fi

# ---- 2. clean transient state the smoke test wrote into dist/ ----------------
rm -rf "$DIST/logs" "$DIST/enginedata" "$DIST/crashes"

# ---- 3. compile the installer -------------------------------------------------
echo "== compiling installer (Inno Setup) =="
# MSYS2_ARG_CONV_EXCL: without it bash rewrites the /D define into a Windows
# path and ISCC sees "more than one script filename".
# Streamed through `tee`, not silently redirected: ISCC's own per-file compile/
# compress lines (this is the slowest step — LZMA2 solid-compressing the whole
# deployed app) now print live instead of the terminal sitting blank until it
# either finishes or fails (user report: "so I can [tell] if it's moving").
# `set -euo pipefail` (top of file) makes `if !` see ISCC's own exit code, not
# tee's, so a real failure still trips this branch.
if ! MSYS2_ARG_CONV_EXCL="*" "$ISCC" "/DAPP_VERSION=$VER" "$(cygpath -w "$ROOT/tools/installer.iss")" \
        2>&1 | tee /tmp/iscc.log; then
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
