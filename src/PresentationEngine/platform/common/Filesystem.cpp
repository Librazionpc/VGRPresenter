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

#if defined(_WIN32)
// windows.h is already in scope above; nothing further.
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <type_traits>

namespace fs = std::filesystem;
namespace bps::platform {

namespace {

// ---------------------------------------------------------------------------
// Path conversion: every path handed to this PAL is UTF-8 — and on Windows the
// narrow route into the OS is LOSSY.
//
// fs::path(std::string), std::fstream(const char*) and CreateFileA all convert
// through the CRT's locale, which on this toolchain maps one wide char per
// BYTE. Importing a ProPresenter songbook therefore wrote
// "WE’LL TALK IT OVER.vgr" as "WEâ€™LL TALK IT OVER.vgr.tmp" and then failed to
// publish it — `rename failed … Input/output error` (ERROR_IO_DEVICE), 194
// times in one sweep, for exactly the titles holding a curly quote and not one
// ASCII title (engine.log, 2026-10-24). The names also never matched what the
// library scan listed, so the shows simply were not there afterwards.
//
// Everything below goes Windows' own UTF-8 -> UTF-16 -> OS route instead;
// POSIX paths are UTF-8 already and need no help.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int need = MultiByteToWideChar(codePage, flags, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) {
        // Not valid UTF-8 (a legacy ANSI path kept in an old index): read the
        // bytes the way this machine names them rather than dropping the path.
        codePage = CP_ACP;
        flags = 0;
        need = MultiByteToWideChar(codePage, flags, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (need <= 0) return {};
    }
    std::wstring wide(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(codePage, flags, utf8.data(), static_cast<int>(utf8.size()), wide.data(), need);
    return wide;
}

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string utf8(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), need, nullptr, nullptr);
    return utf8;
}

fs::path NativePath(std::string_view path) { return fs::path(ToWide(path)); }
std::string PathToUtf8(const fs::path& path) { return ToUtf8(path.wstring()); }

// The wide fopen: _wfopen is the only stream entry point that carries a name
// Windows can actually store.
std::FILE* OpenFile(const std::string& path, const char* mode) {
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(ToWide(path).c_str(), wmode.c_str());
}
#else
fs::path NativePath(std::string_view path) { return fs::path(std::string(path)); }
std::string PathToUtf8(const fs::path& path) { return path.string(); }
std::FILE* OpenFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
#endif

// A std::FILE* that closes itself. It replaces the std::ifstream/std::ofstream
// these functions used to open over the same wrong-name path.
struct FileCloser {
    void operator()(std::FILE* file) const {
        if (file) std::fclose(file);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

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

// ---------------------------------------------------------------------------
// Durability primitives behind WriteAtomic/CopyAtomic (see ReplaceAtomic for
// the ordering argument). All three are OS-specific, so they live here with the
// rest of the PAL's platform knowledge.
// ---------------------------------------------------------------------------

// Whether the volume could be asked to push bytes out of its cache.
enum class FlushOutcome {
    Flushed,       // the platform confirmed the data is on the medium
    Unsupported,   // the volume/filesystem has no flush primitive (a network
                   // share, a filter driver): nothing to promise, nothing broken
    Failed,        // flush exists and the volume reported a real I/O error
};

#if defined(_WIN32)

bool FlushUnsupported(DWORD e) {
    return e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ||
           e == ERROR_CALL_NOT_IMPLEMENTED;
}

// Open + FlushFileBuffers, the Windows way to say "this data is on the disk".
// `directory` opens with FILE_FLAG_BACKUP_SEMANTICS, which is the only way the
// OS lets a caller hold a directory handle at all. NOTE the access rights: a
// FILE handle needs GENERIC_WRITE for FlushFileBuffers (opens for reading fail
// with ERROR_ACCESS_DENIED, which read as a real I/O failure and failed every
// save), while a DIRECTORY handle is opened for reading and flushed as-is.
FlushOutcome FlushPathData(const std::string& path, bool directory) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), directory ? GENERIC_READ : GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING,
                           directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return FlushUnsupported(e) ? FlushOutcome::Unsupported : FlushOutcome::Failed;
    }
    const BOOL ok = FlushFileBuffers(h);
    const DWORD err = ok ? 0 : GetLastError();
    (void)CloseHandle(h);
    if (ok) return FlushOutcome::Flushed;
    return FlushUnsupported(err) ? FlushOutcome::Unsupported : FlushOutcome::Failed;
}

#else

