#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_scripture_test.sh — does the Scripture tab's columns fill?
#
# The report this exists for (user, with the tab's screenshot): the Bibles column
# lists the installed Bible and the books / chapters / verses columns stay EMPTY,
# the preview saying "Pick a verse" — with nothing in the log, because an empty
# column is the same pixels whether the outline read is still in flight, was
# declined, or came back empty.
#
# This drives the REAL app: shows the Scripture tab exactly as a click does
# (showScreen.showInLibrary), then prints what the pane holds and what
# ScriptureService answers for the SAME Bible id — twice, the second time after
# the pane re-asks the way a tab switch makes it (syncSources). The pair is what
# separates the two halves of the fault:
#
#   * books on the second sample but not the first = a notify that never reached
#     the pane (the read landed; nothing told the UI);
#   * none on either                              = the read itself (declined,
#     still in flight, or answered empty).
#
# The service's own `ScriptureCorpus` log lines (one per COLD read) say which of
# those it was, and they are printed here beside the samples.
#
# VERDICT (last line, greppable):
#   SCRIPTURE: PASS sources=<n> bookRows=<n> verses=<n> serviceBooks=<n>
#   SCRIPTURE: FAIL launch | FAIL missing | FAIL empty | FAIL noread
#
# Usage:  bash tools/selftest/run_scripture_test.sh [wait-seconds]  (default 70)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/scripture_test.log"
WAIT_SECS="${1:-70}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "SCRIPTURE: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
# The app's own log (build/logs/engine.log) is where the service's `ScriptureCorpus`
# lines and the pane's `OpenTrace` lines land — stdout only carries console.log. So
# mark where it ends now and read back only this run's lines.
ENGINE_LOG="$BUILD/logs/engine.log"
ENGINE_MARK=0
[ -f "$ENGINE_LOG" ] && ENGINE_MARK="$(stat -c %s "$ENGINE_LOG")"

launched_log() {   # everything the app logged since this harness launched it
    [ -f "$ENGINE_LOG" ] || return 0
    tail -c "+$((ENGINE_MARK + 1))" "$ENGINE_LOG" 2>/dev/null | tr -d '\000'
}

echo "== launching with VGR_SELFTEST=1 VGR_SCRIPTURE_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_SCRIPTURE_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-SCRIPTURE\] re-ask" "$LOG" 2>/dev/null; then
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

echo "== the pane and the service, side by side =="
grep -a "\[SELFTEST-SCRIPTURE\]" "$LOG" 2>/dev/null
echo "== the service's own cold reads (ScriptureCorpus) =="
launched_log | grep -a "ScriptureCorpus" | tail -12
echo "== the pane's own open trace =="
launched_log | grep -a "OpenTrace" | tail -6
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

FIELD() {   # FIELD <tag> <name> — one number out of one sample line
    echo "$RESULT" | grep -a "\[SELFTEST-SCRIPTURE\] $1 " |
        grep -oE "$2='?[^' ]*'?" | head -1 | sed -E "s/$2='?//;s/'?$//"
}

RESULT="$(grep -a "\[SELFTEST-SCRIPTURE\] re-ask" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-SCRIPTURE re-ask line — the scenario never finished; see $LOG)"
    echo "SCRIPTURE: FAIL missing"
    exit 3
fi

SOURCES="$(FIELD re-ask sources)"
BOOK_ROWS="$(FIELD re-ask visibleBooks)"
VERSES="$(FIELD re-ask chapterVerses)"
SERVICE_BOOKS="$(FIELD re-ask serviceBooks)"

if [ -z "$SOURCES" ] || [ "$SOURCES" = "0" ]; then
    echo "SCRIPTURE: FAIL missing — the pane lists no Bible at all (sources=${SOURCES:-?}; see $LOG)"
    exit 4
fi
if [ -z "$BOOK_ROWS" ] || [ "$BOOK_ROWS" = "0" ]; then
    echo "SCRIPTURE: FAIL empty — the Bibles column has $SOURCES Bible(s) and the books column is empty"
    echo "  (the service answers ${SERVICE_BOOKS:-?} book(s) for the same id; ScriptureCorpus lines above say why)"
    exit 5
fi
if [ -z "$VERSES" ] || [ "$VERSES" = "0" ]; then
    echo "SCRIPTURE: FAIL noread — the books column has $BOOK_ROWS row(s) but no chapter was ever read"
    exit 6
fi

echo "SCRIPTURE: PASS sources=$SOURCES bookRows=$BOOK_ROWS verses=$VERSES serviceBooks=$SERVICE_BOOKS"
exit 0
