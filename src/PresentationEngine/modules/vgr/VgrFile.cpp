#include "modules/vgr/VgrFile.hpp"

#include "platform/PlatformAccessor.hpp"

#include <chrono>
#include <thread>

namespace bps::vgr {

namespace {
constexpr const char* kModule = "VgrFile";
// The synced-folder rename retry that used to live here (the ~8% failure rate
// when importing 680 shows into a OneDrive library) is now the PAL's
// IFilesystem::RenameRetry, shared with the plain atomic writers via
// WriteAtomic — one retry policy instead of one per saver.
} // namespace

Result<VgrDocument> VgrFile::Read(const std::string& path) {
    if (path.empty())
        return Error::Make(Err::InvalidArgument, kModule, "no file path given");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.Exists(path))
        return Error::Make(Err::NotFound, kModule, "file not found: " + path);
    auto bytes = fs.ReadBinary(path);
    if (!bytes.ok()) return bytes.error();
    return VgrSerializer::Read(bytes.value());
}

namespace {

// The folder a file lives in, or "" for a bare name. There is no ParentPath in
// the filesystem interface, and this is the one place that needs it: see the
// CreateDirectories call in Write below.
std::string ParentOf(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0)
        return std::string();
    return path.substr(0, slash);
}

} // namespace

Result<void> VgrFile::Write(const std::string& path, const VgrDocument& doc) {
    if (path.empty())
        return Error::Make(Err::InvalidArgument, kModule, "no file path given");
    auto bytes = VgrSerializer::Write(doc);
    if (!bytes.ok()) return bytes.error();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";

    // THE FOLDER MAY NOT EXIST YET, and the write below is the plain one (no
    // parent creation, unlike WriteAtomic). The crash-recovery copy is the case
    // that found this: %LOCALAPPDATA%\bps\recovery is a folder nothing else in
    // the app creates, so the first periodic copy of an unsaved show failed at
    // the very open with "cannot open for writing: …\unsaved-show.vgr.tmp" — a
    // toast about a background copy the operator never asked for. One mkdir
    // here is a no-op for every path that already works, and it makes the copy
    // path work wherever the store is pointed.
    if (const std::string parent = ParentOf(path); !parent.empty()) {
        if (auto made = fs.CreateDirectories(parent); !made.ok())
            return made.error();
    }

    // 1. Write beside the target, then read it back through the same validating
    //    reader Read() uses — proves what is on disk is a loadable file.
    if (auto w = fs.WriteBinary(tmp, bytes.value()); !w.ok()) return w.error();
    auto verify = fs.ReadBinary(tmp);
    if (!verify.ok() || verify.value() != bytes.value() ||
        !VgrSerializer::Read(verify.value()).ok()) {
        (void)fs.Remove(tmp);
        return Error::Make(Err::IoError, kModule,
                           "the saved file failed verification; your previous file was not touched");
    }

    // 2. Swap in. The old file is parked as .bak first so a failure at any point
    //    leaves one complete file on disk.
    const bool hadOld = fs.Exists(path);
    if (hadOld) {
        (void)fs.Remove(bak);
        if (auto m = fs.RenameRetry(path, bak); !m.ok()) {
            (void)fs.Remove(tmp);
            return m.error();
        }
    }
    if (auto m = fs.RenameRetry(tmp, path); !m.ok()) {
        if (hadOld) (void)fs.RenameRetry(bak, path);   // put the previous file back
        (void)fs.Remove(tmp);
        return m.error();
    }
    if (hadOld) (void)fs.Remove(bak);
    return Ok();
}

} // namespace bps::vgr
