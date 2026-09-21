#include "modules/vgr/VgrFile.hpp"

#include "platform/PlatformAccessor.hpp"

namespace bps::vgr {

namespace {
constexpr const char* kModule = "VgrFile";
}

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

Result<void> VgrFile::Write(const std::string& path, const VgrDocument& doc) {
    if (path.empty())
        return Error::Make(Err::InvalidArgument, kModule, "no file path given");
    auto bytes = VgrSerializer::Write(doc);
    if (!bytes.ok()) return bytes.error();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";

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
        if (auto m = fs.Move(path, bak); !m.ok()) {
            (void)fs.Remove(tmp);
            return m.error();
        }
    }
    if (auto m = fs.Move(tmp, path); !m.ok()) {
        if (hadOld) (void)fs.Move(bak, path);   // put the previous file back
        (void)fs.Remove(tmp);
        return m.error();
    }
    if (hadOld) (void)fs.Remove(bak);
    return Ok();
}

} // namespace bps::vgr
