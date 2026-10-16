#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_mark_sidecar_test.sh — what a note or highlight COSTS.
#
# The claim this exists to check is a wall-clock one, made in the running app:
# marking a verse writes a small sidecar file (bibles.userdata.json) and never
# touches the corpus (bibles.json), where the old path read the corpus, parsed
# it and wrote it back — megabytes, under the engine lock every scripture read
# takes — on the INTERFACE thread, because setNote/setHighlighted are
# Q_INVOKABLE on a QML singleton.
#
# The engine tests cover the same claim on a synthetic corpus (bible-mark-sidecar:
# corpus byte-identical; bible-mark-timing: the two wall-clock figures side by
# side). What they cannot cover is the OPERATOR'S own store, through the pane's
# own handlers — so this drives the real app on the real store:
#
#   * the script snapshots the store before the launch (both files);
#   * VGR_MARK_TEST=1 opens the first Bible's first chapter and, on a verse that
#     carries no mark at all, toggles the highlight on and off again and adds and
#     removes a note, timing each call as the interface thread feels it — and
#     then UNDOES everything, so the store it leaves behind is the store it found;
#   * the script then compares both files byte for byte, and counts the engine's
#     own lines: `user-data sidecar written in N ms` must go UP (with kilobytes,
#     not megabytes) while `store written (corpus)` must stay at ZERO.
#
# VERDICT (last line, greppable):
#   MARK: PASS verse=<n> onMs=<n> offMs=<n> addMs=<n> rmMs=<n> sidecarWrites=<k> \
#         sidecarMs=<min>-<max> sidecarBytes=<b> corpusWrites=0 corpusUnchanged=true \
#         sidecarUnchanged=true
#   MARK: FAIL launch | FAIL missing | FAIL nobible | FAIL nomark | FAIL corpuswrite
#         | FAIL corpuschanged | FAIL sidecarchanged | FAIL slow:<field> | FAIL restore
#
# Usage:  bash tools/selftest/run_mark_sidecar_test.sh [wait-seconds]  (default 150)
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/mark_sidecar_test.log"
# The engine's own record is NOT on stdout: the app writes logs/engine.log
# relative to its working directory (see tools/dev_cycle.sh), which is why the
# script reads that file and only the lines written AFTER the launch — a mark
# run must not be credited with a write an earlier run made.
ENGINE_LOG="$BUILD/logs/engine.log"
DELTA="$BUILD/mark_sidecar_engine_delta.log"
WAIT_SECS="${1:-150}"

# The store the app writes (QStandardPaths AppDataLocation) and the two files
# this scenario is about: the corpus and the user data's own sidecar.
STORE="${APPDATA:-$HOME/AppData/Roaming}/VGR/VGRPresenter/enginedata"
CORPUS="$STORE/bibles.json"
SIDECAR="$STORE/bibles.userdata.json"
SNAP="$BUILD/mark_sidecar_snapshot"
# A mark that costs the interface thread more than this is not the cheap path
# whatever the file sizes say.
MAX_UI_MS=250

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "MARK: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi
if [ ! -f "$CORPUS" ]; then
    echo "MARK: FAIL nobible — no store at $CORPUS (import a Bible first)"
    exit 2
fi

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

# ---- the store as it is BEFORE any of this ---------------------------------
rm -rf "$SNAP"
mkdir -p "$SNAP"
cp "$CORPUS" "$SNAP/bibles.json"
CORPUS_BYTES="$(stat -c %s "$CORPUS")"
SIDECAR_BYTES=0
if [ -f "$SIDECAR" ]; then
    cp "$SIDECAR" "$SNAP/bibles.userdata.json"
    SIDECAR_BYTES="$(stat -c %s "$SIDECAR")"
fi
echo "== store: corpus ${CORPUS_BYTES} bytes, sidecar ${SIDECAR_BYTES} bytes -> $SNAP =="

