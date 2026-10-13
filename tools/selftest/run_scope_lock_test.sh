#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_scope_lock_test.sh — does the book pill really scope search?
#
# The feature (user call: "add a chapter lock search ... if i turn the lock for
# the book all my search goes against that book alone and not all the books"):
# a pill beside the padlock in the Scripture and The Table search fields. Open a
# passage, light the pill, and the search covers ONLY that passage — the chapter
# being read in a Bible, the sermon being read in The Table.
#
# This drives the REAL app on BOTH tabs: opens the first year's first sermon (The
# Table) and the first Bible's first book and chapter (Scripture), searches one
# word across the library, then CLICKS the pill and searches again. Judged, per
# tab:
#   * the loose pass really did span places (places > 1) — otherwise "the lock
#     changed nothing" would pass by accident;
#   * every locked row falls INSIDE the open passage (outside === 0), where
#     inside is re-derived from the pane's own book id + chapter number, not
#     taken from the lock;
#   * the locked pass comes from ONE place (places === 1).
#
# VERDICT (last line, greppable):
#   SCOPE: PASS tableRows=<n> tableOutside=0 scriptureRows=<n> scriptureOutside=0
#   SCOPE: FAIL launch | FAIL missing | FAIL table-outside=<n> | FAIL table-places=<n>
#         | FAIL table-narrow | FAIL scripture-outside=<n> | FAIL scripture-places=<n>
#         | FAIL scripture-narrow
#
# Usage:  bash tools/selftest/run_scope_lock_test.sh [wait-seconds]   (default 150)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/scope_lock_test.log"
WAIT_SECS="${1:-150}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "SCOPE: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_SCOPELOCK_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SCOPELOCK_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario quits the app itself once both tabs' locked rows are read back.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-SCOPE\] result" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-SCOPE\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-SCOPE\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-SCOPE result line — the scenario never finished; see $LOG)"
    echo "SCOPE: FAIL missing"
    exit 3
fi

num() { echo "$RESULT" | grep -o "$1=[0-9-]*" | head -1 | cut -d= -f2; }

TABLE_PLACES="$(num tableLoose)"
TABLE_OUTSIDE="$(num tableOutside)"
TABLE_ROWS="$(num tableRows)"
TABLE_BY="$(echo "$RESULT" | grep -o 'tableBy=[a-z]*' | head -1 | cut -d= -f2)"
SCR_PLACES="$(num scriptureLoose)"
SCR_OUTSIDE="$(num scriptureOutside)"
SCR_ROWS="$(num scriptureRows)"
SCR_BY="$(echo "$RESULT" | grep -o 'scriptureBy=[a-z]*' | head -1 | cut -d= -f2)"

if [ "$TABLE_OUTSIDE" != "0" ]; then
    echo "SCOPE: FAIL table-outside=$TABLE_OUTSIDE — a locked sermon row came from outside the open sermon"
    exit 4
fi
if [ -z "$TABLE_PLACES" ] || [ "$TABLE_PLACES" -le 1 ]; then
    echo "SCOPE: FAIL table-places=$TABLE_PLACES — the loose pass did not span places, so the lock proves nothing"
    exit 5
fi
if [ -z "$TABLE_ROWS" ] || [ "$TABLE_ROWS" = "0" ]; then
    echo "SCOPE: FAIL table-narrow — the locked search returned no sermon row at all"
    exit 6
fi
if [ "$TABLE_BY" = "missed" ]; then
    echo "SCOPE: FAIL table-narrow — the pill never turned on (click missed and the handler did not run)"
    exit 7
fi

if [ "$SCR_PLACES" != "skipped" ]; then
    if [ "$SCR_OUTSIDE" != "0" ]; then
        echo "SCOPE: FAIL scripture-outside=$SCR_OUTSIDE — a locked verse row came from outside the open chapter"
        exit 8
    fi
    if [ -z "$SCR_PLACES" ] || [ "$SCR_PLACES" -le 1 ]; then
        echo "SCOPE: FAIL scripture-places=$SCR_PLACES — the loose pass did not span places"
        exit 9
    fi
    if [ -z "$SCR_ROWS" ] || [ "$SCR_ROWS" = "0" ]; then
        echo "SCOPE: FAIL scripture-narrow — the locked search returned no verse row at all"
        exit 10
    fi
    if [ "$SCR_BY" = "missed" ]; then
        echo "SCOPE: FAIL scripture-narrow — the pill never turned on"
        exit 11
    fi
fi

echo "SCOPE: PASS tableRows=${TABLE_ROWS} tableOutside=0 tableBy=${TABLE_BY:-?} scriptureRows=${SCR_ROWS:-skipped} scriptureOutside=0 scriptureBy=${SCR_BY:-skipped}"
exit 0
