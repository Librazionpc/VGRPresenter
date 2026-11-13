#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_outputs_layout_test.sh — does Settings · Outputs say what
# each row's STYLE does to a slide?
#
# The report it answers: "scripture and The Table work, shows do not". A style
# that restyles the content tabs' designs (while a show keeps its own authored
# layout) and a style that lays shows out as well are two different pictures on
# air, and this screen made them indistinguishable: a row said WHICH style it
# wears and nothing about what that style does. The row now carries that
# sentence, and it is the ENGINE's own answer
# (OutputListModel::styleLayoutNote -> SlideRules::LayoutScope, the same call the
# scene builder composes with), so the line on screen cannot drift from the
# picture on the wire.
#
# What this drives (the `SelfTestOutputsLayout` block in qml/Main.qml, armed by
# VGR_SELFTEST=1 VGR_OUTS_TEST=1): open Settings · Outputs through the app's own
# openSettings, wait for the rows and the style roster, then read EVERY row's
# own `styleLayout` property — the value the line under the Style pill binds to,
# off the delegate item rather than the model — and print one line per row plus
# a verdict.
#
# Judged:
#   * `rows>=1`        — the screen really listed the operator's outputs (an
#                        empty screen would make every check below vacuous);
#   * `reported==rows` — EVERY row printed a runtime line for ITS OWN kind
#                        ("HDMI · …", "NDI · …"). The line used to exist only
#                        for NDI/SDI rows, so an HDMI output said nothing about
#                        its own feed — the report "the notification is only
#                        instantiated in one output";
#   * `styled>=1`      — at least one row wears a style, i.e. the case under
#                        test exists on this machine;
#   * `described==styled` — EVERY styled row printed one of the documented
#                        answers. A styled row with no sentence is the fault
#                        this exists to catch (the role resolving in the model
#                        but never reaching the row reads exactly like that);
#   * `scope=`         — which of the three documented answers the row printed
#                        (Restyles / Lays out shows too / No layout of its own),
#                        so a machine whose styles are all background-only is
#                        visible in the verdict instead of passing silently.
#
# VERDICT (last line, greppable):
#   OUTS: PASS rows=N styled=M described=M scopes=<answered/total> reported=N
#   OUTS: FAIL launch | FAIL notrun | FAIL missing | FAIL empty | FAIL undescribed
#       | FAIL unreported (a row with no runtime line of its own)
#
# Usage:  bash tools/selftest/run_outputs_layout_test.sh [wait-seconds]   (default 90)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/outputs_layout_test.log"
WAIT_SECS="${1:-90}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "OUTS: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_OUTS_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_OUTS_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario opens Settings · Outputs and quits the app itself.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-OUTS\] result" "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break     # the app exited on its own
    fi
    sleep 1
    TICK=$((TICK + 1))
done
wait "$RUN_PID" 2>/dev/null

echo "== scenario =="
grep -a "\[SELFTEST-OUTS\]" "$LOG" 2>/dev/null
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-OUTS\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-OUTS result line — the scenario never finished; see $LOG)"
    echo "OUTS: FAIL notrun"
    exit 3
fi
if echo "$RESULT" | grep -q "missing=yes"; then
    echo "OUTS: FAIL missing — the output roster was empty: $RESULT"
    exit 4
fi

field() { echo "$RESULT" | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2; }
ROWS="$(field rows)"
STYLED="$(field styled)"
DESCRIBED="$(field described)"
REPORTED="$(field reported)"
if [ -z "$ROWS" ] || [ "$ROWS" -lt 1 ]; then
    echo "OUTS: FAIL empty — no output row was listed: $RESULT"
    exit 5
fi
if [ -z "$STYLED" ] || [ "$STYLED" -lt 1 ]; then
    echo "OUTS: FAIL empty — no listed output wears a style, so nothing about a style's scope was exercised: $RESULT"
    exit 6
fi
if [ "$DESCRIBED" != "$STYLED" ]; then
    echo "OUTS: FAIL undescribed — $STYLED styled row(s) but only $DESCRIBED printed one of the documented answers: $RESULT"
    exit 7
fi
# EVERY row, every kind. A missing `reported` means the scenario predates the
# per-row runtime line (or the row property disappeared) — either way nothing
# about "all outputs" was actually checked, so it is a failure, not a skip.
if [ -z "$REPORTED" ] || [ "$REPORTED" != "$ROWS" ]; then
    echo "OUTS: FAIL unreported — $ROWS row(s) listed but only '${REPORTED:-0}' printed a runtime line for their own kind: $RESULT"
    echo "(rows as rendered:)"
    grep -a "\[SELFTEST-OUTS\] row=" "$LOG" 2>/dev/null | sed -n 's/.*output=\"\([^\"]*\)\".*kind=\"\([^\"]*\)\".*status=\"\([^\"]*\)\".*/  \1 (\2) -> \3/p'
    exit 8
fi

# What the rows actually said, so a machine whose styles are all background-only
# (a real possibility) is visible rather than passing as a silent success.
SCOPES="$(grep -a "\[SELFTEST-OUTS\] row=" "$LOG" 2>/dev/null \
    | sed -n 's/.*scope="\([^"]*\)".*/\1/p')"
ANSWERED="$(printf '%s\n' "$SCOPES" | grep -c -v '^none$')"
TOTAL="$(printf '%s\n' "$SCOPES" | grep -c .)"
echo "OUTS: PASS rows=$ROWS styled=$STYLED described=$DESCRIBED scopes=$ANSWERED/$TOTAL reported=$REPORTED"
exit 0
