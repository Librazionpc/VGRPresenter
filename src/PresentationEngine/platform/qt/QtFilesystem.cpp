#include "platform/qt/QtFilesystem.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
// Two kernel32 calls, declared here instead of including <windows.h>: that header #defines CreateDirectory,
// CopyFile, DeleteFile... to their -W names and would rename this class's own members.
extern "C" {
// (returns BOOLEAN — ONE byte; declared wider, the rest of the register is garbage and a failure reads as success)
__declspec(dllimport) unsigned char __stdcall CreateSymbolicLinkW(const wchar_t *link, const wchar_t *target, unsigned long flags);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
}
#endif

namespace bps::platform {
namespace {

namespace fs = std::filesystem;

// std::string is the engine's UTF-8 everywhere; QString::fromStdString reads it
// as UTF-8, so a path is never re-encoded through the user's ANSI code page.
QString Q(std::string_view utf8) { return QString::fromStdString(std::string(utf8)); }
std::string U(const QString &s) { return s.toStdString(); }

// The std::filesystem path for the three operations Qt has no portable
// primitive for. Built from the SAME UTF-8 bytes (char8_t keeps the conversion
// explicit and locale-free on Windows, where fs::path(narrow) would go through
// the ANSI code page).
fs::path FsPath(std::string_view utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string FromFsPath(const fs::path &p)
{
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

Error IoError(std::string_view op, std::string_view path, const QString &why = {})
{
    std::string message = std::string(op) + " failed for '" + std::string(path) + "'";
    if (!why.isEmpty())
        message += ": " + U(why);
    return Error::Make(Err::IoError, "Filesystem", message);
}

// An open that failed, with the OS's own reason as nativeError (Error.nativeError, DoD §19: PAL errors surface errno): a
// file that is not there is ENOENT, anything else that stopped the open is EACCES.
Error OpenError(std::string_view op, std::string_view path, const QFile &file)
{
    Error e = IoError(op, path, file.errorString());
    e.nativeError = QFileInfo(file.fileName()).exists()
        ? static_cast<int>(std::errc::permission_denied)
        : static_cast<int>(std::errc::no_such_file_or_directory);
    return e;
}

Error FsError(std::string_view op, std::string_view path, const std::error_code &ec)
{
    Error e = Error::Make(Err::IoError, "Filesystem",
                          std::string(op) + " failed for '" + std::string(path) + "': " + ec.message());
    e.nativeError = ec.value();
    return e;
}

int64_t StampMs(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return 0;
    return info.lastModified().toMSecsSinceEpoch();
}

// HOW A LISTED PATH IS SPELLED — the caller's own directory string, then the entry's relative path
// with the OS's separator. This is exactly what the previous backend returned (`dir / name`), and it
// is a CONTRACT, not a style: the media library hashes this string into each item's id and matches
// it against paths already saved in shows, sources and per-file settings. Qt's own spelling (forward
// slashes, or an absolute path) would give every existing item a new identity.
std::string ListedPath(std::string_view dir, const QString &relative)
{
    QString rel = relative;
    rel.replace(QLatin1Char('/'), QDir::separator());
    std::string out(dir);
    if (!out.empty() && out.back() != '/' && out.back() != '\\')
        out += U(QString(QDir::separator()));
    return out + U(rel);
}

// The POSIX mode bits (rwxrwxrwx as 0400 .. 0001) the contract speaks, from Qt's own flag layout —
// Qt's flags are NOT those bits (its "user read" is 0x0400, POSIX's is 0400 octal).
uint32_t PosixPermissions(QFile::Permissions p)
{
    uint32_t mode = 0;
    if (p & QFile::ReadUser) mode |= 0400;
    if (p & QFile::WriteUser) mode |= 0200;
    if (p & QFile::ExeUser) mode |= 0100;
    if (p & QFile::ReadGroup) mode |= 0040;
    if (p & QFile::WriteGroup) mode |= 0020;
    if (p & QFile::ExeGroup) mode |= 0010;
    if (p & QFile::ReadOther) mode |= 0004;
    if (p & QFile::WriteOther) mode |= 0002;
    if (p & QFile::ExeOther) mode |= 0001;
    return mode;
}

// The read-back that proves the temp holds exactly the bytes we wrote: a short
// (or silently corrupted) save must never reach the target.
bool ContentMatches(const QString &path, const void *bytes, size_t size)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const char *expected = static_cast<const char *>(bytes);
    constexpr qint64 kChunk = 64 * 1024;
    std::vector<char> buf(static_cast<size_t>(std::min<qint64>(kChunk, size == 0 ? 1 : qint64(size))));
    size_t done = 0;
    while (done < size) {
        const qint64 want = static_cast<qint64>(std::min<size_t>(buf.size(), size - done));
        const qint64 got = in.read(buf.data(), want);
        if (got != want)
            return false;
        if (std::memcmp(buf.data(), expected + done, static_cast<size_t>(want)) != 0)
            return false;
        done += static_cast<size_t>(want);
    }
    // Nothing may follow: a longer file is a different file.
    return in.read(1).isEmpty();
}

class QtFileWriteStream final : public IFileWriteStream {
public:
    explicit QtFileWriteStream(QString path) : path_(std::move(path)), file_(path_) {}

    Result<void> Open()
    {
        return file_.open(QIODevice::WriteOnly | QIODevice::Truncate)
            ? Ok() : Result<void>(IoError("open_write_stream", U(path_), file_.errorString()));
    }

    Result<size_t> Write(const uint8_t* bytes, size_t size) override
    {
        if (!file_.isOpen())
            return IoError("write_stream", U(path_), QStringLiteral("stream is closed"));
        if (!bytes && size > 0)
            return IoError("write_stream", U(path_), QStringLiteral("no bytes to write"));
        if (size > static_cast<size_t>(std::numeric_limits<qint64>::max()))
            return IoError("write_stream", U(path_), QStringLiteral("write is too large"));
        const qint64 written = size == 0 ? 0
            : file_.write(reinterpret_cast<const char*>(bytes), static_cast<qint64>(size));
        if (written < 0)
            return IoError("write_stream", U(path_), file_.errorString());
        return static_cast<size_t>(written);
    }

    Result<void> Seek(uint64_t offset) override
    {
        if (!file_.isOpen() || offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())
            || !file_.seek(static_cast<qint64>(offset)))
            return IoError("seek_write_stream", U(path_), file_.errorString());
        return Ok();
    }

    Result<void> Flush() override
    {
        if (!file_.isOpen() || !file_.flush())
            return IoError("flush_write_stream", U(path_), file_.errorString());
        return Ok();
    }

    Result<void> Close() override
    {
        if (!file_.isOpen()) return Ok();
        const bool flushed = file_.flush();
        const QString why = file_.errorString();
        file_.close();
        return flushed ? Ok() : Result<void>(IoError("close_write_stream", U(path_), why));
    }

    ~QtFileWriteStream() override { (void)Close(); }

private:
    QString path_;
    QFile file_;
};

} // namespace

QtFilesystem::~QtFilesystem()
{
    stop_.store(true);
    if (pollThread_.joinable())
        pollThread_.join();
}

bool QtFilesystem::Exists(std::string_view path) const
{
    return QFileInfo::exists(Q(path));
}

bool QtFilesystem::IsDirectory(std::string_view path) const
{
    return QFileInfo(Q(path)).isDir();
}

bool QtFilesystem::IsRegularFile(std::string_view path) const
{
    return QFileInfo(Q(path)).isFile();
}

Result<std::string> QtFilesystem::ReadText(std::string_view path) const
{
    const QString p = Q(path);
    QFile in(p);
    if (!in.open(QIODevice::ReadOnly))
        return OpenError("read", path, in);
    QByteArray data = in.readAll();
    if (in.error() != QFileDevice::NoError)
        return IoError("read", path, in.errorString());
    return std::string(data.constData(), static_cast<size_t>(data.size()));
}

Result<std::vector<uint8_t>> QtFilesystem::ReadBinary(std::string_view path) const
{
    const QString p = Q(path);
    QFile in(p);
    if (!in.open(QIODevice::ReadOnly))
        return OpenError("read", path, in);
    const QByteArray data = in.readAll();
    if (in.error() != QFileDevice::NoError)
        return IoError("read", path, in.errorString());
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(data.constData()),
                                reinterpret_cast<const uint8_t *>(data.constData()) + data.size());
}

