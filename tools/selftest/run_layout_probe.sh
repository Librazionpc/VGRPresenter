#!/usr/bin/env bash
# ============================================================================
# tools/selftest/run_layout_probe.sh — the layout probe (a measurement, not a gate).
#
# It drives, one step per second, the two interactions the hang captures name — a
# filter applied to a library list, and a pane switch — and prints for each step:
#
#   stallMax / stallSum   how long the INTERFACE thread was parked, from the thread's
#                         own 16 ms heartbeat (a timer that asks for 16 ms and reports
#                         the overshoot). This is the freeze itself, not the work that
#                         caused it, and a DirectWrite/relayout pile-up cannot hide
#                         from it.
#   built / dropped       the delegates every list built and THREW AWAY during the step
#                         (shows / media / overlay / template / verse rows). builds ==
#                         drops is a list rebuilding itself; builds > 0 with drops == 0 is
#                         a list growing a new tail.
#   rows                  what each model holds after the step, so a rebuild of a
#                         6-card library cannot be read as one of a 680-row one.
#
# A SETTLE is only half the reported lag. The SCROLL PHASE (the `scroll-*` steps) measures
# the other half: the cost paid per FRAME while rows enter and leave the viewport. Each of
# those steps scrolls ONE named list through its whole content (a linear 700 ms animation —
# a fast wheel drag, no flick inertia, so two runs compare) and reports, for that window:
#
#   frames / fps / avgFrameMs / worstFrameMs / jank33 / jank50
#                       frame times from a FrameAnimation on the interface thread (the first
#                       frame is skipped — its frameTime is the idle gap, not a scroll cost).
#                       The C++ FrameProbe lines beside these are the same frames from the
#                       render thread.
#   Built / Dropped     the scrolled list's delegates over the scroll window (reported in the
#                       list's own field: refVerse*, refBook*, refChapter*, refSource*,
#                       template*/overlay* for the design grids, shows* for the shows table).
#   Evals               the row BINDING evaluations the same window cost — the reference rows'
#                       own label/text bindings (refVerseEvals / refBookEvals / refChapterEvals
#                       / refSourceEvals) and the design cards' fit solvers (cardFitSolves: 11
#                       bisections over a hidden twin Text per text block of every card built).
#                       Zero with a slow scroll is a painting/layout cost; scaling with the
#                       distance is binding and shaping work.
#   contentPx / viewPx / scrollPx / scrollMs / listFound
#                       how far it actually scrolled: a short list, an unlaid-out one and a
#                       missing objectName are each visibly different from a cheap scroll.
#
# The same log carries the C++ FrameProbe lines (frame-to-frame time painted by the
# render thread, 1/s, with >33/>50/>100 ms counts), so the freeze and the frames can
# be read together.
#
# ONE INVARIANT, not just numbers: each design grid re-checks its own row model against
# the service's list after every sync and logs `[DesignRows] <noun> rows=N want=M match=1`
# (or `MISMATCH row=i id=… want=…`). A cheap filter that showed the WRONG cards would
# score exactly like a correct one on the churn columns, so the summary prints the
# distinct lines and the mismatch count beside them. The walk it drives: a cold switch through every library tab; the
# SAME-WIDTH pane pairs against the WIDTH-CROSSING ones (a reference tab used to take
# the template column's whole width); a filter on the shows table and on both design
# grids; the passage pane's own filter and citation search; the scroll phase — the shows
# table, The Table's passage list, Scripture's passages / bible list / chapter list, and
# both design grids; and the pane walk again, warm.
#
# Usage:  bash tools/selftest/run_layout_probe.sh [wait-seconds]
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
APP="appVGRPresenterUI.exe"
LOG="$BUILD/logs/engine.log"
WAIT_SECS="${1:-180}"

if [ -d /c/msys64/ucrt64/bin ]; then
    MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
else
    MINGW_BIN="${MINGW_BIN:-C:/msys64/ucrt64/bin}"
    QT_BIN="${QT_BIN:-C:/Qt/6.11.1/mingw_64/bin}"
fi

. "$HERE/../agent_env.sh"

if [ ! -f "$BUILD/$APP" ]; then
    echo "LAYOUT: no app — build first"
    exit 2
fi

# A delta, not the whole file: engine.log is appended across runs.
MARK="$(grep -ac . "$LOG" 2>/dev/null || echo 0)"

echo "== stopping any running instance =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
sleep 2

echo "== launching with VGR_LAYOUT_TEST=1 VGR_FRAMEPROBE=1 (log: $LOG) =="
(
    cd "$BUILD" || exit 1
    VGR_SELFTEST=1 VGR_LAYOUT_TEST=1 VGR_FRAMEPROBE=1 \
        PATH="$MINGW_BIN:$QT_BIN:$PATH" \
        "./$APP" > "$BUILD/layout_probe_app.log" 2>&1
) &
RUN_PID=$!

