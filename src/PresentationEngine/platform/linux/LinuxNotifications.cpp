#include "platform/linux/LinuxNotifications.hpp"

#include "platform/linux/LinuxExec.hpp"

namespace bps::platform {

using namespace linux_backend;

bool LinuxNotifications::Supported() const { return CommandAvailable("notify-send"); }

Result<void> LinuxNotifications::Show(std::string_view title, std::string_view body) {
    if (!CommandAvailable("notify-send"))
        return Error::Make(Err::Unsupported, "Notifications",
                           "notify-send not available (headless host?)");
    std::string cmd = "notify-send " + ShellQuote(title) + " " + ShellQuote(body);
    CmdResult r = RunCapture(cmd + " >/dev/null 2>&1");
    if (r.exitCode == 0) return Ok();
    return Error::Make(Err::IoError, "Notifications",
                       "notify-send exited with code " + std::to_string(r.exitCode));
}

} // namespace bps::platform
