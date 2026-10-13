#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_bus_light_test.sh — does the bus status light actually go
# green for a source whose device is there?
#
# The report: "i thought i said we should give the status light access to check
# if [the] device in the source is bringing data … so i expect to see all green"
# — with every device present and every bus routed, all three lights still read
# amber. Two independent ways that happens, and this checks both:
#
#   1. THE ROSTER NEVER CARRIED THE DEVICE. The boot enumeration is TARGETED (it
#      walks only the devices the setup named), so a source whose label never
#      reached that hand-over is invisible to every reader — including the light.
#      The light's ping is what fixes that: it NAMES the source it is asking
#      about, so the sweep walks that device. The engine log proves the ping
#      happened and, on the second look, was no longer needed ("status light for
#      … is not in the device or screen rosters … pinging" is one line per
#      source per session).
#
#   2. THE BINDING HAD NOTHING TO RE-EVALUATE IT. The light reads Q_INVOKABLE
#      model lookups, which QML does not track: without a dependency on the
#      bridge's own devicesChanged, the card keeps whatever answer it got when it
#      was first evaluated — amber for the whole session, however full the
#      rosters are. That is the failure this scenario reads out of the REAL card
#      properties (`busStatus`, the very value its colour is drawn from) after
#      the pane has been open long enough for the sweep to have landed.
#
# AND THE GREEN MEANS DELIVERING: the light's live half (inputDelivery) is read
# off the taps the app already holds, so this run takes two for real — the bus
# card's own monitor gesture for VIDEO, and an audio-only bus's monitor gesture
# for AUDIO — waits PAST the engine's warm-up grace, and reads both.
#
# The two halves are checked differently, on purpose:
#
#   * AUDIO is the POSITIVE case. A meter tap hands over levels — real data,
#     silence included — so a held meter must answer "ok" and its card must read
#     "connected". Without this the run could pass with every light amber.
#   * VIDEO is the CONSISTENCY case. A camera pointed at a wall may honestly
#     deliver nothing, and a tap nothing is watching has no live evidence at all,
#     so the assertion is that the LIGHT AGREES with the tap: "starved" must paint
#     the card amber, "ok"/"" must leave it connected. That is what catches a
#     light that would keep glowing green over a source that stopped delivering.
#
# Both taps are released before the run ends, so nothing is left open and the pane
# is photographed with nothing being pulled.
#
# VERDICT (last line, greppable):
#   BUSLIGHT: PASS green=N/N buses=M pings=K video=ok audio=ok heldBus=B audioBus=A
#   BUSLIGHT: FAIL status — a bus with a routed source did not read "connected"
#   BUSLIGHT: FAIL lightvideo — the card's status disagreed with its tap's delivery
#   BUSLIGHT: FAIL audio — a held meter tap did not read delivering
#   BUSLIGHT: FAIL nodelivery — the run never held a video tap (live half untested)
#   BUSLIGHT: FAIL noaudio — no audio-only bus with a routed input, so no positive case
#   BUSLIGHT: FAIL norouted — no bus had a routed source, so the run proves nothing
#   BUSLIGHT: FAIL missing — the scenario never finished (see the log)
#   BUSLIGHT: FAIL launch — build/appVGRPresenterUI.exe not found
#
# Usage:  bash tools/selftest/run_bus_light_test.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/bus_light_test.log"
ELOG="$BUILD/logs/engine.log"

WAIT_SECS="${1:-90}"
case "$WAIT_SECS" in
    ''|*[!0-9]*) WAIT_SECS=90 ;;
esac

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "BUSLIGHT: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# Log-line DELTA, not the whole file: engine.log is appended across runs, so the
# pings of earlier sessions must not decide this run.
BEFORE=$(wc -l < "$ELOG" 2>/dev/null || echo 0)
rm -f "$LOG"
echo "== launching with VGR_SELFTEST=1 VGR_BUSLIGHT_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_BUSLIGHT_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-BUSLIGHT\] result" "$LOG" 2>/dev/null; then
        sleep 2   # let the quit path finish and the log flush
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
grep -a "\[SELFTEST-BUSLIGHT\]" "$LOG" 2>/dev/null || echo "(none)"
echo "== status-light pings this run (engine log) =="
tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null \
    | grep -a "status light for" | tr -d '\r' | sed 's/^/  /' || true
# The SAME pattern tools/dev_cycle.sh's health check uses: a WARN/ERROR/FATAL
# line tagged [QML], or a JS throw. Matching a bare "QML" instead counted the
# StartupTrace INFO lines ("QML load begin: …"), which are progress, not errors.
QML_PATTERN='\[(WARN|ERROR|FATAL)\].*\[QML\]|RangeError|Cannot read|TypeError'
echo "== QML errors (should be none) =="
{ grep -aE "$QML_PATTERN" "$LOG" 2>/dev/null
  tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -aE "$QML_PATTERN"; } \
    | tr -d '\r' | head -8

QERR="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -acE "$QML_PATTERN")"
if [ "${QERR:-0}" != "0" ]; then
    echo "BUSLIGHT: FAIL qml — $QERR QML error line(s) since launch"
    exit 5
fi