TICK=0
while [ "$TICK" -lt "$WAIT_SECS" ]; do
    if tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -qa "LayoutProbe.*phase=done"; then
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    TICK=$((TICK + 1))
done
sleep 1

echo "== the walk (per step: wall time and delegates built) =="
tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "LayoutProbe" | sed 's/.*\[LayoutProbe\]/LayoutProbe/'

echo "== the design grids' row model, checked against the service's own list =="
tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "\[DesignRows\]" \
    | sed 's/.*\[DesignRows\]/DesignRows/' | sort | uniq -c | sed 's/^ *\([0-9]*\) /  \1 x /'
MISMATCHES="$(tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -ac "DesignRows.*MISMATCH")"
if [ "$MISMATCHES" = "0" ]; then
    echo "   every sync ended with the rows equal to the service's list: 0 mismatches"
else
    echo "   $MISMATCHES MISMATCH line(s) — the grid was fed rows that do not match the service's answer:"
    tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "DesignRows.*MISMATCH" | sed 's/.*\[DesignRows\]/   DesignRows/'
fi

echo "== the scroll cost (per list scrolled: frames, delegates, binding evaluations) =="
SCROLL="$(tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "LayoutProbe.*phase=scroll-" | sed 's/.*\[LayoutProbe\]/LayoutProbe/')"
if [ -n "$SCROLL" ]; then
    printf '%s\n' "$SCROLL"
    echo "== one line per scrolled list =="
    printf '%s\n' "$SCROLL" | awk '
        {
            tag = list = fps = worst = jank = jank50 = px = frames = rows = ""
            built = dropped = evals = cpx = vpx = found = ms = wall = items = ""
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "phase") tag = kv[2]
                else if (kv[1] == "scrollList") list = kv[2]
                else if (kv[1] == "listFound") found = kv[2]
                else if (kv[1] == "fps") fps = kv[2]
                else if (kv[1] == "worstFrameMs") worst = kv[2]
                else if (kv[1] == "jank33") jank = kv[2]
                else if (kv[1] == "jank50") jank50 = kv[2]
                else if (kv[1] == "scrollMs") ms = kv[2]
                else if (kv[1] == "scrollWallMs") wall = kv[2]
                else if (kv[1] == "scrollPx") px = kv[2]
                else if (kv[1] == "contentPx") cpx = kv[2]
                else if (kv[1] == "viewPx") vpx = kv[2]
                else if (kv[1] == "frames") frames = kv[2]
                else if (kv[1] == "scrollRows") rows = kv[2]
                else if (kv[1] == "scrollItems") items = kv[2]
                else if (kv[1] == "scrollBuilt") built = kv[2]
                else if (kv[1] == "scrollDropped") dropped = kv[2]
                else if (kv[1] == "scrollEvals") evals = kv[2]
            }
            printf "  %-26s %-31s fps=%-6s worstFrameMs=%-7s jank33=%-3s jank50=%-3s wallMs=%-6s meterMs=%-6s scrollPx=%-7s items=%-5s rows=%-7s built=%-7s dropped=%-7s evals=%-7s (content %s / view %s, found %s)\n",
                   tag, list, fps, worst, jank, jank50, wall, ms, px, items, rows, built, dropped, evals, cpx, vpx, found
        }'
    echo "   worst frame in the whole scroll phase:"
    printf '%s\n' "$SCROLL" | sed -n 's/.*worstFrameMs=\([0-9.]*\).*/\1/p' | sort -rn | head -1 | sed 's/^/     worstFrameMs=/'
    echo "   scrolls that could not move (content fits the view, or a missing list):"
    printf '%s\n' "$SCROLL" | grep -a "scrollPx=0\|listFound=0" | sed 's/.*phase=\(scroll-[a-z-]*\) .*/     \1/'
else
    echo "   (no scroll steps in this run — the phase is only in VGR_LAYOUT_TEST)"
fi

echo "== frame-to-frame time while it ran (1 s windows) =="
tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "FrameProbe" | sed 's/.*\[FrameProbe\]/FrameProbe/'

echo "== worst gaps inside the run (frames > 50 ms) =="
tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null | grep -a "FrameProbe" \
    | sed -n 's/.*>50ms=\([0-9]*\).*/\1/p' | sort -rn | head -3 | tr '\n' ' '
echo

echo "== stopping the app =="
powershell -NoProfile -Command \
    "Get-Process appVGRPresenterUI -ErrorAction SilentlyContinue | Stop-Process -Force" \
    >/dev/null 2>&1
wait "$RUN_PID" 2>/dev/null
echo "LAYOUT: done (this is a measurement, not a verdict)"
