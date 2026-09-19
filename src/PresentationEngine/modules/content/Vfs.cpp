#include "modules/content/Vfs.hpp"

#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace bps::content {

namespace {

std::string NormalizePath(std::string_view p) {
    std::string out;
    out.reserve(p.size());
    bool lastSlash = false;
    for (char c : p) {
        if (c == '\\') c = '/';
        if (c == '/') {
            if (lastSlash) continue;
            lastSlash = true;
        } else {
            lastSlash = false;
        }
        out += c;
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (!out.empty() && out.front() == '/') out.erase(0, 1);
    return out;
}

std::string ParentDir(std::string_view rel) {
    auto n = rel.find_last_of('/');
    return n == std::string_view::npos ? std::string{} : std::string(rel.substr(0, n));
}

uint16_t ReadLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void WriteLE16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void WriteLE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

// --- CRC-32 (IEEE 802.3), standard table-based implementation ---
uint32_t g_crcTable[256];
bool g_crcInit = false;
std::mutex g_crcMutex;

void InitCrcTable() {
    if (g_crcInit) return;
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[n] = c;
    }
    g_crcInit = true;
}

} // namespace

uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed) {
    std::lock_guard<std::mutex> lock(g_crcMutex);
    InitCrcTable();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = g_crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// DiskVfs
// ---------------------------------------------------------------------------

DiskVfs::DiskVfs(std::string name, std::string rootDir)
    : name_(std::move(name)), root_(std::move(rootDir)) {}

std::string DiskVfs::ToHostPath(std::string_view rel) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::string norm = NormalizePath(rel);
    return fs.Join(root_, norm);
}

Result<std::vector<uint8_t>> DiskVfs::Read(std::string_view path) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.ReadBinary(ToHostPath(path));
}

Result<std::string> DiskVfs::ReadText(std::string_view path) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.ReadText(ToHostPath(path));
}

Result<void> DiskVfs::Write(std::string_view path, const std::vector<uint8_t>& data) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto r = CreateDirectories(ParentDir(path)); !r.ok()) return r;
    return fs.WriteBinary(ToHostPath(path), data);
}

Result<void> DiskVfs::WriteText(std::string_view path, std::string_view text) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto r = CreateDirectories(ParentDir(path)); !r.ok()) return r;
    return fs.Write(ToHostPath(path), text);
}

Result<void> DiskVfs::Remove(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.Remove(ToHostPath(path));
}

Result<void> DiskVfs::Move(std::string_view from, std::string_view to) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto r = CreateDirectories(ParentDir(to)); !r.ok()) return r;
    return fs.Move(ToHostPath(from), ToHostPath(to));
}

Result<bool> DiskVfs::Exists(std::string_view path) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return Result<bool>{fs.Exists(ToHostPath(path))};
}

Result<uint64_t> DiskVfs::Size(std::string_view path) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.FileSize(ToHostPath(path));
}

Result<std::vector<std::string>> DiskVfs::List(std::string_view dir) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::string host = ToHostPath(dir);
    if (!fs.IsDirectory(host))
        return Error::Make(Err::NotFound, "CAMS", "no such directory in mount " + name_);
    auto entries = fs.Enumerate(host);
    if (!entries.ok()) return entries.error();
    std::vector<std::string> rel;
    rel.reserve(entries.value().size());
    // Normalize the prefix AND compare against a normalized entry —
    // fs.Enumerate() returns std::filesystem::path::string(), which is
    // backslash-separated on Windows, so a raw starts_with() against a
    // forward-slash prefix silently matched nothing on that platform (every
    // entry fell out of the loop, not just misnormalized — List() returned
    // an empty list for every real Windows directory).
    std::string prefix = NormalizePath(root_);
    if (prefix.empty() || prefix.back() != '/') prefix += '/';
    for (const auto& e : entries.value()) {
        std::string ne = NormalizePath(e);
        if (ne.starts_with(prefix)) rel.push_back(ne.substr(prefix.size()));
    }
    return Result<std::vector<std::string>>{std::move(rel)};
}

Result<void> DiskVfs::CreateDirectories(std::string_view dir) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::string host = ToHostPath(dir);
    if (fs.Exists(host)) return Ok();
    return fs.CreateDirectories(host);
}

// ---------------------------------------------------------------------------
// MemoryVfs
// ---------------------------------------------------------------------------

MemoryVfs::MemoryVfs(std::string name) : name_(std::move(name)) {}

Result<std::vector<uint8_t>> MemoryVfs::Read(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(NormalizePath(path));
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "CAMS", "memory mount: " + std::string(path));
    return Result<std::vector<uint8_t>>{it->second};
}

Result<std::string> MemoryVfs::ReadText(std::string_view path) const {
    auto r = Read(path);
    if (!r.ok()) return r.error();
    const auto& bytes = r.value();
    return Result<std::string>{std::string(bytes.begin(), bytes.end())};
}

