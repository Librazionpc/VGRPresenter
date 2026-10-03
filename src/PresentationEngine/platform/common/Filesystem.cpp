#include "platform/common/Filesystem.hpp"
#include "platform/OsTag.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// <windows.h> #defines CreateDirectory to CreateDirectoryA/W — silently
// rewriting our own FilesystemImpl::CreateDirectory definition below into
// "CreateDirectoryA", which then doesn't match its own declaration. Every
// other method name in this file was checked against windows.h and doesn't
// collide; this is the only one.
#undef CreateDirectory
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <type_traits>

namespace fs = std::filesystem;
namespace bps::platform {

namespace {
Error FsError(std::string_view op, std::string_view path, const std::error_code& ec,
              std::string_view module = "Filesystem") {
    Error e = Error::Make(Err::IoError, module,
                          std::string(op) + " failed for '" + std::string(path) + "': " +
                              ec.message());
    e.nativeError = ec.value();   // PAL DoD §19: native error surfaced
    e.stack = CaptureStack();     // debug builds only
    return e;
}

// Stream-open / write failures: surfaces errno + debug stack (DoD §19).
Error IoError(std::string_view module, std::string_view message) {
    Error e = Error::Make(Err::IoError, module, std::string(message));
    if (errno != 0) e.nativeError = errno;
    e.stack = CaptureStack();
    return e;
}

// mtime tick used as a change fingerprint (ns resolution).
std::string Fingerprint(const fs::path& p) {
#if defined(_WIN32)
    // Verified by direct trace (2026-09-19): this MinGW-w64/libstdc++
    // combination's std::filesystem::last_write_time() returns the exact
    // same value before and after a real, confirmed-successful rewrite of
    // the file's contents — not stale-but-eventually-consistent, genuinely
    // frozen. A file-watch fingerprint built on it can never detect a
    // change on Windows. GetFileAttributesExA's raw FILETIME (no
    // std::filesystem clock-epoch conversion involved at all) doesn't go
    // through whatever that library's bug is.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(p.string().c_str(), GetFileExInfoStandard, &data)) return {};
    uint64_t t = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                 data.ftLastWriteTime.dwLowDateTime;
    return std::to_string(t);
#else
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return {};
    return std::to_string(t.time_since_epoch().count());
#endif
}
} // namespace

FilesystemImpl::~FilesystemImpl() {
    stop_.store(true);
    if (pollThread_.joinable()) pollThread_.join();
}

bool FilesystemImpl::Exists(std::string_view path) const {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

bool FilesystemImpl::IsDirectory(std::string_view path) const {
    std::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

bool FilesystemImpl::IsRegularFile(std::string_view path) const {
    std::error_code ec;
    return fs::is_regular_file(fs::path(path), ec);
}

Result<std::string> FilesystemImpl::ReadText(std::string_view path) const {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) return IoError("Filesystem", "cannot open for reading: " + std::string(path));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Result<std::vector<uint8_t>> FilesystemImpl::ReadBinary(std::string_view path) const {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) return IoError("Filesystem", "cannot open for reading: " + std::string(path));
    in.seekg(0, std::ios::end);
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

Result<void> FilesystemImpl::Write(std::string_view path, std::string_view data) {
    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    if (!out) return IoError("Filesystem", "cannot open for writing: " + std::string(path));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) return IoError("Filesystem", "write failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::WriteBinary(std::string_view path, const std::vector<uint8_t>& data) {
    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    if (!out) return IoError("Filesystem", "cannot open for writing: " + std::string(path));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) return IoError("Filesystem", "write failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::Append(std::string_view path, std::string_view data) {
    std::ofstream out(std::string(path), std::ios::binary | std::ios::app);
    if (!out) return IoError("Filesystem", "cannot open for append: " + std::string(path));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) return IoError("Filesystem", "append failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::CreateDirectory(std::string_view path) {
    std::error_code ec;
    fs::create_directory(fs::path(path), ec);
    if (ec) return FsError("create_directory", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::CreateDirectories(std::string_view path) {
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
    if (ec) return FsError("create_directories", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::Remove(std::string_view path) {
    std::error_code ec;
    fs::remove(fs::path(path), ec);
    if (ec) return FsError("remove", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::RemoveAll(std::string_view path) {
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
    if (ec) return FsError("remove_all", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::Move(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::rename(fs::path(from), fs::path(to), ec);
    if (ec) return FsError("rename", from, ec);
    return Ok();
}

Result<void> FilesystemImpl::Rename(std::string_view from, std::string_view to) {
    return Move(from, to);
}

Result<void> FilesystemImpl::Copy(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::copy(fs::path(from), fs::path(to), fs::copy_options::recursive, ec);
    if (ec) return FsError("copy", from, ec);
    return Ok();
}

Result<uint64_t> FilesystemImpl::FileSize(std::string_view path) const {
    std::error_code ec;
    auto size = fs::file_size(fs::path(path), ec);
    if (ec) return FsError("file_size", path, ec);
    return static_cast<uint64_t>(size);
}

Result<std::string> FilesystemImpl::Absolute(std::string_view path) const {
    // An EMPTY path is not an error to report — it is the caller asking a
    // question with no answer, and the honest reply is the empty string.
    // std::filesystem::absolute() disagrees: on Windows it calls
    // GetFullPathNameW, which fails with ERROR_INVALID_PARAMETER (22) for an
    // empty string. The error_code overload below does NOT throw, so this
    // function itself was never the problem — but it returned a Result the
    // callers treat as fatal, and worse, callers that reached for the
    // THROWING overload died here instead (see HardenAbsolute below).
    //
    // Measured on this toolchain (GCC 15.2 MinGW, -std=c++26):
    //   fs::absolute(fs::path(""), ec) -> ec=22 'Invalid argument', result=''
    //   fs::absolute(fs::path(""))     -> throws filesystem_error
    //     "filesystem error: cannot make absolute path: Invalid argument []"
    if (path.empty()) return std::string();

    std::error_code ec;
    auto abs = fs::absolute(fs::path(path), ec);
    if (ec) return FsError("absolute", path, ec);
    return abs.string();
}

Result<std::vector<std::string>> FilesystemImpl::Enumerate(std::string_view dir) const {
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) return FsError("directory_iterator", dir, ec);
    std::vector<std::string> out;
    for (const auto& entry : it) out.push_back(entry.path().string());
    return out;
}

std::string FilesystemImpl::Join(std::string_view a, std::string_view b) const {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    if (a.back() == '/' || a.back() == '\\') return std::string(a) + std::string(b);
    return std::string(a) + "/" + std::string(b);
}

std::string FilesystemImpl::Extension(std::string_view path) const {
    return fs::path(path).extension().string();
}

Result<IFilesystem::FileMetadata> FilesystemImpl::Metadata(std::string_view path) const {
    // Empty path: no file has no metadata. Reporting IoError here made an
    // "is this thing a file?" probe on an empty string look like a disk
    // failure in the log; the honest answer is "not a file, not a directory".
    if (path.empty()) {
        FileMetadata none;
        none.isDirectory = false;
        none.isRegularFile = false;
        return none;
    }
    std::error_code ec;
    auto st = fs::status(fs::path(path), ec);
    if (ec) return FsError("status", path, ec);
    FileMetadata m;
    m.isDirectory = fs::is_directory(st);
    m.isRegularFile = fs::is_regular_file(st);
    auto perms = st.permissions();   // std::filesystem::perms -> raw bits
    m.permissions = static_cast<uint32_t>(static_cast<std::underlying_type_t<fs::perms>>(perms));
    if (m.isRegularFile) {
        auto size = fs::file_size(fs::path(path), ec);
        if (ec) return FsError("file_size", path, ec);
        m.sizeBytes = static_cast<uint64_t>(size);
    }
#if defined(_WIN32)
    // std::filesystem on MinGW reports file times to the whole second (via stat), so two writes in the
    // same second look identical - "is this newer than that" cannot be answered. Windows itself keeps
    // 100 ns; ask for that.
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        const fs::path native(path);
        if (GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data)) {
            ULARGE_INTEGER ticks;
            ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
            ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
            constexpr unsigned long long kUnixEpochTicks = 116444736000000000ULL;   // 1601 -> 1970, in 100 ns
            if (ticks.QuadPart >= kUnixEpochTicks) {
                m.modifiedEpochNs = static_cast<int64_t>((ticks.QuadPart - kUnixEpochTicks) * 100ULL);
                return m;
            }
        }
    }
#endif
    auto mtime = fs::last_write_time(fs::path(path), ec);
    if (!ec) {
        // file_clock's epoch is not the Unix epoch (on Windows it is 1601, and nanoseconds since
        // then overflow int64), so convert to the system clock before counting.
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(mtime);
        m.modifiedEpochNs = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
    }
    return m;
}

Result<std::string> FilesystemImpl::CreateTempFile(std::string_view prefix) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec) return FsError("temp_directory_path", "", ec);

    std::random_device rd;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string name = std::string(prefix) + std::to_string(rd()) + ".tmp";
        fs::path p = dir / name;
        std::ofstream out(p.string(), std::ios::binary | std::ios::trunc);
        if (out) return p.string();
    }
    return IoError("Filesystem", "cannot create temp file with prefix " + std::string(prefix));
}

Result<std::vector<std::string>> FilesystemImpl::FindFiles(std::string_view dir,
                                                           std::string_view extension) const {
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(dir), ec);
    if (ec) return FsError("recursive_directory_iterator", dir, ec);
    std::vector<std::string> out;
    for (const auto& entry : it) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!extension.empty() && entry.path().extension() != std::string(extension)) continue;
        out.push_back(entry.path().string());
    }
    return out;
}

Result<void> FilesystemImpl::CreateSymlink(std::string_view target, std::string_view linkPath) {
#if defined(_WIN32)
    // std::filesystem::create_symlink calls CreateSymbolicLinkW WITHOUT
    // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (0x2, Windows 10 1703+)
    // — without it, symlink creation needs full elevation even with
    // Developer Mode on, which is why this failed outright on a normal
    // (non-admin) process. The flag doesn't remove the requirement that
    // SOME privilege be granted (elevation OR Developer Mode still has to
    // be on), it just makes Developer Mode alone sufficient instead of
    // requiring elevation on top of it — the standard, documented way an
    // ordinary process creates symlinks on modern Windows.
    constexpr DWORD kUnprivileged = 0x2;   // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    std::error_code ec;
    const bool targetIsDir = fs::is_directory(fs::path(target), ec);
    const DWORD flags = kUnprivileged | (targetIsDir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (!CreateSymbolicLinkW(fs::path(linkPath).wstring().c_str(),
                             fs::path(target).wstring().c_str(), flags)) {
        return FsError("create_symlink", linkPath,
                       std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    return Ok();
#else
    std::error_code ec;
    fs::create_symlink(fs::path(target), fs::path(linkPath), ec);
    if (ec) return FsError("create_symlink", linkPath, ec);
    return Ok();
#endif
}

Result<std::string> FilesystemImpl::ReadSymlink(std::string_view linkPath) const {
    std::error_code ec;
    auto target = fs::read_symlink(fs::path(linkPath), ec);
    if (ec) return FsError("read_symlink", linkPath, ec);
    return target.string();
}

Result<IFilesystem::WatchToken> FilesystemImpl::Watch(
    std::string_view path, std::function<void(std::string_view)> onChange) {
    if (!Exists(path))
        return Error::Make(Err::NotFound, "Filesystem",
                           "watch target does not exist: " + std::string(path));

    WatchToken token = nextToken_++;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WatchEntry e;
        e.path = std::string(path);
        e.onChange = std::move(onChange);
        e.fingerprint = Fingerprint(fs::path(e.path));
        watches_[token] = std::move(e);
        if (!pollThreadStarted_) {
            pollThreadStarted_ = true;
            pollThread_ = std::thread([this] { PollLoop(); });
        }
    }
    return token;
}

Result<void> FilesystemImpl::CancelWatch(WatchToken token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (watches_.erase(token) == 0)
        return Error::Make(Err::NotFound, "Filesystem", "unknown watch token");
    return Ok();
}

void FilesystemImpl::PollLoop() {
    using namespace std::chrono;
    const auto interval = milliseconds(500);
    while (!stop_.load()) {
        std::this_thread::sleep_for(interval);
        if (stop_.load()) break;

        // Detect changes under the lock, then fire the callbacks OUTSIDE it so
        // a callback may safely call Watch()/CancelWatch() without deadlocking
        // (the callback is copied by value, so erasing its watch is fine).
        std::vector<std::pair<std::string, std::function<void(std::string_view)>>> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [token, entry] : watches_) {
                std::string fp = Fingerprint(fs::path(entry.path));
                if (fp != entry.fingerprint) {
                    entry.fingerprint = fp;
                    changed.emplace_back(entry.path, entry.onChange);
                }
            }
        }
        for (auto& [path, cb] : changed) cb(path);
    }
}

} // namespace bps::platform