// O_RDONLY is enough for both fsync(fd) and F_FULLFSYNC(fd) — neither needs a
// writable descriptor.
FlushOutcome FlushPathData(const std::string& path, bool directory) {
    const int flags = directory ? (O_RDONLY
#if defined(O_DIRECTORY)
                                   | O_DIRECTORY
#endif
                                   )
                                : O_RDONLY;
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        const int e = errno;
        return (e == EINVAL || e == ENOTSUP
#if defined(EOPNOTSUPP)
                || e == EOPNOTSUPP
#endif
                )
                   ? FlushOutcome::Unsupported
                   : FlushOutcome::Failed;
    }
    int rc;
#if defined(__APPLE__)
    // macOS: fsync() only reaches the drive's own cache; F_FULLFSYNC is the one
    // that asks for the platter. Falls back to fsync where a filesystem has no
    // F_FULLFSYNC (it then answers ENOTSUP, which we read as Unsupported).
    rc = ::fcntl(fd, F_FULLFSYNC);
    if (rc != 0) {
        const int e = errno;
        if (e == ENOTSUP || e == EINVAL)
            rc = ::fsync(fd);   // no F_FULLFSYNC here: the weaker flush is all there is
        else {
            (void)::close(fd);
            return FlushOutcome::Failed;
        }
    }
#else
    rc = ::fsync(fd);
#endif
    const int e = errno;
    (void)::close(fd);
    if (rc == 0) return FlushOutcome::Flushed;
    return (e == EINVAL || e == ENOTSUP
#if defined(EOPNOTSUPP)
            || e == EOPNOTSUPP
#endif
            )
               ? FlushOutcome::Unsupported
               : FlushOutcome::Failed;
}

#endif

// The bytes we hold and the bytes the file now holds, compared in chunks (no
// second copy of the whole document in memory). A short read, a longer file or
// one differing byte is a mismatch.
bool ContentMatches(const std::string& path, const void* bytes, size_t size) {
    FilePtr in(OpenFile(path, "rb"));
    if (!in) return false;
    const char* expected = static_cast<const char*>(bytes);
    constexpr size_t kChunk = 64 * 1024;
    size_t done = 0;
    std::vector<char> buf(std::min(kChunk, size == 0 ? size_t{1} : size));
    while (done < size) {
        const size_t want = std::min(buf.size(), size - done);
        if (std::fread(buf.data(), 1, want, in.get()) != want) return false;
        if (std::memcmp(buf.data(), expected + done, want) != 0) return false;
        done += want;
    }
    // Nothing may follow: a longer file is a different file.
    return std::fgetc(in.get()) == EOF;
}

// mtime tick used as a change fingerprint (ns resolution).
std::string Fingerprint(std::string_view path) {
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
    if (!GetFileAttributesExW(ToWide(path).c_str(), GetFileExInfoStandard, &data)) return {};
    uint64_t t = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                 data.ftLastWriteTime.dwLowDateTime;
    return std::to_string(t);
#else
    std::error_code ec;
    auto t = fs::last_write_time(NativePath(path), ec);
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
    return fs::exists(NativePath(path), ec);
}

bool FilesystemImpl::IsDirectory(std::string_view path) const {
    std::error_code ec;
    return fs::is_directory(NativePath(path), ec);
}

bool FilesystemImpl::IsRegularFile(std::string_view path) const {
    std::error_code ec;
    return fs::is_regular_file(NativePath(path), ec);
}

Result<std::string> FilesystemImpl::ReadText(std::string_view path) const {
    FilePtr in(OpenFile(std::string(path), "rb"));
    if (!in) return IoError("Filesystem", "cannot open for reading: " + std::string(path));
    std::string data;
    char buf[64 * 1024];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, in.get())) > 0) data.append(buf, got);
    if (std::ferror(in.get())) return IoError("Filesystem", "read failed: " + std::string(path));
    return data;
}

Result<std::vector<uint8_t>> FilesystemImpl::ReadBinary(std::string_view path) const {
    FilePtr in(OpenFile(std::string(path), "rb"));
    if (!in) return IoError("Filesystem", "cannot open for reading: " + std::string(path));
    std::vector<uint8_t> data;
    uint8_t buf[64 * 1024];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, in.get())) > 0)
        data.insert(data.end(), buf, buf + got);
    if (std::ferror(in.get())) return IoError("Filesystem", "read failed: " + std::string(path));
    return data;
}

