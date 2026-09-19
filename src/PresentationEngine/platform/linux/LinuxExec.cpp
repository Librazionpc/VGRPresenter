#include "platform/linux/LinuxExec.hpp"

#include <cstdio>
#include <cstring>

namespace bps::platform::linux_backend {

std::string ShellQuote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

bool CommandAvailable(const char* tool) {
    std::string cmd = "command -v ";
    cmd += tool;
    cmd += " >/dev/null 2>&1";
    return ::system(cmd.c_str()) == 0;
}

CmdResult RunCapture(const std::string& cmd) {
    CmdResult r;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[4096];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof buf, pipe)) > 0)
        r.stdoutText.append(buf, n);
    r.exitCode = ::pclose(pipe);
    if (r.exitCode != -1 && !r.stdoutText.empty() && r.stdoutText.back() == '\n')
        r.stdoutText.pop_back();
    return r;
}

} // namespace bps::platform::linux_backend