Result<void> MemoryVfs::Write(std::string_view path, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[NormalizePath(path)] = data;
    return Ok();
}

Result<void> MemoryVfs::WriteText(std::string_view path, std::string_view text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    return Write(path, bytes);
}

Result<void> MemoryVfs::Remove(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(NormalizePath(path));
    return Ok();
}

Result<void> MemoryVfs::Move(std::string_view from, std::string_view to) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(NormalizePath(from));
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "CAMS", "memory mount: " + std::string(from));
    entries_[NormalizePath(to)] = std::move(it->second);
    entries_.erase(it);
    return Ok();
}

Result<bool> MemoryVfs::Exists(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<bool>{entries_.count(NormalizePath(path)) > 0};
}

Result<uint64_t> MemoryVfs::Size(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(NormalizePath(path));
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "CAMS", "memory mount: " + std::string(path));
    return Result<uint64_t>{it->second.size()};
}

Result<std::vector<std::string>> MemoryVfs::List(std::string_view dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string prefix = NormalizePath(dir);
    if (!prefix.empty()) prefix += "/";
    std::vector<std::string> out;
    for (const auto& [key, unused] : entries_) {
        (void)unused;
        if (key.starts_with(prefix)) out.push_back(key);
    }
    return Result<std::vector<std::string>>{std::move(out)};
}

Result<void> MemoryVfs::CreateDirectories(std::string_view) { return Ok(); }

size_t MemoryVfs::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// ---------------------------------------------------------------------------
// ZipVfs
// ---------------------------------------------------------------------------

ZipVfs::ZipVfs(std::string name, std::string archivePath)
    : name_(std::move(name)), archive_(std::move(archivePath)) {}

Result<void> ZipVfs::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    return OpenLocked();
}

Result<void> ZipVfs::OpenLocked() {
    if (opened_) return Ok();
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto bytesR = fs.ReadBinary(archive_);
    if (!bytesR.ok()) return bytesR.error();
    // Cache the archive bytes: entry reads then avoid re-reading the whole
    // file from disk per access.
    bytes_ = std::move(bytesR.value());
    const auto& data = bytes_;

    // Locate End Of Central Directory: scan backwards for PK\x05\x06.
    const uint8_t kEocdSig[4] = {0x50, 0x4B, 0x05, 0x06};
    const size_t eocdMin = data.size() >= 22 ? data.size() - 22 : 0;
    size_t eocd = std::string::npos;
    for (size_t i = data.size(); i-- > eocdMin;) {
        if (data[i] == 0x50 && data.size() - i >= 22 &&
            std::memcmp(&data[i], kEocdSig, 4) == 0) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos)
        return Error::Make(Err::ParseError, "CAMS", "not a ZIP archive: " + archive_);

    uint16_t count = ReadLE16(&data[eocd + 10]);
    uint32_t cdOffset = ReadLE32(&data[eocd + 16]);
    if (count == 0) { opened_ = true; return Ok(); }
    if (cdOffset >= data.size())
        return Error::Make(Err::ParseError, "CAMS", "corrupt ZIP central directory");

    // Walk the central directory.
    size_t pos = cdOffset;
    for (uint16_t i = 0; i < count && pos + 46 <= data.size(); ++i) {
        if (std::memcmp(&data[pos], "PK\x01\x02", 4) != 0)
            return Error::Make(Err::ParseError, "CAMS", "corrupt ZIP entry directory");
        Entry e;
        e.method = ReadLE16(&data[pos + 10]);
        e.crc = ReadLE32(&data[pos + 16]);
        e.compressedSize = ReadLE32(&data[pos + 20]);
        e.uncompressedSize = ReadLE32(&data[pos + 24]);
        uint16_t nameLen = ReadLE16(&data[pos + 28]);
        uint16_t extraLen = ReadLE16(&data[pos + 30]);
        uint16_t commentLen = ReadLE16(&data[pos + 32]);
        e.localOffset = ReadLE32(&data[pos + 42]);
        if (pos + 46 + nameLen > data.size()) break;
        std::string name(reinterpret_cast<const char*>(&data[pos + 46]), nameLen);
        entries_[NormalizePath(name)] = e;
        entryNames_.push_back(NormalizePath(name));
        pos += 46 + nameLen + extraLen + commentLen;
    }
    opened_ = true;
    return Ok();
}