Result<void> FilesystemImpl::Write(std::string_view path, std::string_view data) {
    FilePtr out(OpenFile(std::string(path), "wb"));
    if (!out) return IoError("Filesystem", "cannot open for writing: " + std::string(path));
    const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), out.get()) == data.size();
    const bool flushed = wrote && std::fflush(out.get()) == 0;
    // The close is checked too: a stream can fail on close alone (a full disk
    // answers there), which is why the old ofstream checked out.close().
    const bool closed = std::fclose(out.release()) == 0;
    if (!wrote || !flushed || !closed) return IoError("Filesystem", "write failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::WriteBinary(std::string_view path, const std::vector<uint8_t>& data) {
    FilePtr out(OpenFile(std::string(path), "wb"));
    if (!out) return IoError("Filesystem", "cannot open for writing: " + std::string(path));
    const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), out.get()) == data.size();
    const bool flushed = wrote && std::fflush(out.get()) == 0;
    const bool closed = std::fclose(out.release()) == 0;
    if (!wrote || !flushed || !closed) return IoError("Filesystem", "write failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::Append(std::string_view path, std::string_view data) {
    FilePtr out(OpenFile(std::string(path), "ab"));
    if (!out) return IoError("Filesystem", "cannot open for append: " + std::string(path));
    const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), out.get()) == data.size();
    const bool flushed = wrote && std::fflush(out.get()) == 0;
    const bool closed = std::fclose(out.release()) == 0;
    if (!wrote || !flushed || !closed) return IoError("Filesystem", "append failed: " + std::string(path));
    return Ok();
}

Result<void> FilesystemImpl::RenameRetry(std::string_view from, std::string_view to) {
    constexpr int kAttempts = 5;
    constexpr auto kDelay = std::chrono::milliseconds(150);
    Result<void> last = IoError("Filesystem", "rename not attempted");
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        last = Move(from, to);
        if (last.ok()) return Ok();
        // Only a transient holder (a sync filter, an indexer) is worth waiting
        // out; a genuinely missing parent or a read-only volume just pays the
        // delay and reports the same error it would have reported once.
        std::this_thread::sleep_for(kDelay);
    }
    return last;
}

Result<void> FilesystemImpl::WriteAtomic(std::string_view path, std::string_view data) {
    return ReplaceAtomic(path, data.data(), data.size());
}

Result<void> FilesystemImpl::WriteBinaryAtomic(std::string_view path,
                                               const std::vector<uint8_t>& data) {
    return ReplaceAtomic(path, data.data(), data.size());
}