rm -f "$LOG"
rm -f "$DELTA"
ENGINE_LINES_BEFORE=0
if [ -f "$ENGINE_LOG" ]; then
    ENGINE_LINES_BEFORE="$(wc -l < "$ENGINE_LOG" | tr -d ' ')"
fi
echo "== launching with VGR_SELFTEST=1 VGR_MARK_TEST=1 (output -> $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_MARK_TEST=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$LOG" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if grep -qa "\[SELFTEST-MARK\] result" "$LOG" 2>/dev/null; then
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
sleep 1

# Only what THIS run wrote (engine.log is appended to across runs).
if [ -f "$ENGINE_LOG" ]; then
    tail -n "+$((ENGINE_LINES_BEFORE + 1))" "$ENGINE_LOG" > "$DELTA" 2>/dev/null || : > "$DELTA"
else
    : > "$DELTA"
fi

echo "== scenario =="
grep -a "\[SELFTEST-MARK\]" "$LOG" 2>/dev/null
echo "== the engine's own record of this run's writes =="
grep -a "BibleEngine: user-data sidecar written\|BibleEngine: store written (corpus)" "$DELTA" 2>/dev/null | tail -12
echo "== QML errors (should be none) =="
grep -aE "QML|RangeError|Cannot read|TypeError" "$LOG" 2>/dev/null | head -8
# In the engine log the same words appear in healthy startup lines ("QML load
# begin"), so only real QML diagnostics are counted there: a file:line.
grep -aE "\.qml:[0-9]+:|RangeError|Cannot read|TypeError" "$DELTA" 2>/dev/null | head -8

RESULT="$(grep -a "\[SELFTEST-MARK\] result" "$LOG" 2>/dev/null | tail -1)"
if [ -z "$RESULT" ]; then
    echo "(no SELFTEST-MARK result line — the scenario never finished; see $LOG)"
    echo "MARK: FAIL missing"
    exit 3
fi

num() { echo "$RESULT" | grep -o "$1=[0-9-]*" | head -1 | cut -d= -f2; }
VERSE="$(num verse)"
ON="$(num highlightOnMs)"
OFF="$(num highlightOffMs)"
ADD="$(num noteAddMs)"
RM="$(num noteRemoveMs)"
RESTORED="$(echo "$RESULT" | grep -o "restored=[a-z]*" | head -1 | cut -d= -f2)"
DETAIL="$(echo "$RESULT" | sed -n "s/.*detail='\([^']*\)'.*/\1/p")"

if [ "$RESTORED" != "true" ]; then
    echo "MARK: FAIL restore — the scenario did not undo its own marks (${DETAIL:-no detail})"
    exit 4
fi
if [ -z "$VERSE" ] || [ "$VERSE" = "0" ]; then
    echo "MARK: FAIL nomark — no verse was marked (${DETAIL:-no detail})"
    exit 5
fi

# ---- the engine's own lines: sidecar writes up, corpus writes at zero -------
SIDECAR_LINES="$(grep -a "BibleEngine: user-data sidecar written" "$DELTA" 2>/dev/null)"
CORPUS_WRITES="$(grep -ac "BibleEngine: store written (corpus)" "$DELTA" 2>/dev/null)"
SIDECAR_WRITES="$(echo "$SIDECAR_LINES" | grep -c "user-data sidecar written")"
if [ "$CORPUS_WRITES" != "0" ]; then
    echo "MARK: FAIL corpuswrite — $CORPUS_WRITES corpus write(s) during a mark run:"
    grep -a "BibleEngine: store written (corpus)" "$DELTA" | head -3
    exit 6
fi
# The four marks of the scenario (highlight on, off, note add, remove) each write
# the sidecar; two is the floor that still proves the path ran.
if [ -z "$SIDECAR_WRITES" ] || [ "$SIDECAR_WRITES" -lt 2 ]; then
    echo "MARK: FAIL missing — the engine logged $SIDECAR_WRITES sidecar write(s), want >= 2"
    exit 7
