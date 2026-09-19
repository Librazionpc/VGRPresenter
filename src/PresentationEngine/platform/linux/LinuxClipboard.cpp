#include "platform/linux/LinuxClipboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace bps::platform {

namespace {
bool HasTool(const char* tool) {
    std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

const char* ReadCommand() {
    if (std::getenv("WAYLAND_DISPLAY") && HasTool("wl-paste")) return "wl-paste -n";
    if (HasTool("xclip")) return "xclip -selection clipboard -o";
    if (HasTool("xsel")) return "xsel -b -o";
    return nullptr;
}

const char* WriteCommand() {
    if (std::getenv("WAYLAND_DISPLAY") && HasTool("wl-copy")) return "wl-copy";
    if (HasTool("xclip")) return "xclip -selection clipboard";
    if (HasTool("xsel")) return "xsel -b -i";
    return nullptr;
}

// File-list (text/uri-list) read/write commands. xclip and wl-copy support a
// dedicated target; xsel does not, so xsel-only hosts get Unsupported for
// file lists.
const char* ReadFilesCommand() {
    if (std::getenv("WAYLAND_DISPLAY") && HasTool("wl-paste"))
        return "wl-paste -n --type text/uri-list";
    if (HasTool("xclip")) return "xclip -selection clipboard -o -t text/uri-list";
    return nullptr;
}

const char* WriteFilesCommand() {
    if (std::getenv("WAYLAND_DISPLAY") && HasTool("wl-copy"))
        return "wl-copy --type text/uri-list";
    if (HasTool("xclip")) return "xclip -selection clipboard -t text/uri-list";
    return nullptr;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// "file:///home/u/a%20b.txt" (and file://host/...) -> "/home/u/a b.txt".
std::string FileUriToPath(std::string_view uri) {
    if (uri.rfind("file://", 0) != 0) return {};
    std::string_view rest = uri.substr(7);
    if (rest.rfind("localhost", 0) == 0) rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/') return {};   // remote hosts unsupported
    return PercentDecode(rest);
}
} // namespace

Result<std::string> LinuxClipboard::ReadText() const {
    const char* cmd = ReadCommand();
    if (!cmd)
        return Error::Make(Err::Unsupported, "Clipboard",
                           "no clipboard tool (wl-paste/xclip/xsel) available");
    FILE* pipe = popen(cmd, "r");
    if (!pipe)
        return Error::Make(Err::IoError, "Clipboard", "failed to spawn clipboard reader");
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    int rc = pclose(pipe);
    if (rc != 0) return Error::Make(Err::IoError, "Clipboard", "clipboard reader failed");
    return out;
}

Result<void> LinuxClipboard::WriteText(std::string_view text) {
    const char* cmd = WriteCommand();
    if (!cmd)
        return Error::Make(Err::Unsupported, "Clipboard",
                           "no clipboard tool (wl-copy/xclip/xsel) available");
    FILE* pipe = popen(cmd, "w");
    if (!pipe)
        return Error::Make(Err::IoError, "Clipboard", "failed to spawn clipboard writer");
    fwrite(text.data(), 1, text.size(), pipe);
    int rc = pclose(pipe);
    if (rc != 0) return Error::Make(Err::IoError, "Clipboard", "clipboard writer failed");
    return Ok();
}

Result<std::vector<std::string>> LinuxClipboard::GetFiles() const {
    const char* cmd = ReadFilesCommand();
    if (!cmd)
        return Error::Make(Err::Unsupported, "Clipboard",
                           "no file-list clipboard tool (wl-paste/xclip) available");
    FILE* pipe = popen(cmd, "r");
    if (!pipe)
        return Error::Make(Err::IoError, "Clipboard", "failed to spawn clipboard reader");
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    int rc = pclose(pipe);
    if (rc != 0) return Error::Make(Err::IoError, "Clipboard", "clipboard reader failed");
    std::vector<std::string> files;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string path = FileUriToPath(line);
        if (!path.empty()) files.push_back(std::move(path));
    }
    return files;
}

Result<void> LinuxClipboard::SetFiles(const std::vector<std::string>& paths) {
    const char* cmd = WriteFilesCommand();
    if (!cmd)
        return Error::Make(Err::Unsupported, "Clipboard",
                           "no file-list clipboard tool (wl-copy/xclip) available");
    std::string payload;
    for (const auto& p : paths) payload += "file://" + p + "\r\n";
    FILE* pipe = popen(cmd, "w");
    if (!pipe)
        return Error::Make(Err::IoError, "Clipboard", "failed to spawn clipboard writer");
    fwrite(payload.data(), 1, payload.size(), pipe);
    int rc = pclose(pipe);
    if (rc != 0) return Error::Make(Err::IoError, "Clipboard", "clipboard writer failed");
    return Ok();
}

} // namespace bps::platform