// Write to a sibling temp, prove the bytes landed, then swap it in. The
// invariant is the whole point: `path` holds a COMPLETE file at every instant,
// including while a save is in flight or if the process dies mid-save. Nothing
// between the truncate and the rename can be observed through `path`.
Result<void> FilesystemImpl::ReplaceAtomic(std::string_view path, const void* bytes, size_t size) {
    if (!bytes && size > 0)
        return IoError("Filesystem", "no bytes to write for " + std::string(path));
    // The temp must be a SIBLING (one volume, so the rename is a real rename),
    // so the parent has to exist first — created here rather than pushed onto
    // every caller, which is how half of them ended up writing into a missing
    // directory and reporting it as a save failure.
    const std::string target(path);
    const size_t cut = target.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        if (auto made = CreateDirectories(std::string_view(target).substr(0, cut)); !made.ok())
            return made;
    }
    const std::string temp = target + ".tmp";
    {
        FilePtr out(OpenFile(temp, "wb"));
        if (!out) return IoError("Filesystem", "cannot open for writing: " + temp);
        const bool wrote = size == 0 ||
                           std::fwrite(bytes, 1, size, out.get()) == size;
        const bool flushed = wrote && std::fflush(out.get()) == 0;
        const bool closed = std::fclose(out.release()) == 0;   // before the size check below
        if (!wrote || !flushed || !closed) {
            (void)Remove(temp);
            return IoError("Filesystem", "write failed: " + temp);
        }
    }
    // A short (or zero-length) temp is exactly what an interrupted save leaves
    // behind, and it must never reach the target.
    auto landed = FileSize(temp);
    if (!landed.ok() || landed.value() != size) {
        const uint64_t wrote = landed.ok() ? landed.value() : 0;
        (void)Remove(temp);
        return IoError("Filesystem",
                       "wrote " + std::to_string(wrote) + " of " + std::to_string(size) +
                           " bytes to " + temp + "; " + target + " was left untouched");
    }

    // ORDERING, before anything is published: the temp's bytes go to the medium
    // FIRST, and only then does the rename put a name on them. Without this a
    // power cut can persist the rename while the data is still in the driver's
    // cache, which is how a document ends up NAMED and hollow. A volume that
    // has no flush primitive (a network share, a filter driver answering
    // ERROR_INVALID_FUNCTION) is not an error: there is simply nothing more to
    // ask of it and no promise to make. A real I/O failure here IS fatal — the
    // bytes are not durable, so the save is reported as failed and the target
    // keeps the last good document.
    switch (FlushPathData(temp, /*directory=*/false)) {
    case FlushOutcome::Flushed:
    case FlushOutcome::Unsupported:
        break;
    case FlushOutcome::Failed:
        (void)Remove(temp);
        return IoError("Filesystem",
                       "could not flush " + temp + " to disk; " + target +
                           " was left untouched");
    }

    // The byte count says the write was not SHORT; it does not say the bytes are
    // the ones we sent. Read the temp back and compare it against the buffer —
    // the read-back vgr::VgrFile::Write does for shows (which additionally
    // parses), done here for EVERY document, so a torn or reordered write is
    // caught before it can take the target's name.
    if (!ContentMatches(temp, bytes, size)) {
        (void)Remove(temp);
        return IoError("Filesystem",
                       "read-back of " + temp + " does not match the " + std::to_string(size) +
                           " bytes written; " + target + " was left untouched");
    }

    if (auto swapped = RenameRetry(temp, target); !swapped.ok()) {
        (void)Remove(temp);   // leave no debris beside the real file
        return swapped;
    }

    // The rename ITSELF is a change to the parent directory, so flush the
    // directory entry too or a power cut can undo a save that already reported
    // success. Best-effort by nature, and deliberately non-fatal: what is at
    // stake is only how RECENT the surviving document is — either the old file
    // or the new one, both complete — which is the same recency every save
    // without a journal gives away. Atomicity is never at risk here.
    if (cut != std::string::npos)
        (void)FlushPathData(target.substr(0, cut), /*directory=*/true);
    return Ok();
}

Result<void> FilesystemImpl::CopyAtomic(std::string_view from, std::string_view to) {
    const std::string source(from);
    if (!IsRegularFile(from))
        return IoError("Filesystem", "no file to copy from: " + source);
    const std::string target(to);
    const size_t cut = target.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        if (auto made = CreateDirectories(std::string_view(target).substr(0, cut)); !made.ok())
            return made;
    }
    const std::string temp = target + ".tmp";
    std::error_code ec;
    fs::copy_file(NativePath(source), NativePath(temp),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        (void)Remove(temp);
        return FsError("copy_file", source, ec);
    }
    // Whole-file check: a copy that stopped early must not be swapped in, or a
    // backup/destination ends up looking complete and unreadable.
    auto expected = FileSize(source);
    auto landed = FileSize(temp);
    if (!expected.ok() || !landed.ok() || landed.value() != expected.value()) {
        const uint64_t want = expected.ok() ? expected.value() : 0;
        const uint64_t wrote = landed.ok() ? landed.value() : 0;
        (void)Remove(temp);
        return IoError("Filesystem",
                       "copied " + std::to_string(wrote) + " of " + std::to_string(want) +
                           " bytes to " + temp + "; " + target + " was left untouched");
    }
    // Same ordering rule as ReplaceAtomic, and for the same reason: a copy that
    // backs up or restores user work must be on the medium before it takes the
    // target's name. Unsupported (no flush primitive) is not a failure.
    if (FlushPathData(temp, /*directory=*/false) == FlushOutcome::Failed) {
        (void)Remove(temp);
        return IoError("Filesystem",
                       "could not flush " + temp + " to disk; " + target +
                           " was left untouched");
    }
    if (auto swapped = RenameRetry(temp, target); !swapped.ok()) {
        (void)Remove(temp);
        return swapped;
    }
    if (cut != std::string::npos)
        (void)FlushPathData(target.substr(0, cut), /*directory=*/true);
    return Ok();
}

