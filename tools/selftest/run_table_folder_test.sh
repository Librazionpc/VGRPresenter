#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_table_folder_test.sh — what does the folder-import notice
# actually SAY on screen?
#
# The complaint behind it: "Add sermons folder" over a mixed folder answered
# "40 imported" and said nothing about the 300 files it would not read, so the
# operator could not tell which files to fix. The engine now names every refusal
# ("<file>: <reason>"), and the service draws that list in one toast — but the
# wording of that toast was covered only by the engine's unit tests, which see
# the ImportReport and never the notice. This runs the REAL sweep (the service
# call the folder picker makes, minus the dialog a script cannot answer) over a
# folder of files that are all refused, reads the notices the app raised, and
# asserts the sentence the operator reads names every one of them.
#
# WHY A FOLDER OF ONLY-REFUSED FILES: the app writes into the operator's OWN
# sermon library (%LOCALAPPDATA%/bps/the_table.json) and indexes what it imports.
# With nothing importable in the folder, the sweep stores nothing, skips its save
# and skips the reindex — so the run leaves the user's library untouched, which
# this script also verifies (the library file's digest before and after).
#
# VERDICT (last line, greppable):
#   TABLEFOLDER: PASS unsupported=7 failed=1 named=8 cards=2 matched=yes
#   TABLEFOLDER: FAIL launch | FAIL missing | FAIL summary | FAIL named=…
#                | FAIL truncated | FAIL toast | FAIL library
#
# Usage:  bash tools/selftest/run_table_folder_test.sh [wait-seconds]  (default 120)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/table_folder_test.log"
ELOG="$BUILD/logs/engine.log"
SRC="$BUILD/logs/table_folder_src"
WAIT_SECS="${1:-120}"
LIBDIR="${LOCALAPPDATA:-$HOME/AppData/Local}/bps"
LIB="$LIBDIR/the_table.json"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "TABLEFOLDER: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

# ---- the folder: a sermon-shaped pile of files that are NOT sermons ----------
# 6 spreadsheets (ZIP magic, never read: the extension alone refuses them),
# 1 image, and 1 .txt that is really a spreadsheet (read, then refused by its own
# leading bytes) — eight refusals, three more than the old cap of five, so a
# still-truncated list cannot pass this run.
NAMES=(
    "58_0101_Sheet.xlsx" "58_0102_Sheet.xlsx" "58_0103_Sheet.xlsx"
    "58_0104_Sheet.xlsx" "58_0105_Sheet.xlsx" "58_0106_Sheet.xlsx"
    "58_0201_Chart.png"
    "58_0202_Renamed.txt"
)
rm -rf "$SRC"
mkdir -p "$SRC/1958" || { echo "TABLEFOLDER: FAIL launch — cannot create $SRC"; exit 2; }
for n in "${NAMES[@]}"; do
    case "$n" in
    *.png)  printf '\211PNG\r\n\032\n image bytes\n' > "$SRC/1958/$n" ;;
    # A .txt whose first bytes are a ZIP local-file header: the format gate
    # reads the bytes, not the name, and counts it as FAILED (a sermon file that
    # could not be read) rather than unsupported.
    *.txt)  printf 'PK\003\004\024\000 a spreadsheet named .txt\n' > "$SRC/1958/$n" ;;
    *)      printf 'PK\003\004\024\000 a spreadsheet\n' > "$SRC/1958/$n" ;;
    esac
done
echo "== folder under test: $SRC/1958 (${#NAMES[@]} files, none of them a sermon) =="
ls -1 "$SRC/1958" | sed 's/^/  /'

# A native path for the app: an MSYS path (/c/...) is not what the engine's
# filesystem resolves.
APPDIR="$(cygpath -w "$SRC" 2>/dev/null || echo "$SRC")"

LIB_BEFORE=""
if [ -f "$LIB" ]; then
    LIB_BEFORE="$(certutil -hashfile "$(cygpath -w "$LIB")" SHA256 2>/dev/null | sed -n 2p)"
    [ -z "$LIB_BEFORE" ] && LIB_BEFORE="$(md5sum "$LIB" 2>/dev/null | cut -d' ' -f1)"
fi
echo "== sermon library before: ${LIB_BEFORE:-<none>} ($LIB)"

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Log-line DELTA, not the whole file: engine.log is appended across runs.
BEFORE=$(wc -l < "$ELOG" 2>/dev/null || echo 0)
rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_TABLEFOLDER_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_TABLEFOLDER_TEST=1 VGR_TABLEFOLDER_DIR="$APPDIR" \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario imports the folder, reads the notices, reports and quits itself.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-TABLEFOLDER\] cards=" "$LOG" 2>/dev/null; then
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
grep -a "\[SELFTEST-TABLEFOLDER\]" "$LOG" 2>/dev/null | tr -d '\r' | sed 's/^/  /' || echo "  (none)"
echo "== sweep in the engine log (this run) =="
tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -a "Table import\|Folder import" \
    | tr -d '\r' | tail -5 | sed 's/^/  /' || true
echo "== QML errors (should be none) =="
{ grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null
  tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
      | grep -aE "QML|RangeError|Cannot read|TypeError"; } | tr -d '\r' | head -8

