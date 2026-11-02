#pragma once

#include <string>

namespace bps::platform::linux_backend {
// Escapes `s` for safe interpolation into a /bin/sh single-quoted argument.
std::string ShellQuote(std::string_view s);

// True when `tool` is on PATH (command -v).
bool CommandAvailable(const char* tool);

struct CmdResult {
    int exitCode = -1;
    std::string stdoutText;
};

// Runs `cmd` via /bin/sh, capturing stdout (popen). exitCode = -1 on popen
// failure.
CmdResult RunCapture(const std::string& cmd);
} // namespace bps::platform::linux_backend