Result<void> FilesystemImpl::CreateDirectory(std::string_view path) {
    std::error_code ec;
    fs::create_directory(NativePath(path), ec);
    if (ec) return FsError("create_directory", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::CreateDirectories(std::string_view path) {
    std::error_code ec;
    fs::create_directories(NativePath(path), ec);
    if (ec) return FsError("create_directories", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::Remove(std::string_view path) {
    std::error_code ec;
    fs::remove(NativePath(path), ec);
    if (ec) return FsError("remove", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::RemoveAll(std::string_view path) {
    std::error_code ec;
    fs::remove_all(NativePath(path), ec);
    if (ec) return FsError("remove_all", path, ec);
    return Ok();
}

Result<void> FilesystemImpl::Move(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::rename(NativePath(from), NativePath(to), ec);
    if (ec) return FsError("rename", from, ec);
    return Ok();
}

Result<void> FilesystemImpl::Rename(std::string_view from, std::string_view to) {
    return Move(from, to);
}

Result<void> FilesystemImpl::Copy(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::copy(NativePath(from), NativePath(to), fs::copy_options::recursive, ec);
    if (ec) return FsError("copy", from, ec);
    return Ok();
}

Result<uint64_t> FilesystemImpl::FileSize(std::string_view path) const {
    std::error_code ec;
    auto size = fs::file_size(NativePath(path), ec);
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
    auto abs = fs::absolute(NativePath(path), ec);
    if (ec) return FsError("absolute", path, ec);
    return PathToUtf8(abs);
}

Result<std::vector<std::string>> FilesystemImpl::Enumerate(std::string_view dir) const {
    std::error_code ec;
    fs::directory_iterator it(NativePath(dir), ec);
    if (ec) return FsError("directory_iterator", dir, ec);
    std::vector<std::string> out;
    for (const auto& entry : it) out.push_back(PathToUtf8(entry.path()));
    return out;
}

std::string FilesystemImpl::Join(std::string_view a, std::string_view b) const {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    if (a.back() == '/' || a.back() == '\\') return std::string(a) + std::string(b);
    return std::string(a) + "/" + std::string(b);
}

std::string FilesystemImpl::Extension(std::string_view path) const {
    return PathToUtf8(NativePath(path).extension());
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
    auto st = fs::status(NativePath(path), ec);
    if (ec) return FsError("status", path, ec);
    FileMetadata m;
    m.isDirectory = fs::is_directory(st);
    m.isRegularFile = fs::is_regular_file(st);
    auto perms = st.permissions();   // std::filesystem::perms -> raw bits
    m.permissions = static_cast<uint32_t>(static_cast<std::underlying_type_t<fs::perms>>(perms));
    if (m.isRegularFile) {
        auto size = fs::file_size(NativePath(path), ec);
        if (ec) return FsError("file_size", path, ec);
        m.sizeBytes = static_cast<uint64_t>(size);
    }
#if defined(_WIN32)
    // std::filesystem on MinGW reports file times to the whole second (via stat), so two writes in the
    // same second look identical - "is this newer than that" cannot be answered. Windows itself keeps
    // 100 ns; ask for that.
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        const std::wstring native = ToWide(path);
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
    auto mtime = fs::last_write_time(NativePath(path), ec);
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
        FilePtr out(OpenFile(PathToUtf8(p), "wb"));
        if (out) return PathToUtf8(p);
    }
    return IoError("Filesystem", "cannot create temp file with prefix " + std::string(prefix));
}

Result<std::vector<std::string>> FilesystemImpl::FindFiles(std::string_view dir,
                                                           std::string_view extension) const {
    std::error_code ec;
    fs::recursive_directory_iterator it(NativePath(dir), ec);
    if (ec) return FsError("recursive_directory_iterator", dir, ec);
    std::vector<std::string> out;
    for (const auto& entry : it) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!extension.empty() && entry.path().extension() != std::string(extension)) continue;
        out.push_back(PathToUtf8(entry.path()));
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
    const bool targetIsDir = fs::is_directory(NativePath(target), ec);
    const DWORD flags = kUnprivileged | (targetIsDir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (!CreateSymbolicLinkW(ToWide(linkPath).c_str(),
                             ToWide(target).c_str(), flags)) {
        return FsError("create_symlink", linkPath,
                       std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    return Ok();
#else
    std::error_code ec;
    fs::create_symlink(NativePath(target), NativePath(linkPath), ec);
    if (ec) return FsError("create_symlink", linkPath, ec);
    return Ok();
#endif
}

Result<std::string> FilesystemImpl::ReadSymlink(std::string_view linkPath) const {
    std::error_code ec;
    auto target = fs::read_symlink(NativePath(linkPath), ec);
    if (ec) return FsError("read_symlink", linkPath, ec);
    return PathToUtf8(target);
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
        e.fingerprint = Fingerprint(e.path);
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
                std::string fp = Fingerprint(entry.path);
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