Result<void> QtFilesystem::WriteThrough(std::string_view path, const void *bytes, size_t size,
                                        bool append, std::string_view op)
{
    const QString p = Q(path);
    QFile out(p);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    mode |= append ? QIODevice::Append : QIODevice::Truncate;
    if (!out.open(mode))
        return IoError(op, path, out.errorString());
    const bool wrote = size == 0 || out.write(static_cast<const char *>(bytes), static_cast<qint64>(size))
                                          == static_cast<qint64>(size);
    const bool flushed = wrote && out.flush();
    out.close();
    // The close is part of the write: a stream can fail on close alone (a full
    // disk answers there), which is why the C stdio backend checked it too.
    if (!wrote || !flushed)
        return IoError(op, path, out.errorString());
    return Ok();
}

Result<void> QtFilesystem::Write(std::string_view path, std::string_view data)
{
    return WriteThrough(path, data.data(), data.size(), /*append=*/false, "write");
}

Result<void> QtFilesystem::WriteBinary(std::string_view path, const std::vector<uint8_t> &data)
{
    return WriteThrough(path, data.data(), data.size(), /*append=*/false, "write");
}

Result<void> QtFilesystem::Append(std::string_view path, std::string_view data)
{
    return WriteThrough(path, data.data(), data.size(), /*append=*/true, "append");
}

