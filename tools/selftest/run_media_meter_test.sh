#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_media_meter_test.sh — does a media row's audio bar read the
# SIGNAL (the bridge's stream tap) or the row's stored Volume?
#
# The report: "the media audio animation is still stuck even though it shows the
# opposite in edit". The card on Settings · Audio & Video drew the row's STORED
# volume as a level — a fixed fill that tracked the Volume slider and never
# moved — while the Edit dialog for the same row drew the stream tap and read a
# flat -∞ dB (its file was gone). One row, two readings that could not agree.
#
# This drives the real board: it adds a media row pointed at a real audio file
# (Windows' own system sounds — Media Foundation decodes a WAV with no codec
# pack) and one pointed at a file that is not there with the stored level the
# reported row carries (4, which fills ~53% of the bar's own scale — the frozen
# fill in the screenshot), opens Settings · Audio & Video and reads each row's
# level bar across eighteen meter-pump ticks. Also checked, in the engine log:
#   * the live row's file really got a STREAM TAP ("stream meter tap live for")
#     — so the level came off a decode, not off a number the UI made up;
#   * the missing file really got NO tap ("stream meter refused for") — so the
#     silent row's zero is honest silence and not an absent measurement.
#
# VERDICT (last line, greppable):
#   MEDIAMETER: PASS live=<bar> silent=0 stored=4 agree=PASS file="…"
#   MEDIAMETER: FAIL hang — the GUI thread stalled during the run (the state
#               this scenario first uncovered: a media tap parked on its file's
#               end-of-stream while holding the meter table's mutex)
#   MEDIAMETER: FAIL launch | FAIL missing | FAIL live | FAIL silent | FAIL agree
#               | FAIL nolive-T1 (no stream tap was ever started for the file)
#               | FAIL nosilent-T1 (the missing file was tap'd, so the silent
#                                  row proves nothing)
#
# Usage:  bash tools/selftest/run_media_meter_test.sh [audio-file] [wait-seconds]
#         The audio file is optional: the script picks the largest .wav under
#         C:/Windows/Media when none is given.
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/media_meter_test.log"
ELOG="$BUILD/logs/engine.log"
MEDIA_DIR="${MEDIA_DIR:-C:/Windows/Media}"

WANT_FILE=""
WAIT_SECS=""
for arg in "$@"; do
    case "$arg" in
        ''|*[!0-9]*) WANT_FILE="$arg" ;;
        *)           WAIT_SECS="$arg" ;;
    esac
done
WAIT_SECS="${WAIT_SECS:-90}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "MEDIAMETER: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

# A real audio file for the live row. The scenario has its own candidate list, so
# an empty pick here just means the QML list decides.
if [ -z "$WANT_FILE" ]; then
    WANT_FILE="$(ls -S "$MEDIA_DIR"/*.wav 2>/dev/null | head -1)"
fi
if [ -n "$WANT_FILE" ] && [ ! -f "$WANT_FILE" ]; then
    echo "== '$WANT_FILE' is not a file — letting the scenario pick =="
    WANT_FILE=""
fi
echo "== live row's file: ${WANT_FILE:-<scenario default list>} =="

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Log-line DELTA, not the whole file: engine.log is appended across runs, so the
# taps of earlier sessions must not decide this run.
BEFORE=$(wc -l < "$ELOG" 2>/dev/null || echo 0)
rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_MEDIAMETER_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_MEDIAMETER_TEST=1 VGR_MEDIAMETER_FILE="$WANT_FILE" \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

# The scenario adds two rows, samples the meters, reports, removes the rows and
# quits the app itself.
TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-MEDIAMETER\] result" "$LOG" 2>/dev/null; then
        sleep 2   # let the removal's debounced roster write land
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
grep -a "\[SELFTEST-MEDIAMETER\]" "$LOG" 2>/dev/null || echo "(none)"
echo "== stream taps this run (engine log) =="
tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
    | grep -a "stream meter \(tap live\|refused\)" | tr -d '\r' | sed 's/^/  /' || true
echo "== QML errors (should be none) =="
{ grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null
  tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
      | grep -aE "QML|RangeError|Cannot read|TypeError"; } | tr -d '\r' | head -8

# A FROZEN GUI THREAD IS THE FAILURE THIS SCENARIO FIRST FOUND: a media tap
# whose file PLAYS OUT parked its decode thread holding the meter table's
# mutex, so the UI's 50 ms pump blocked on it forever the moment a row's audio
# reached its end. The app's own watchdog is the witness — a stall line in this
# run's log delta fails the test before any of the readings below are trusted.
HANGS="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -ac 'HangWatchdog')"
if [ "${HANGS:-0}" != "0" ]; then
    echo "== GUI stalls this run =="
    tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -a 'HangWatchdog' \
        | tr -d '\r' | sed 's/^/  /'
    echo "MEDIAMETER: FAIL hang — the interface stalled $HANGS time(s); the meter readings cannot be trusted"
    exit 9
fi

RESULT="$(grep -a "\[SELFTEST-MEDIAMETER\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-MEDIAMETER result line — the scenario never finished; see $LOG)"
    echo "MEDIAMETER: FAIL missing"
    exit 3
fi

SAMPLE="$(grep -a "\[SELFTEST-MEDIAMETER\] file=" "$LOG" 2>/dev/null | tail -1)"
FIELD() { echo "$1" | grep -o "$2=[^ ]*" | head -1 | cut -d= -f2; }

LIVE="$(FIELD "$RESULT" live)"
SILENT="$(FIELD "$RESULT" silent)"
AGREE="$(FIELD "$RESULT" agree)"
BARLIVE="$(FIELD "$SAMPLE" barLive)"
STORED="$(FIELD "$SAMPLE" storedSilent)"
FILE="$(echo "$SAMPLE" | sed 's/.*file=\(.*\) samples=.*/\1/')"

# The engine-log half: the file must have been TAPPED (a decode really ran) and
# the missing one must have been REFUSED (no tap, so its silence is real).
TAPS="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
        | grep -a "stream meter tap live" | tr -d '\r')"
REFUSED="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
           | grep -a "stream meter refused" | tr -d '\r')"
if [ -n "$FILE" ] && ! echo "$TAPS" | grep -qF "'$FILE'"; then
    echo "MEDIAMETER: FAIL nolive-T1 — no stream tap was ever started for $FILE (the bar's reading had no source)"
    exit 7
fi
if ! echo "$REFUSED" | grep -q "selftest-meter-missing.mp4"; then
    echo "MEDIAMETER: FAIL nosilent-T1 — the missing file was never refused a tap, so the silent row proves nothing"
    exit 8
fi

if [ "$LIVE" != "PASS" ]; then
    echo "MEDIAMETER: FAIL live — a media row pointed at a real file never moved its bar (barLive=${BARLIVE:-?}, $RESULT)"
    exit 4
fi
if [ "$SILENT" != "PASS" ]; then
    echo "MEDIAMETER: FAIL silent — a row whose file is gone still drew a level (stored=${STORED:-?}, $RESULT)"
    exit 5
fi
if [ "$AGREE" = "FAIL" ]; then
    echo "MEDIAMETER: FAIL agree — the card's bar and the Edit dialog's strips disagree about the same row ($SAMPLE)"
    exit 6
fi
echo "MEDIAMETER: PASS live=${BARLIVE:-?} silent=0 stored=${STORED:-?} agree=${AGREE:-?} file=\"$FILE\" taps=$(echo "$TAPS" | grep -c .)"
exit 0
