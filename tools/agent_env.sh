# tools/agent_env.sh — sourced by the coding agent's shell, never by the app.
#
# WHY THIS EXISTS: the agent shell (busybox ash, spawned by the app) starts
# with a minimal PATH containing only the app's own bin dirs — no System32,
# no MSYS2, no Qt, no Git. Everything the project needs IS installed on this
# machine, it just isn't reachable. This file puts it on PATH.
#
# Usage:   . tools/agent_env.sh
#
# The app's own dev_cycle.sh does NOT need this (it hardcodes the same paths
# via CMAKE_BIN/MINGW_BIN/QT_BIN) — but a bare `cmake --build build`, `git`,
# `tasklist` or `g++` in an agent shell does. Keep the two in sync if a path
# ever moves.
#
# Note: busybox ash does not glob drive letters, and /c/... does NOT resolve
# here (the shell is not MSYS) — always use the native C:/... form.
#
# ORDER IS LOAD-BEARING — do not reshuffle these blocks. Qt's mingw_64/bin
# ships its OWN libwinpthread-1.dll (Qt 6.11.1's, built May 2023) which does
# NOT export what this toolchain's cc1plus.exe needs; if it wins the search,
# EVERY compile dies with
#     cc1plus.exe: The specified procedure could not be found. Error 0xc0000139
# and g++ exits 1 with NO diagnostic — which ninja then reports as a bare
# "FAILED: <obj>" with no error text. MSYS2's ucrt64/bin must come FIRST.
# (Qt's libgcc_s_seh-1.dll / libstdc++-6.dll are harmless in front; only
# libwinpthread-1.dll breaks cc1plus. Verified by swapping one DLL at a time.)
#
# Also: the app's own dev_cycle.sh builds with PATH="$MINGW_BIN:$PATH", which
# is exactly this ordering, which is why the build works there and not in a
# bare shell.

# Windows itself: tasklist/taskkill/cmd/powershell live here.
export PATH="/Windows/System32:/Windows/System32/WindowsPowerShell/v1.0:$PATH"

# MSYS2 UCRT64 toolchain — GCC 15.2 (the project builds with -std=c++26).
# MUST precede the Qt bin dir (see the ordering note above).
export PATH="C:/msys64/ucrt64/bin:$PATH"

# Qt 6.11.1 (mingw_64) — the DLLs must be on PATH to *run* a dev build, and
# the headers/libs to build one.
export PATH="$PATH:C:/Qt/6.11.1/mingw_64/bin"

# CMake + Ninja ship inside the Qt install (Qt/Tools), not in MSYS2.
export PATH="C:/Qt/Tools/CMake_64/bin:C:/Qt/Tools/Ninja:$PATH"

# Git (the repo uses it for status/diff/log; dev_cycle.sh uses it to find the root).
export PATH="C:/Program Files/Git/cmd:$PATH"

# Shorthands so the rest of the tooling is one word each.
CMAKE="C:/Qt/Tools/CMake_64/bin/cmake.exe"
GIT="C:/Program Files/Git/cmd/git.exe"
export CMAKE GIT
