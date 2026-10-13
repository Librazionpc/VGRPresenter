#!/usr/bin/env bash
# ============================================================================
# tools/check_channel_trim.sh — run the closed-channel reopen policy check.
#
# The policy every audio surface shares ("a closed side comes back at the trim
# it was closed with, never at unity by accident") lives in one header,
# qml/models/include/models/ChannelTrim.h, and is pure arithmetic — so this
# compiles that header against tools/channel_trim_check.cpp and runs it. No
# app, no engine, no device, no window: a second, not a scenario.
#
# Usage:  bash tools/check_channel_trim.sh
# Verdict (last line, greppable):  CHANNEL_TRIM: PASS | CHANNEL_TRIM: FAIL
# Overrides: MINGW_BIN, QT_BIN (same convention as tools/dev_cycle.sh).
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/channel_trim_check"

MINGW_BIN="${MINGW_BIN:-/c/msys64/ucrt64/bin}"
QT_BIN="${QT_BIN:-/c/Qt/6.11.1/mingw_64/bin}"
export PATH="$MINGW_BIN:$PATH"

if ! command -v g++ >/dev/null 2>&1; then
    echo "CHANNEL_TRIM: FAIL no g++ on PATH (looked in $MINGW_BIN) — nothing was checked"
    exit 2
fi

# QT_NO_VERSION_TAGGING: the header needs QtGlobal only for `qreal`, and the
# version tag would otherwise demand a link against QtCore for a maths check.
if ! g++ -std=c++17 -Wall -Wextra -DQT_NO_VERSION_TAGGING \
        -I "$ROOT/qml/models/include/models" \
        -I "$QT_BIN/../include" -I "$QT_BIN/../include/QtCore" \
        -o "$OUT.exe" "$HERE/channel_trim_check.cpp"; then
    echo "CHANNEL_TRIM: FAIL build — the policy header did not compile (see above)"
    exit 3
fi

"$OUT.exe"