RESULT="$(grep -a "\[SELFTEST-BUSLIGHT\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-BUSLIGHT result line — the scenario never finished; see $LOG)"
    echo "BUSLIGHT: FAIL missing"
    exit 3
fi

FIELD() { echo "$1" | grep -o "$2=[^ ]*" | head -1 | cut -d= -f2; }
ROUTED="$(FIELD "$RESULT" routed)"
GREEN="$(FIELD "$RESULT" green)"
VERDICT="$(FIELD "$RESULT" busStatus)"
BUSES="$(FIELD "$RESULT" buses)"
ERR="$(FIELD "$RESULT" err)"
PINGS="$(tail -n +"$((BEFORE + 1))" "$ELOG" 2>/dev/null | grep -ac "status light for")"
# THE PING MUST HAVE FIRED: the scenario asks about a label no device has, so a
# light that only ever reads a cache (never refreshes it) is caught here — that
# is the user's own ask ("give them access to ping") rather than an internal
# detail, and it is the half a roster that happens to answer everything hides.
if [ "${PINGS:-0}" = "0" ]; then
    echo "BUSLIGHT: FAIL noping — a source whose device is in no roster never reached the status light's ping path (no 'status light for' line in the engine log)"
    exit 7
fi
if ! grep -qa "status light for 'selftest-buslight-ghost-window'" \
        "$ELOG" 2>/dev/null; then
    echo "BUSLIGHT: FAIL pinglabel — the ping fired, but not for the label the scenario asked about"
    exit 8
fi

DELIVERY="$(FIELD "$RESULT" delivery)"
HELDBUS="$(FIELD "$RESULT" heldBus)"
HELDSTATUS="$(FIELD "$RESULT" heldStatus)"
AUDIO="$(FIELD "$RESULT" audio)"
AUDIOBUS="$(FIELD "$RESULT" audioBus)"
AUDIOSTATUS="$(FIELD "$RESULT" audioStatus)"
# THE LIVE HALF, VIDEO SIDE: the run must have HELD a real tap (its own card
# monitor gesture) and read what it delivers. The tap is read after the engine's
# warm-up grace, and the light must AGREE with it — "starved" amber, "ok"/""
# connected — because the point is the card's own answer, not the bridge's.
if [ "${HELDBUS:--1}" -lt 0 ]; then
    echo "BUSLIGHT: FAIL nodelivery — the run never held a tap, so the light's live half was not exercised"
    exit 12
fi
WANTSTATUS="connected"
[ "$DELIVERY" = "starved" ] && WANTSTATUS="starved"
if [ "$HELDSTATUS" != "$WANTSTATUS" ]; then
    echo "BUSLIGHT: FAIL lightvideo — the held tap's delivery was '$DELIVERY' but the card read '$HELDSTATUS' (wanted '$WANTSTATUS'): the light is not consuming its own live half"
    exit 13
fi
# THE POSITIVE CASE, AUDIO SIDE. A meter tap hands LEVELS over — silence is data —
# so a held meter is the one tap that can be relied on to answer "ok", and its
# card must read connected off it. "n/a" (no audio-only bus in the setup) fails
# too: then nothing in this run proves green is ever earned.
if [ "${AUDIOBUS:--1}" -lt 0 ]; then
    echo "BUSLIGHT: FAIL noaudio — the run found no audio-only bus with a routed input, so it had no delivering tap to prove green against"
    exit 14
fi
if [ "$AUDIO" != "ok" ]; then
    echo "BUSLIGHT: FAIL audio — a held meter tap read '${AUDIO:-<none>}' instead of delivering (see the delivery audio line above)"
    exit 15
fi
if [ "$AUDIOSTATUS" != "connected" ]; then
    echo "BUSLIGHT: FAIL audio — the held meter tap delivered but its card read '$AUDIOSTATUS' (wanted 'connected')"
    exit 16
fi

GHOST="$(grep -a "\[SELFTEST-BUSLIGHT\] ping label=" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$GHOST" ]; then
    echo "BUSLIGHT: FAIL noping — the scenario never asked about an unknown source (see $LOG)"
    exit 9
fi
if echo "$GHOST" | grep -q "alive=true"; then
    echo "BUSLIGHT: FAIL ghostalive — a source no device backs was reported alive ($GHOST)"
    exit 10
fi
WRONGKIND="$(grep -a "\[SELFTEST-BUSLIGHT\] wrongkind " "$LOG" 2>/dev/null | tail -1)"
if echo "$WRONGKIND" | grep -q "alive=false"; then
    echo "BUSLIGHT: FAIL wrongkind — a source that IS in a roster read not-available when asked with another kind ($WRONGKIND)"
    exit 11
fi

if [ "${ROUTED:-0}" = "0" ]; then
    echo "BUSLIGHT: FAIL norouted — no bus had a routed source, so the light proved nothing"
    exit 4
fi
if [ "$VERDICT" != "PASS" ]; then
    echo "BUSLIGHT: FAIL status — routed=$ROUTED green=$GREEN err=$ERR (see the per-bus lines above)"
    exit 6
fi
echo "BUSLIGHT: PASS green=$GREEN/$ROUTED buses=${BUSES:-?} pings=$PINGS video=$DELIVERY audio=$AUDIO heldBus=$HELDBUS audioBus=$AUDIOBUS"
exit 0
