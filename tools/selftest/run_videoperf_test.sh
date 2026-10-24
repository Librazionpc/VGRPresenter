#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_videoperf_test.sh — the live video path, measured under CPU
# load: frames delivered and last-frame age, PER OUTPUT.
#
# The report this answers: "when I'm showing HDMI and the laptop is stressed the
# video lags badly". Three costs used to sit in that path, all of them worst on a
# busy machine:
#
#   * the capture callback held the tap's mutex across the whole per-sample job
#     (BGRA->RGBA swizzle, a full JPEG encode) and every reader copied an 8 MB
#     frame under that same mutex — so the device's rate was the rate an 8 MB
#     hand-off could be passed around;
#   * the composition re-copied and re-uploaded every live layer (taken input,
#     media on air, each bus plane) on EVERY build, for EVERY output, even when
#     the frame had not changed;
#   * bus frames were copied out of CompositorState under its mutex on every
#     build as well.
#
# The app's scenario (Main.qml, VGR_VIDEOPERF_TEST=1) puts a real camera/HDMI
# input on air, goes LIVE with it, and samples for ten seconds what the ENGINE
# already publishes per output — the delivered frame count and the age of the
# last presented frame — plus the live loop's throughput and the pump's frame
# revisions. This runner launches it with N CPU hogs running, so the stressed
# machine of the report is the machine being measured.
#
# WHAT IT PRINTS (per run)
#   out "<name>"  frames <first> -> <last>  (<fps> fps)  age mean <ms> ms  max <ms> ms
#   loop <fps> fps  input <fps> fps    (loop = the whole live iteration,
#                                       input = the feed's own frames)
#
# BEFORE / AFTER: the engine carries two measurement hooks (not settings) that
# restore the OLD behaviour for one run, so the same machine, feed, window and
# load can be measured both ways:
#
#   BPS_LAYER_REFRESH_ALWAYS=1   every live layer is refreshed on every build
#                               (the pre-fix copy/upload cost)
#   BPS_TAP_COPY_UNDER_LOCK=1   the PAL's pixel pull copies under the tap's mutex
#                               (the pre-fix capture/consumer hand-off)
#
#   bash tools/selftest/run_videoperf_test.sh              # one run (shipped path)
#   bash tools/selftest/run_videoperf_test.sh --compare    # legacy run, then shipped
#
# VERDICT (last line, greppable):
#   VIDEOPERF: PASS outs=<n> fps=<min> age=<mean>ms label="<source>"
#   VIDEOPERF: FAIL launch | FAIL nosamples | FAIL nofeed | FAIL age
#
# BUDGETS (override by env): VIDEOPERF_MIN_INPUT_FPS (default 8, the feed's own
# rate), VIDEOPERF_MIN_FPS (default 0.15), VIDEOPERF_MAX_AGE_MS (default 3000).
# They are a floor for "the feed is alive and reachable", not a target: the
# numbers printed above the verdict are the point.
#
# WHY THE OUTPUT BUDGETS LOOK LOW: this build runs the SOFTWARE renderer (GPUs
# are opt-in, and the app is that default), so a live iteration here is a
# 1920x1080 CPU raster per output — tens of milliseconds at best, seconds under
# load. The copy work this scenario exists to price is ~10-30 ms of that, so the
# honest reading is the DELTA between the two legs (--compare), not the absolute
# frame rate. Read `input` for the capture path and `age`/`frames` for the
# delivery path.
#
# Usage:  bash tools/selftest/run_videoperf_test.sh [--compare] [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
COMPARE=0
if [ "${1:-}" = "--compare" ]; then
    COMPARE=1
    shift
fi
WAIT_SECS="${1:-180}"
# A quarter of this machine's cores: enough to be "the laptop is stressed", not
# so much that the app cannot run at all (12 on 16 cores starves it into noise).
HOGS="${VIDEOPERF_HOGS:-4}"
MIN_FPS="${VIDEOPERF_MIN_FPS:-0.15}"
MAX_AGE_MS="${VIDEOPERF_MAX_AGE_MS:-3000}"
# The feed's own rate, which is what the capture-path fix is about — it does not
# depend on how fast the render loop can rasterize.
MIN_INPUT_FPS="${VIDEOPERF_MIN_INPUT_FPS:-8}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

if [ ! -f "$BUILD/$APP" ]; then
    echo "VIDEOPERF: FAIL launch — $BUILD/$APP not found (build first)"
    exit 2
fi

stop_app() {
    powershell -NoProfile -Command \
        "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
        >/dev/null 2>&1
    sleep 2
}

# ---- CPU LOAD --------------------------------------------------------------
# One busy process per core, tracked by PID. Only OUR pids are killed — a
# pattern kill would match this script's own command line.
HOG_PIDS=()
start_hogs() {
    [ "$HOGS" -le 0 ] && return
    for _ in $(seq 1 "$HOGS"); do
        ( while :; do :; done ) &
        HOG_PIDS+=("$!")
    done
    sleep 1
}
stop_hogs() {
    for pid in ${HOG_PIDS[@]+"${HOG_PIDS[@]}"}; do
        kill "$pid" 2>/dev/null
    done
    HOG_PIDS=()
    wait 2>/dev/null
}