Result<std::unique_ptr<IFileWriteStream>> QtFilesystem::OpenWriteStream(std::string_view path)
{
    auto stream = std::make_unique<QtFileWriteStream>(Q(path));
    if (auto opened = stream->Open(); !opened.ok()) return opened.error();
    return std::unique_ptr<IFileWriteStream>(std::move(stream));
}

namespace {
// The QFile stays open for the life of the mapping (the mapping is released through it).
class QtMappedFile final : public IMappedFile {
public:
    explicit QtMappedFile(QString path) : file_(std::move(path)) {}
    ~QtMappedFile() override {
        if (data_) file_.unmap(data_);
        file_.close();
    }
    bool Open() {
        if (!file_.open(QIODevice::ReadOnly)) return false;
        size_ = static_cast<size_t>(file_.size());
        if (size_ == 0) return true;   // nothing to map: an empty view
        data_ = file_.map(0, static_cast<qint64>(size_));
        return data_ != nullptr;
    }
    const char* Data() const override { return reinterpret_cast<const char*>(data_); }
    size_t Size() const override { return size_; }
private:
    mutable QFile file_;
    uchar* data_ = nullptr;
    size_t size_ = 0;
};
} // namespace

Result<std::shared_ptr<const IMappedFile>> QtFilesystem::MapReadOnly(std::string_view path) const
{
    auto mapped = std::make_shared<QtMappedFile>(Q(path));
    if (!mapped->Open())
        return IoError("map", path, QStringLiteral("the file could not be mapped"));
    return std::shared_ptr<const IMappedFile>(std::move(mapped));
}

Result<void> QtFilesystem::ReplaceAtomic(std::string_view path, const void *bytes, size_t size)
{
    if (!bytes && size > 0)
        return IoError("write", path, QStringLiteral("no bytes to write"));
    const QString target = Q(path);
    // The engine saves into folders that may not exist yet (a fresh project,
    // a per-show cache): the parent is ours to create.
    const QString parent = QFileInfo(target).absolutePath();
    if (!parent.isEmpty() && !QDir().mkpath(parent))
        return IoError("create_directories", U(parent));

    // THE TEMP IS path + ".tmp", confirmed byte for byte, then swapped in — the
    // same observable contract the previous backend had (including leaving a
    // stale .tmp overwritten rather than trusted).
    const QString temp = target + QStringLiteral(".tmp");
    {
        QFile out(temp);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return IoError("write", U(temp), out.errorString());
        const bool wrote = size == 0
                           || out.write(static_cast<const char *>(bytes), static_cast<qint64>(size))
                                  == static_cast<qint64>(size);
        const bool flushed = wrote && out.flush();
        out.close();
        if (!wrote || !flushed) {
            QFile::remove(temp);
            return IoError("write", U(temp), out.errorString());
        }
    }
    const qint64 landed = QFileInfo(temp).size();
    if (landed < 0 || static_cast<uint64_t>(landed) != size) {
        QFile::remove(temp);
        return IoError("write", U(temp),
                       QStringLiteral("wrote %1 of %2 bytes; %3 was left untouched")
                           .arg(landed).arg(qulonglong(size)).arg(U(target)));
    }
    if (!ContentMatches(temp, bytes, size)) {
        QFile::remove(temp);
        return IoError("write", U(temp),
                       QStringLiteral("read-back does not match; %1 was left untouched").arg(U(target)));
    }
    std::error_code ec;
    fs::rename(FsPath(U(temp)), FsPath(std::string(path)), ec);
    if (ec) {
        QFile::remove(temp);   // never leave debris beside the real file
        return FsError("rename", path, ec);
    }
    return Ok();
}