STARTED="$(grep -a "\[SELFTEST-TABLEFOLDER\] started=" "$LOG" 2>/dev/null | tail -1 \
           | grep -o 'started=[^ ]*' | cut -d= -f2)"
if [ "$STARTED" != "true" ]; then
    echo "TABLEFOLDER: FAIL launch — the sweep never started ($STARTED; see $LOG)"
    exit 4
fi

RESULT="$(grep -a "\[SELFTEST-TABLEFOLDER\] cards=" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-TABLEFOLDER cards line — the scenario never finished; see $LOG)"
    echo "TABLEFOLDER: FAIL missing"
    exit 3
fi

# The messages exactly as the app put them on screen (one per notice).
MESSAGES="$(grep -a "\[SELFTEST-TABLEFOLDER\] notice " "$LOG" 2>/dev/null | tr -d '\r')"
SUMMARY_MSG="$(echo "$MESSAGES" | grep -a "message=Imported " | sed 's/.*message=//' | tail -1)"
REFUSED_MSG="$(echo "$MESSAGES" | grep -a "message=Skipped " | sed 's/.*message=//' | tail -1)"
CARDS="$(echo "$RESULT" | grep -o 'cards=[^ ]*' | cut -d= -f2)"
LAST_MSG="$(echo "$RESULT" | sed 's/.*lastMessage=//')"

if [ -z "$SUMMARY_MSG" ]; then
    echo "TABLEFOLDER: FAIL summary — the sweep's own outcome line never reached a notice"
    exit 5
fi
if [ -z "$REFUSED_MSG" ]; then
    echo "TABLEFOLDER: FAIL summary — no refusal notice at all: the 8 files it would not read"
    echo "                   were reported by count only (the bug this test exists for)"
    echo "                   summary: $SUMMARY_MSG"
    exit 6
fi

# One reason per refused file: every name in the folder has to be IN the notice.
MISSING=""
NAMED=0
for n in "${NAMES[@]}"; do
    if echo "$REFUSED_MSG" | grep -qF "$n"; then
        NAMED=$((NAMED + 1))
    else
        MISSING="$MISSING $n"
    fi
done
echo "== the wording under test =="
echo "  summary : $SUMMARY_MSG"
echo "  refusals: $REFUSED_MSG"
echo "  named   : $NAMED of ${#NAMES[@]}${MISSING:+ (missing:$MISSING)}"
if [ "$NAMED" != "${#NAMES[@]}" ]; then
    echo "TABLEFOLDER: FAIL named=$NAMED/${#NAMES[@]} — the notice does not name:$MISSING"
    exit 7
fi

# A list that stops early announces itself ("and 3 more"). The old cap did; this
# run must not — the count is eight and eight names are on screen.
if echo "$REFUSED_MSG" | grep -qE "and [0-9]+ more|[0-9]+ more\)"; then
    echo "TABLEFOLDER: FAIL truncated — the refusal notice is a capped list: $REFUSED_MSG"
    exit 8
fi

# The rendered half: one card per notice, and the newest card (the refusals) was
# handed exactly the sentence collected above.
if [ "${CARDS:-0}" -lt 2 ]; then
    echo "TABLEFOLDER: FAIL toast — $CARDS card(s) on screen for the notices raised"
    exit 9
fi
if [ "$LAST_MSG" != "$REFUSED_MSG" ]; then
    echo "TABLEFOLDER: FAIL toast — the card on screen does not carry the refusal wording"
    echo "                   card   : $LAST_MSG"
    echo "                   notice : $REFUSED_MSG"
    exit 10
fi

# The counts beside the names, from the same run.
echo "$SUMMARY_MSG" | grep -q "not sermon files" || {
    echo "TABLEFOLDER: FAIL summary — the outcome line does not count the refused files: $SUMMARY_MSG"
    exit 11
}
echo "$SUMMARY_MSG" | grep -q "1 failed" || {
    echo "TABLEFOLDER: FAIL summary — the renamed spreadsheet is not counted as failed: $SUMMARY_MSG"
    exit 12
}
UP="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -a "Folder import done:" | tail -1)"
case "$UP" in
*"0 imported"*) ;;
*)
    echo "TABLEFOLDER: FAIL summary — the sweep did not report 0 imported: ${UP:-<no line>}"
    exit 13
    ;;
esac

# And the operator's own library is where it was: nothing importable meant nothing
# stored, nothing re-written.
LIB_AFTER=""
if [ -f "$LIB" ]; then
    LIB_AFTER="$(certutil -hashfile "$(cygpath -w "$LIB")" SHA256 2>/dev/null | sed -n 2p)"
    [ -z "$LIB_AFTER" ] && LIB_AFTER="$(md5sum "$LIB" 2>/dev/null | cut -d' ' -f1)"
fi
if [ "$LIB_BEFORE" != "$LIB_AFTER" ]; then
    echo "TABLEFOLDER: FAIL library — the sermon library changed ($LIB_BEFORE -> $LIB_AFTER)"
    exit 14
fi

echo "TABLEFOLDER: PASS unsupported=7 failed=1 named=$NAMED cards=$CARDS matched=yes"
exit 0