# ---- ONE RUN ---------------------------------------------------------------
run_once() {
    local tag="$1" log="$2" envs="$3"
    stop_app
    rm -f "$log"
    echo "== $tag: VGR_SELFTEST=1 VGR_VIDEOPERF_TEST=1 ${envs:-<shipped path>} (hogs=$HOGS) =="
    start_hogs
    (
        cd "$BUILD" || exit 1
        # shellcheck disable=SC2086
        env $envs VGR_SELFTEST=1 VGR_VIDEOPERF_TEST=1 \
            PATH="$MINGW_BIN:$QT_BIN:$PATH" \
            "./$APP" > "$log" 2>&1
    ) &
    local run_pid=$!
    local tick=0
    while [ "$tick" -lt "$WAIT_SECS" ]; do
        grep -qa "SELFTEST-VIDEOPERF\] done" "$log" 2>/dev/null && break
        kill -0 "$run_pid" 2>/dev/null || break
        sleep 1
        tick=$((tick + 1))
    done
    sleep 1
    stop_hogs
    stop_app
    wait "$run_pid" 2>/dev/null
}

# ---- PARSE -----------------------------------------------------------------
# Per output: first/last frame counters and mean/max last-frame age. Plus the
# loop's fps and the feed's own fps (inputRev is bumped once per frame the pump
# actually took, so it is the capture rate as the app sees it).
REPORT_AWK='
function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
function num(line, key,    re) {
    re = key "=-?[0-9]+"
    if (!match(line, re)) return -1
    return substr(line, RSTART + length(key) + 1, RLENGTH - length(key) - 1) + 0
}
# The output name is QUOTED and may contain spaces, so it is matched as a whole
# field rather than split on whitespace.
function outName(line,    re) {
    re = "out=.[^\"]*\""
    if (!match(line, re)) return ""
    return substr(line, RSTART + 5, RLENGTH - 6)
}
function stamp(line,    re) {
    re = "t=[0-9]+"
    if (!match(line, re)) return -1
    return substr(line, RSTART + 2, RLENGTH - 2) + 0
}
BEGIN { samples = 0; loopFirst = -1; loopLast = -1; revFirst = -1; revLast = -1; tFirst = -1; tLast = -1 }
/SELFTEST-VIDEOPERF\] t=/ && / out=/ {
    t = stamp($0)
    if (t >= 0) { if (tFirst < 0) tFirst = t; tLast = t }
    name = outName($0)
    if (name == "") next
    frames = num($0, "frames"); if (frames < 0) frames = 0
    age = num($0, "ageMs")
    if (!(name in seen)) { seen[name] = 1; order[++nout] = name; fFirst[name] = frames; ageSum[name] = 0; ageN[name] = 0; ageMax[name] = 0 }
    fLast[name] = frames
    if (age >= 0) { ageSum[name] += age; ageN[name]++; if (age > ageMax[name]) ageMax[name] = age }
    next
}
/SELFTEST-VIDEOPERF\] t=/ && / loop=/ {
    t = stamp($0)
    if (t >= 0) { if (tFirst < 0) tFirst = t; tLast = t }
    v = num($0, "loop"); if (v >= 0) { if (loopFirst < 0) loopFirst = v; loopLast = v }
    v = num($0, "inputRev"); if (v >= 0) { if (revFirst < 0) revFirst = v; revLast = v }
    ++samples
    next
}
END {
    if (samples == 0) { print "NOSAMPLES"; exit }
    secs = (tLast - tFirst) / 1000.0
    if (secs <= 0) secs = 1
    totalAge = 0; totalN = 0; minFps = -1
    for (i = 1; i <= nout; ++i) {
        name = order[i]
        fps = (fLast[name] - fFirst[name]) / secs
        mean = (ageN[name] > 0) ? (ageSum[name] / ageN[name]) : -1
        printf "  out \"%s\"  frames %d -> %d  (%.1f fps)  age mean %.0f ms  max %d ms\n",
               name, fFirst[name], fLast[name], fps, mean, ageMax[name]
        if (mean >= 0) { totalAge += mean; totalN++ }
        if (minFps < 0 || fps < minFps) minFps = fps
    }
    loopFps = (loopLast >= 0) ? (loopLast - loopFirst) / secs : 0
    revFps = (revLast >= 0) ? (revLast - revFirst) / secs : 0
    printf "  loop %.1f fps  input %.1f fps\n", loopFps, revFps
    printf "STATS outs=%d minfps=%.2f meanage=%.0f inputfps=%.1f loopfps=%.1f\n",
           nout, (minFps < 0 ? 0 : minFps), (totalN > 0 ? totalAge / totalN : -1), revFps, loopFps
}
'

