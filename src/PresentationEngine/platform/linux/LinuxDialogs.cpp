#include "platform/linux/LinuxDialogs.hpp"

#include "platform/linux/LinuxExec.hpp"

#include <cstdlib>
#include <sstream>

namespace bps::platform {

using namespace linux_backend;

namespace {

const char* kZenity = "zenity";

Error NoDesktop(const char* tool) {
    return Error::Make(Err::Unsupported, "Dialogs",
                       std::string(tool) + " not available (no desktop session?)");
}

bool HasDisplay() {
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
}

const char* DialogTool() { return CommandAvailable(kZenity) ? kZenity : "kdialog"; }

// zenity's --file-filter wants "NAME | *.a *.b" (patterns after a pipe).
// The callers pass the Qt-style "NAME (*.a *.b)" — rewritten here so the
// filter actually matches files instead of being a name with no patterns.
std::string ZenityFileFilter(const std::string& f) {
    size_t open = f.find('(');
    size_t close = f.find(')', open);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1)
        return f;
    std::string name = f.substr(0, open);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    std::string pats = f.substr(open + 1, close - open - 1);
    // One pattern per token; bare "*" becomes zenity's every-file "*".
    std::string joined;
    std::istringstream pats_in{pats};
    std::string tok;
    while (pats_in >> tok) {
        if (!joined.empty()) joined.push_back(' ');
        joined += tok;
    }
    if (joined.empty()) return name;
    return name + " | " + joined;
}

// Runs the zenity file-selection dialog; returns the picked path or nullopt on
// cancel. File pickers are zenity-only (kdialog uses a different syntax); the
// kdialog fallback is used for message/question boxes via DialogTool().
Result<std::optional<std::string>> RunFileDialog(const std::string& args) {
    if (!HasDisplay()) return NoDesktop("dialogs");
    if (!CommandAvailable(kZenity)) return NoDesktop("zenity");
    CmdResult r = RunCapture(args);
    // zenity: 0 = selected, 1 = cancelled, 5 = timeout.
    if (r.exitCode == 0 && !r.stdoutText.empty())
        return std::optional<std::string>{r.stdoutText};
    return std::optional<std::string>{};   // cancelled / timeout
}

} // namespace

Result<std::optional<std::string>> LinuxDialogs::OpenFileDialog(
    std::string_view title, const std::vector<std::string>& filters) {
    std::string cmd = std::string(kZenity) + " --file-selection --title=" +
                      ShellQuote(title);
    for (const auto& f : filters)
        cmd += " --file-filter=" + ShellQuote(ZenityFileFilter(f));
    return RunFileDialog(cmd + " 2>/dev/null");
}

Result<std::optional<std::string>> LinuxDialogs::SaveFileDialog(
    std::string_view title, std::string_view defaultName) {
    if (!defaultName.empty())
        return RunFileDialog(std::string(kZenity) +
                             " --file-selection --save --confirm-overwrite --title=" +
                             ShellQuote(title) + " --filename=" + ShellQuote(defaultName) +
                             " 2>/dev/null");
    return RunFileDialog(std::string(kZenity) +
                         " --file-selection --save --confirm-overwrite --title=" +
                         ShellQuote(title) + " 2>/dev/null");
}

Result<std::optional<std::string>> LinuxDialogs::SelectFolderDialog(std::string_view title) {
    return RunFileDialog(std::string(kZenity) +
                         " --file-selection --directory --title=" + ShellQuote(title) +
                         " 2>/dev/null");
}

Result<void> LinuxDialogs::MessageDialog(std::string_view title, std::string_view message) {
    if (!HasDisplay()) return NoDesktop("dialogs");
    const char* tool = DialogTool();
    if (!CommandAvailable(kZenity) && !CommandAvailable("kdialog"))
        return NoDesktop("zenity/kdialog");
    CmdResult r = RunCapture(std::string(tool) + " --info --title=" + ShellQuote(title) +
                             " --text=" + ShellQuote(message) + " 2>/dev/null");
    if (r.exitCode == 0) return Ok();
    return Error::Make(Err::IoError, "Dialogs",
                       std::string(tool) + " exited with code " + std::to_string(r.exitCode));
}

Result<bool> LinuxDialogs::QuestionDialog(std::string_view title, std::string_view message) {
    if (!HasDisplay()) return NoDesktop("dialogs");
    const char* tool = DialogTool();
    if (!CommandAvailable(kZenity) && !CommandAvailable("kdialog"))
        return NoDesktop("zenity/kdialog");
    CmdResult r = RunCapture(std::string(tool) + " --question --title=" + ShellQuote(title) +
                             " --text=" + ShellQuote(message) + " 2>/dev/null");
    return r.exitCode == 0;   // 0 = Yes/OK; anything else (No/Cancel/timeout) = false
}

Result<std::optional<std::string>> LinuxDialogs::ColorPicker(std::string_view title,
                                                             std::string_view initialColor) {
    // zenity --color-selection prints '#RRGGBB' on OK. Requires a desktop
    // session + zenity; kdialog uses a different syntax so this is zenity-only
    // (like the file pickers).
    if (!HasDisplay()) return NoDesktop("dialogs");
    if (!CommandAvailable(kZenity)) return NoDesktop("zenity");
    std::string cmd = std::string(kZenity) + " --color-selection --title=" + ShellQuote(title);
    if (!initialColor.empty() && initialColor.front() == '#')
        cmd += " --color=" + ShellQuote(initialColor);
    CmdResult r = RunCapture(cmd + " 2>/dev/null");
    if (r.exitCode != 0 || r.stdoutText.empty()) return std::optional<std::string>{};
    std::string color = r.stdoutText;
    while (!color.empty() && (color.back() == '\n' || color.back() == '\r'))
        color.pop_back();
    return std::optional<std::string>{color};
}

Result<std::optional<std::string>> LinuxDialogs::FontPicker(std::string_view title,
                                                            std::string_view initialFont) {
    // zenity --font-selection prints 'Family Style size' on OK.
    if (!HasDisplay()) return NoDesktop("dialogs");
    if (!CommandAvailable(kZenity)) return NoDesktop("zenity");
    std::string cmd = std::string(kZenity) + " --font-selection --title=" + ShellQuote(title);
    if (!initialFont.empty()) cmd += " --fontname=" + ShellQuote(initialFont);
    CmdResult r = RunCapture(cmd + " 2>/dev/null");
    if (r.exitCode != 0 || r.stdoutText.empty()) return std::optional<std::string>{};
    std::string font = r.stdoutText;
    while (!font.empty() && (font.back() == '\n' || font.back() == '\r'))
        font.pop_back();
    return std::optional<std::string>{font};
}

} // namespace bps::platform