Result<std::vector<uint8_t>> ZipVfs::Read(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto r = const_cast<ZipVfs*>(this)->OpenLocked(); !r.ok()) return r.error();
    auto it = entries_.find(NormalizePath(path));
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "CAMS", "zip entry not found: " + std::string(path));
    const Entry& e = it->second;
    const auto& data = bytes_;

    size_t pos = e.localOffset;
    if (pos + 30 > data.size() || std::memcmp(&data[pos], "PK\x03\x04", 4) != 0)
        return Error::Make(Err::ParseError, "CAMS", "corrupt local file header");
    uint16_t nameLen = ReadLE16(&data[pos + 26]);
    uint16_t extraLen = ReadLE16(&data[pos + 28]);
    size_t payload = pos + 30 + nameLen + extraLen;
    if (payload + e.compressedSize > data.size())
        return Error::Make(Err::ParseError, "CAMS", "truncated zip entry");

    if (e.method == 0) {   // stored
        return Result<std::vector<uint8_t>>{
            std::vector<uint8_t>(data.begin() + static_cast<ptrdiff_t>(payload),
                                 data.begin() + static_cast<ptrdiff_t>(payload + e.compressedSize))};
    }
    return Error::Make(Err::Unsupported, "CAMS",
                       std::format("zip entry uses compression method {} "
                                    "(only stored entries supported; deflate/zstd are future)",
                                    e.method));
}

Result<std::string> ZipVfs::ReadText(std::string_view path) const {
    auto r = Read(path);
    if (!r.ok()) return r.error();
    const auto& bytes = r.value();
    return Result<std::string>{std::string(bytes.begin(), bytes.end())};
}

Result<bool> ZipVfs::Exists(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto r = const_cast<ZipVfs*>(this)->OpenLocked(); !r.ok()) return r.error();
    return Result<bool>{entries_.count(NormalizePath(path)) > 0};
}

Result<uint64_t> ZipVfs::Size(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto r = const_cast<ZipVfs*>(this)->OpenLocked(); !r.ok()) return r.error();
    auto it = entries_.find(NormalizePath(path));
    if (it == entries_.end())
        return Error::Make(Err::NotFound, "CAMS", "zip entry not found: " + std::string(path));
    return Result<uint64_t>{it->second.uncompressedSize};
}

Result<std::vector<std::string>> ZipVfs::List(std::string_view dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto r = const_cast<ZipVfs*>(this)->OpenLocked(); !r.ok()) return r.error();
    std::string prefix = NormalizePath(dir);
    if (!prefix.empty()) prefix += "/";
    std::vector<std::string> out;
    for (const auto& [key, unused] : entries_) {
        (void)unused;
        if (key.starts_with(prefix)) out.push_back(key);
    }
    return Result<std::vector<std::string>>{std::move(out)};
}

// ---------------------------------------------------------------------------
// ZipWriter (stored entries)
// ---------------------------------------------------------------------------

Result<void> ZipWriter::AddEntry(std::string name, const std::vector<uint8_t>& data) {
    Pending p;
    p.name = std::move(name);
    p.data = data;
    p.crc = Crc32(data.data(), data.size());
    entries_.push_back(std::move(p));
    return Ok();
}

Result<std::vector<uint8_t>> ZipWriter::Finalize() const {
    std::vector<uint8_t> out;
    out.reserve(4096);
    std::vector<uint32_t> offsets;
    offsets.reserve(entries_.size());

    for (const auto& e : entries_) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        // Local file header
        out.insert(out.end(), {'P', 'K', 0x03, 0x04});
        WriteLE16(out, 20);                 // version needed
        WriteLE16(out, 0);                  // flags
        WriteLE16(out, 0);                  // method: stored
        WriteLE16(out, 0);                  // mod time
        WriteLE16(out, 0);                  // mod date
        WriteLE32(out, e.crc);
        WriteLE32(out, static_cast<uint32_t>(e.data.size()));
        WriteLE32(out, static_cast<uint32_t>(e.data.size()));
        WriteLE16(out, static_cast<uint16_t>(e.name.size()));
        WriteLE16(out, 0);                  // extra len
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
    }

    uint32_t cdStart = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        out.insert(out.end(), {'P', 'K', 0x01, 0x02});
        WriteLE16(out, 20);
        WriteLE16(out, 20);
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE32(out, e.crc);
        WriteLE32(out, static_cast<uint32_t>(e.data.size()));
        WriteLE32(out, static_cast<uint32_t>(e.data.size()));
        WriteLE16(out, static_cast<uint16_t>(e.name.size()));
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE16(out, 0);
        WriteLE32(out, 0);
        WriteLE32(out, offsets[i]);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cdEnd = static_cast<uint32_t>(out.size());

    out.insert(out.end(), {'P', 'K', 0x05, 0x06});
    WriteLE16(out, 0);
    WriteLE16(out, 0);
    WriteLE16(out, static_cast<uint16_t>(entries_.size()));
    WriteLE16(out, static_cast<uint16_t>(entries_.size()));
    WriteLE32(out, cdEnd - cdStart);
    WriteLE32(out, cdStart);
    WriteLE16(out, 0);
    return Result<std::vector<uint8_t>>{std::move(out)};
}

Result<void> ZipWriter::WriteTo(std::string_view hostPath) const {
    auto zip = Finalize();
    if (!zip.ok()) return zip.error();
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.WriteBinary(hostPath, zip.value());
}

} // namespace bps::content