# Prints the table, then the one STATS line the gate reads.
parse_run() {
    awk "$REPORT_AWK" "$1"
}

field() { printf '%s\n' "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p" | tail -1; }

# ---- THE RUN(S) ------------------------------------------------------------
FIXED_LOG="$BUILD/videoperf_fixed.log"
LEGACY_LOG="$BUILD/videoperf_legacy.log"
STATS_LEGACY=""

if [ "$COMPARE" -eq 1 ]; then
    run_once "legacy" "$LEGACY_LOG" "BPS_LAYER_REFRESH_ALWAYS=1 BPS_TAP_COPY_UNDER_LOCK=1"
    echo "== legacy: every layer re-copied per build; pixel pull under the tap lock =="
    LEGACY_OUT="$(parse_run "$LEGACY_LOG")"
    printf '%s\n' "$LEGACY_OUT"
    STATS_LEGACY="$(printf '%s\n' "$LEGACY_OUT" | grep '^STATS' | tail -1)"
fi

run_once "shipped" "$FIXED_LOG" ""
echo "== shipped: an unchanged frame is skipped; the pixel hand-off is off the lock =="
FIXED_OUT="$(parse_run "$FIXED_LOG")"
printf '%s\n' "$FIXED_OUT"
STATS_FIXED="$(printf '%s\n' "$FIXED_OUT" | grep '^STATS' | tail -1)"

if [ -z "$STATS_FIXED" ] || [ "$STATS_FIXED" = "NOSAMPLES" ]; then
    if grep -qa "SELFTEST-VIDEOPERF\] take" "$FIXED_LOG" 2>/dev/null; then
        echo "VIDEOPERF: FAIL nosamples — the scenario ran but published no sample (see $FIXED_LOG)"
    else
        echo "VIDEOPERF: FAIL launch — the scenario never reported a take (see $FIXED_LOG)"
    fi
    exit 3
fi

FIXED_FPS="$(field "$STATS_FIXED" minfps)"
FIXED_AGE="$(field "$STATS_FIXED" meanage)"
FIXED_OUTS="$(field "$STATS_FIXED" outs)"
LABEL="$(grep -a "SELFTEST-VIDEOPERF\] take" "$FIXED_LOG" | tail -1 | sed 's/.*label=//; s/ taken=.*//')"

if [ "$COMPARE" -eq 1 ] && [ -n "$STATS_LEGACY" ] && [ "$STATS_LEGACY" != "NOSAMPLES" ]; then
    LEGACY_FPS="$(field "$STATS_LEGACY" minfps)"
    LEGACY_AGE="$(field "$STATS_LEGACY" meanage)"
    echo "== before / after (same machine, same feed, same load, same window) =="
    awk -v lf="$LEGACY_FPS" -v la="$LEGACY_AGE" -v ff="$FIXED_FPS" -v fa="$FIXED_AGE" 'BEGIN {
        printf "  legacy  min %.1f fps   mean age %.0f ms\n", lf, la
        printf "  shipped min %.1f fps   mean age %.0f ms\n", ff, fa
        if (la + 0 > 0 && fa + 0 > 0)
            printf "  age     %.0f%% lower (%.0f ms -> %.0f ms)\n", (la - fa) * 100.0 / la, la, fa
        if (lf + 0 > 0)
            printf "  frames  %.0f%% more per output (%.1f -> %.1f fps)\n", (ff - lf) * 100.0 / lf, lf, ff
    }'
fi

if awk -v fps="$FIXED_FPS" -v age="$FIXED_AGE" -v maxage="$MAX_AGE_MS" 'BEGIN { exit ((age + 0) > (maxage + 0)) ? 1 : 0 }'; then
    :
else
    echo "VIDEOPERF: FAIL age — mean last-frame age ${FIXED_AGE} ms exceeds ${MAX_AGE_MS} ms"
    exit 5
fi
if awk -v fps="$FIXED_FPS" -v minfps="$MIN_FPS" 'BEGIN { exit ((fps + 0) < (minfps + 0)) ? 1 : 0 }'; then
    :
else
    echo "VIDEOPERF: FAIL nofeed — the slowest enabled output delivered ${FIXED_FPS} fps (floor ${MIN_FPS})"
    exit 4
fi
FIXED_INPUT="$(field "$STATS_FIXED" inputfps)"
if awk -v fps="$FIXED_INPUT" -v minfps="$MIN_INPUT_FPS" 'BEGIN { exit ((fps + 0) < (minfps + 0)) ? 1 : 0 }'; then
    :
else
    echo "VIDEOPERF: FAIL nofeed — the capture path delivered ${FIXED_INPUT} fps (floor ${MIN_INPUT_FPS})"
    exit 4
fi

echo "VIDEOPERF: PASS outs=$FIXED_OUTS fps=$FIXED_FPS inputfps=$FIXED_INPUT age=${FIXED_AGE}ms label=$LABEL"
exit 0