Result<void> QtFilesystem::WriteAtomic(std::string_view path, std::string_view data)
{
    return ReplaceAtomic(path, data.data(), data.size());
}

Result<void> QtFilesystem::WriteBinaryAtomic(std::string_view path,
                                             const std::vector<uint8_t> &data)
{
    return ReplaceAtomic(path, data.data(), data.size());
}

Result<void> QtFilesystem::CopyAtomic(std::string_view from, std::string_view to)
{
    if (!IsRegularFile(from))
        return IoError("copy", from, QStringLiteral("no file to copy from"));
    const QString target = Q(to);
    const QString parent = QFileInfo(target).absolutePath();
    if (!parent.isEmpty() && !QDir().mkpath(parent))
        return IoError("create_directories", U(parent));
    const std::string temp = std::string(to) + ".tmp";
    std::error_code ec;
    fs::copy_file(FsPath(from), FsPath(temp), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        QFile::remove(Q(temp));
        return FsError("copy_file", from, ec);
    }
    auto expected = FileSize(from);
    auto landed = FileSize(temp);
    if (!expected.ok() || !landed.ok() || landed.value() != expected.value()) {
        QFile::remove(Q(temp));
        return IoError("copy", std::string(to),
                       QStringLiteral("copy is not the whole file; %1 was left untouched").arg(U(target)));
    }
    fs::rename(FsPath(temp), FsPath(std::string(to)), ec);
    if (ec) {
        QFile::remove(Q(temp));
        return FsError("rename", to, ec);
    }
    return Ok();
}

Result<void> QtFilesystem::RenameRetry(std::string_view from, std::string_view to)
{
    constexpr int kAttempts = 5;
    constexpr auto kDelay = std::chrono::milliseconds(150);
    Result<void> last(Error::Make(Err::IoError, "Filesystem", "rename not attempted"));
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        last = Move(from, to);
        if (last.ok())
            return Ok();
        std::this_thread::sleep_for(kDelay);
    }
    return last;
}

Result<void> QtFilesystem::CreateDirectory(std::string_view path)
{
    const QString p = Q(path);
    if (QFileInfo(p).isDir())
        return Ok();
    if (!QDir().mkdir(p))
        return IoError("create_directory", path);
    return Ok();
}

Result<void> QtFilesystem::CreateDirectories(std::string_view path)
{
    const QString p = Q(path);
    if (p.isEmpty() || QFileInfo(p).isDir())
        return Ok();
    if (!QDir().mkpath(p))
        return IoError("create_directories", path);
    return Ok();
}

Result<void> QtFilesystem::Remove(std::string_view path)
{
    const QString p = Q(path);
    const QFileInfo info(p);
    // Removing what is already gone is SUCCESS (the previous backend's contract, and what callers
    // that clean up "if it is there" rely on): the state asked for is the state there is.
    if (!info.exists() && !info.isSymLink())
        return Ok();
    const bool removed = info.isDir() && !info.isSymLink() ? QDir().rmdir(p) : QFile::remove(p);
    if (!removed)
        return IoError("remove", path);
    return Ok();
}

Result<void> QtFilesystem::RemoveAll(std::string_view path)
{
    const QString p = Q(path);
    if (!QFileInfo::exists(p))
        return Ok();
    if (QFileInfo(p).isDir()) {
        QDir dir(p);
        if (!dir.removeRecursively())
            return IoError("remove_all", path);
        return Ok();
    }
    if (!QFile::remove(p))
        return IoError("remove_all", path);
    return Ok();
}

