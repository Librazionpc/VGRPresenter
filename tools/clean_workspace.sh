#!/usr/bin/env bash
# Clears the debugging litter out of the working tree so the project stays easy to read:
# stray run logs, probe screenshots, throwaway probe sources, scratch build trees and old
# installers. NOTHING that a build, the installer or the app needs is touched.
#
#   tools/clean_workspace.sh            # dry run: lists what would go, with sizes
#   tools/clean_workspace.sh --apply    # does it
#
# What it keeps, on purpose:
#   * the engine log (logs/engine.log and build/logs/engine.log) — the one log that matters;
#   * build/ itself (binaries, CMake state, the generated ep_compat.cpp, gdb_*.gdb helpers),
#     build-release/ (make_installer.sh and make_portable.sh build from it), dev-profile/,
#     backups/, .venv/, and the newest installer + portable build in dist/;
#   * build/test_imports_quarantine/ (looks like import test data — reported, never deleted).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1

cd "$ROOT" || exit 1
# Never run from anywhere but the project root.
[ -f CMakeLists.txt ] && [ -d src ] || { echo "not the project root: $ROOT" >&2; exit 2; }

LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT

add() { [ -e "$1" ] && printf '%s\n' "$1" >> "$LIST"; }

# ---- repo root: run logs, throwaway probe pages, test litter -----------------------------
for f in ./*.log _paste_view.html probe.html fontprobe.html; do add "$f"; done
for d in tmp_diag screenshots recordings .cloud-attachments; do add "$d"; done

# ---- logs/ and build/logs/: everything except engine.log ---------------------------------
for dir in logs build/logs; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' f; do
        [ "$(basename "$f")" = "engine.log" ] || add "$f"
    done < <(find "$dir" -mindepth 1 -maxdepth 1 -print0)
done

# ---- build/: debugging files dropped next to the binaries --------------------------------
if [ -d build ]; then
    while IFS= read -r -d '' f; do
        n="$(basename "$f")"
        case "$n" in
            # real build inputs / outputs — keep
            CMakeCache.txt|compile_commands.json|ep_compat.cpp|appvgrpresenterui_qmltyperegistrations.cpp) ;;
            app.rc|build.ninja|cmake_install.cmake|CTestTestfile.cmake|gdb_cmds.txt|gdb_go_live.gdb) ;;
            *.exe|*.a|*.dll|*.manifest) ;;
            # debugging litter
            *.log|*.png|*.out|*.err|*.pid|*.bak|*.d|*.ps1|*.mjs|*.js|*.html|*.qml|*.vgr|*.cpp|*.txt) add "$f" ;;
        esac
    done < <(find build -mindepth 1 -maxdepth 1 -type f -print0)
    for d in dumps probe shadertest tmpdiag tmp stkfqCMz sthtSKzR dev-cycle-tmp recordings; do
        add "build/$d"
    done
fi

# ---- a second, unused build tree ---------------------------------------------------------
add build-verify

# ---- dist/: keep only the newest installer (and its portable build) ----------------------
if [ -d dist ]; then
    newest="$(ls dist/VGRPresenter-Setup-*.exe 2>/dev/null | sort -V | tail -1)"
    for f in dist/VGRPresenter-Setup-*.exe; do
        [ -e "$f" ] && [ "$f" != "$newest" ] && add "$f"
    done
    echo "dist/ keeps: ${newest:-<no installer>} + VGRPresenter/ + VGRPresenter-win64.zip"
fi

if [ ! -s "$LIST" ]; then
    echo "Nothing to clean."
    exit 0
fi

total=0
while IFS= read -r p; do
    kb=$(du -sk "$p" 2>/dev/null | cut -f1); kb=${kb:-0}
    total=$((total + kb))
    printf '%9s KB  %s\n' "$kb" "$p"
done < "$LIST"
echo "-----"
printf '%9s MB  total in %s items\n' "$((total / 1024))" "$(wc -l < "$LIST")"

if [ "$APPLY" -eq 1 ]; then
    # engine.log is held open by a running app and is never in the list, so the app can stay up.
    while IFS= read -r p; do rm -rf -- "$p"; done < "$LIST"
    echo "Removed."
else
    echo "Dry run — nothing was deleted. Re-run with --apply."
fi
[ -d build/test_imports_quarantine ] && echo "note: build/test_imports_quarantine/ left alone (looks like import test data)."
exit 0