fi
# The sidecar's own numbers, straight out of the engine's lines: milliseconds
# (the interface thread's cost, mostly) and kilobytes (never the corpus).
SIDE_MS="$(echo "$SIDECAR_LINES" | sed -n 's/.*written in \([0-9]*\) ms.*/\1/p')"
SIDE_BYTES="$(echo "$SIDECAR_LINES" | sed -n 's/.*(\([0-9]*\) bytes.*/\1/p')"
SIDE_MS_MIN="$(echo "$SIDE_MS" | sort -n | head -1)"
SIDE_MS_MAX="$(echo "$SIDE_MS" | sort -n | tail -1)"
SIDE_BYTES_MAX="$(echo "$SIDE_BYTES" | sort -n | tail -1)"
if [ -z "$SIDE_MS_MAX" ] || [ "$SIDE_MS_MAX" -gt "$MAX_UI_MS" ]; then
    echo "MARK: FAIL slow:sidecar — a sidecar write took ${SIDE_MS_MAX:-?} ms (want <= $MAX_UI_MS)"
    exit 8
fi
# Kilobytes, not megabytes: a sidecar that ever grows to the corpus's size means
# the split has been undone.
if [ -z "$SIDE_BYTES_MAX" ] || [ "$SIDE_BYTES_MAX" -gt 262144 ]; then
    echo "MARK: FAIL slow:sidecarbytes — the sidecar write was ${SIDE_BYTES_MAX:-?} bytes (want < 256 KB)"
    exit 9
fi
for FIELD in "$ON" "$OFF" "$ADD" "$RM"; do
    if [ -z "$FIELD" ] || [ "$FIELD" -gt "$MAX_UI_MS" ]; then
        echo "MARK: FAIL slow:ui — a mark took ${FIELD:-?} ms on the interface thread (want <= $MAX_UI_MS)"
        exit 10
    fi
done

# ---- and the store itself: byte for byte what it was ------------------------
if ! cmp -s "$CORPUS" "$SNAP/bibles.json"; then
    echo "MARK: FAIL corpuschanged — the corpus changed during a run that only marked verses"
    echo "      (before $(stat -c %s "$SNAP/bibles.json") bytes, after $(stat -c %s "$CORPUS") bytes)"
    exit 11
fi
if [ -f "$SNAP/bibles.userdata.json" ]; then
    if ! cmp -s "$SIDECAR" "$SNAP/bibles.userdata.json"; then
        echo "MARK: FAIL sidecarchanged — the sidecar's CONTENT differs from before the run"
        echo "      (the scenario marks then undoes: the same marks must leave the same bytes)"
        exit 12
    fi
else
    # No sidecar before the run. The marks CREATED it — that is the schema-3
    # switch (the corpus carries the user-data arrays only until the sidecar's
    # first successful write) — so it is expected afterwards; what must not be
    # there is a MARK, since the scenario undid everything it did.
    if [ ! -f "$SIDECAR" ]; then
        echo "MARK: FAIL sidecarchanged — the marks wrote no sidecar at all"
        exit 12
    fi
    if grep -q '"highlights":\[{\|\"notes\":\[{"' "$SIDECAR"; then
        echo "MARK: FAIL sidecarchanged — the sidecar still carries a mark after the undo:"
        head -c 200 "$SIDECAR"; echo
        exit 12
    fi
    if [ "$(stat -c %s "$SIDECAR")" -gt 1024 ]; then
        echo "MARK: FAIL sidecarchanged — the sidecar is $(stat -c %s "$SIDECAR") bytes with no marks in it"
        exit 12
    fi
fi

echo "MARK: PASS verse=$VERSE onMs=$ON offMs=$OFF addMs=$ADD rmMs=$RM sidecarWrites=$SIDECAR_WRITES sidecarMs=$SIDE_MS_MIN-$SIDE_MS_MAX sidecarBytes=$SIDE_BYTES_MAX corpusWrites=0 corpusUnchanged=true sidecarUnchanged=true"
exit 0