Result<void> QtFilesystem::Move(std::string_view from, std::string_view to)
{
    std::error_code ec;
    fs::rename(FsPath(from), FsPath(to), ec);
    if (ec)
        return FsError("rename", from, ec);
    return Ok();
}

Result<void> QtFilesystem::Rename(std::string_view from, std::string_view to)
{
    return Move(from, to);
}

Result<void> QtFilesystem::Copy(std::string_view from, std::string_view to)
{
    std::error_code ec;
    fs::copy(FsPath(from), FsPath(to), fs::copy_options::recursive, ec);
    if (ec)
        return FsError("copy", from, ec);
    return Ok();
}

Result<uint64_t> QtFilesystem::FileSize(std::string_view path) const
{
    const QFileInfo info(Q(path));
    if (!info.exists() || !info.isFile())
        return IoError("file_size", path, QStringLiteral("not a regular file"));
    return static_cast<uint64_t>(info.size());
}

Result<std::string> QtFilesystem::Absolute(std::string_view path) const
{
    if (path.empty())
        return std::string();
    std::error_code ec;
    const fs::path abs = fs::absolute(FsPath(path), ec);
    if (ec)
        return FsError("absolute", path, ec);
    return FromFsPath(abs);
}

Result<IFilesystem::FileMetadata> QtFilesystem::Metadata(std::string_view path) const
{
    if (path.empty())
        return FileMetadata{};
    const QFileInfo info(Q(path));
    if (!info.exists() && !info.isSymLink())
        {
            // The PAL contract carries the OS error code (DoD §19): "no such file" is ENOENT on every OS.
            Error e = IoError("status", path, QStringLiteral("no such file"));
            e.nativeError = static_cast<int>(std::errc::no_such_file_or_directory);
            return e;
        }
    FileMetadata m;
    m.isDirectory = info.isDir();
    m.isRegularFile = info.isFile();
    m.permissions = PosixPermissions(info.permissions());
    if (m.isRegularFile)
        m.sizeBytes = static_cast<uint64_t>(info.size());
    // ms is the finest Qt reports; the contract's unit is ns, so scale it and
    // keep the socket (a mtime the watcher can compare) stable across platforms.
    m.modifiedEpochNs = info.lastModified().toMSecsSinceEpoch() * 1000000LL;
    return m;
}

Result<std::string> QtFilesystem::CreateTempFile(std::string_view prefix)
{
    QTemporaryFile temp(QDir::tempPath() + QLatin1Char('/') + Q(prefix) + QStringLiteral("XXXXXX.tmp"));
    temp.setAutoRemove(false);
    if (!temp.open())
        return IoError("create_temp", prefix, temp.errorString());
    const QString name = temp.fileName();
    temp.close();
    return U(name);
}

Result<std::vector<std::string>> QtFilesystem::FindFiles(std::string_view dir,
                                                         std::string_view extension) const
{
    const QString root = Q(dir);
    if (!QFileInfo(root).isDir())
        return IoError("find_files", dir, QStringLiteral("not a directory"));
    std::vector<std::string> out;
    const QDir rootDir(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (!extension.empty()) {
            // The contract's extension INCLUDES the dot (".json"), the same
            // spelling std::filesystem::path::extension() produces.
            if (it.fileInfo().suffix().isEmpty()
                || QLatin1Char('.') + it.fileInfo().suffix() != Q(extension))
                continue;
        }
        out.push_back(ListedPath(dir, rootDir.relativeFilePath(file)));
    }
    return out;
}

Result<void> QtFilesystem::CreateSymlink(std::string_view target, std::string_view linkPath)
{
    // Qt has no "create a symbolic link" API (QFile::link writes a .lnk shell shortcut on Windows, a different
    // object). std is the portable primitive — except on Windows, where this toolchain's std::filesystem reports
    // create_symlink as "function not implemented"; there it is the OS call itself, with the OS's own error.
#if defined(_WIN32)
    const std::wstring wtarget = Q(target).toStdWString();
    const std::wstring wlink = Q(linkPath).toStdWString();
    const bool targetIsDir = QFileInfo(Q(target)).isDir();
    // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (0x2): works without elevation when Developer Mode is on.
    const unsigned long flags = (targetIsDir ? 0x1ul /* SYMBOLIC_LINK_FLAG_DIRECTORY */ : 0ul) | 0x2ul;
    if (!CreateSymbolicLinkW(wlink.c_str(), wtarget.c_str(), flags)) {
        Error e = IoError("create_symlink", linkPath);
        e.nativeError = static_cast<int>(GetLastError());   // 1314 = ERROR_PRIVILEGE_NOT_HELD
        return e;
    }
    return Ok();
#else
    std::error_code ec;
    fs::create_symlink(FsPath(target), FsPath(linkPath), ec);
    if (ec)
        return FsError("create_symlink", linkPath, ec);
    return Ok();
#endif
}

Result<std::string> QtFilesystem::ReadSymlink(std::string_view linkPath) const
{
    // Qt reads a link on every OS (an NTFS symbolic link included); std's read_symlink is "not implemented" on the
    // Windows toolchain. An empty answer is "not a link".
    const QFileInfo info(Q(linkPath));
    if (!info.isSymLink()) {
        Error e = IoError("read_symlink", linkPath, QStringLiteral("not a symbolic link"));
        e.nativeError = static_cast<int>(std::errc::invalid_argument);
        return e;
    }
    return U(info.symLinkTarget());
}

Result<std::vector<std::string>> QtFilesystem::Enumerate(std::string_view dir) const
{
    const QDir root(Q(dir));
    if (!root.exists())
        return IoError("enumerate", dir, QStringLiteral("not a directory"));
    std::vector<std::string> out;
    const QFileInfoList entries =
        root.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                           QDir::NoSort);
    for (const QFileInfo &entry : entries)
        out.push_back(ListedPath(dir, entry.fileName()));
    return out;
}

std::string QtFilesystem::Join(std::string_view a, std::string_view b) const
{
    if (a.empty())
        return std::string(b);
    if (b.empty())
        return std::string(a);
    if (a.back() == '/' || a.back() == '\\')
        return std::string(a) + std::string(b);
    return std::string(a) + "/" + std::string(b);
}

std::string QtFilesystem::Extension(std::string_view path) const
{
    return FromFsPath(FsPath(path).extension());
}

Result<IFilesystem::WatchToken> QtFilesystem::Watch(
    std::string_view path, std::function<void(std::string_view)> onChange)
{
    if (!Exists(path))
        return Error::Make(Err::NotFound, "Filesystem",
                           "watch target does not exist: " + std::string(path));
    const QString p = Q(path);
    WatchToken token = nextToken_++;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WatchEntry e;
        e.path = std::string(path);
        e.onChange = std::move(onChange);
        e.stampMs = StampMs(p);
        watches_[token] = std::move(e);
        if (!pollThreadStarted_) {
            pollThreadStarted_ = true;
            pollThread_ = std::thread([this] { PollLoop(); });
        }
    }
    return token;
}

Result<void> QtFilesystem::CancelWatch(WatchToken token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (watches_.erase(token) == 0)
        return Error::Make(Err::NotFound, "Filesystem", "unknown watch token");
    return Ok();
}

void QtFilesystem::PollLoop()
{
    using namespace std::chrono;
    // The same 500 ms cadence the previous backend used: a poll thread, not a
    // Qt event loop, because Watch() is callable from any engine thread and a
    // QFileSystemWatcher only delivers where an event loop is running.
    const auto interval = milliseconds(500);
    while (!stop_.load()) {
        std::this_thread::sleep_for(interval);
        if (stop_.load())
            break;
        std::vector<std::pair<std::string, std::function<void(std::string_view)>>> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &[token, entry] : watches_) {
                const int64_t stamp = StampMs(Q(entry.path));
                if (stamp != entry.stampMs) {
                    entry.stampMs = stamp;
                    changed.emplace_back(entry.path, entry.onChange);
                }
            }
        }
        for (auto &[changedPath, cb] : changed)
            if (cb)
                cb(changedPath);
    }
}

} // namespace bps::platform
