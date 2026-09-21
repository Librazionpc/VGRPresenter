#include "services/MediaLibraryService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "modules/media/MediaLibrary.hpp"
#include "modules/media/ThumbnailCache.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJSEngine>
#include <QQmlEngine>
#include <QThreadPool>
#include <QUrl>

namespace bm = bps::media;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// QML hands over either a plain path or a file:// URL (what a file dialog gives).
QString cleanPath(const QString &raw)
{
    QString p = raw.trimmed();
    if (p.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        p = QUrl(p).toLocalFile();
    return p;
}

void report(const QString &title, const QString &message, const QString &level = QStringLiteral("error"))
{
    EventBus::instance().notify(message, level, title, QStringLiteral("media.library"));
}

} // namespace

MediaLibraryService::MediaLibraryService(QObject *parent)
    : QObject(parent)
{
    // The engine's platform layer (paths, files, dialogs) exists once it has booted; the Media
    // tab is only ever created after that.
    if (!EngineBridge::instance().booted())
        return;
    auto &paths = bps::platform::PlatformAccessor::Get().Paths();
    auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    library_ = std::make_unique<bm::MediaLibrary>(fs.Join(paths.UserDataDir(), "media-folders.json"));
    thumbnails_ = std::make_unique<bm::ThumbnailCache>(fs.Join(paths.CacheDir(), "media-cache"),
                                                       bm::MakePlatformFrameSource());
    if (auto loaded = library_->Load(); !loaded.ok())
        report(tr("Media"), tr("The media folder list could not be read: %1").arg(qstr(loaded.error().message)));
    // The folder list is remembered; what is in the folders is read again on every start.
    for (const bm::LibraryFolder &f : library_->Folders())
        startScan(qstr(f.path));
}

MediaLibraryService::~MediaLibraryService() = default;

MediaLibraryService &MediaLibraryService::instance()
{
    static MediaLibraryService s;
    return s;
}

MediaLibraryService *MediaLibraryService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

int MediaLibraryService::frameSteps() const { return bm::ThumbnailCache::kFrameSteps; }

// ---------------------------------------------------------------------------
// Reading the engine's library
// ---------------------------------------------------------------------------

QVariantList MediaLibraryService::folders() const
{
    QVariantList out;
    if (!library_)
        return out;
    for (const bm::LibraryFolder &f : library_->Folders())
        out.append(QVariantMap{
            { QStringLiteral("path"), qstr(f.path) },
            { QStringLiteral("name"), qstr(f.name) },
            { QStringLiteral("count"), qulonglong(f.count) },
            { QStringLiteral("scanning"), scanning_.contains(qstr(f.path)) },
        });
    return out;
}

QVariantList MediaLibraryService::folderTree() const
{
    QVariantList rows;
    if (!library_)
        return rows;
    for (const bm::LibraryFolder &f : library_->Folders()) {
        const QString rootPath = qstr(f.path);
        const bool busy = scanning_.contains(rootPath);
        rows.append(QVariantMap{
            { QStringLiteral("path"), rootPath },
            { QStringLiteral("name"), qstr(f.name) },
            { QStringLiteral("depth"), 0 },
            { QStringLiteral("parent"), QString() },
            { QStringLiteral("count"), qulonglong(f.count) },
            { QStringLiteral("scanning"), busy },
            { QStringLiteral("root"), true },
            { QStringLiteral("hasChildren"), false },
        });
        // The engine lists a folder's subfolders in tree order; each one's parent is the last folder seen one
        // level up, which is all the nesting the UI needs.
        QStringList stack{ rootPath };   // stack[d] = the latest folder at depth d
        for (const bm::LibrarySubfolder &sub : library_->Subfolders(f.path)) {
            while (stack.size() > sub.depth)
                stack.removeLast();
            const QString path = qstr(sub.path);
            rows.append(QVariantMap{
                { QStringLiteral("path"), path },
                { QStringLiteral("name"), qstr(sub.name) },
                { QStringLiteral("depth"), sub.depth },
                { QStringLiteral("parent"), stack.isEmpty() ? rootPath : stack.last() },
                { QStringLiteral("count"), qulonglong(sub.count) },
                { QStringLiteral("scanning"), busy },
                { QStringLiteral("root"), false },
                { QStringLiteral("hasChildren"), false },
            });
            stack.append(path);
        }
    }
    // A row has children when the row after it is deeper.
    for (int i = 0; i + 1 < rows.size(); ++i) {
        QVariantMap row = rows[i].toMap();
        if (rows[i + 1].toMap().value(QStringLiteral("depth")).toInt() > row.value(QStringLiteral("depth")).toInt()) {
            row.insert(QStringLiteral("hasChildren"), true);
            rows[i] = row;
        }
    }
    return rows;
}

int MediaLibraryService::totalCount() const
{
    return library_ ? int(library_->Count()) : 0;
}

QVariantList MediaLibraryService::items(const QString &folderPath, const QString &query) const
{
    QVariantList out;
    if (!library_)
        return out;
    const std::vector<bm::LibraryItem> found = query.trimmed().isEmpty()
        ? library_->Items(folderPath.toStdString())
        : library_->Search(query.toStdString(), folderPath.toStdString());
    out.reserve(int(found.size()));
    for (const bm::LibraryItem &it : found)
        out.append(QVariantMap{
            { QStringLiteral("id"), qstr(it.id) },
            { QStringLiteral("name"), qstr(it.name) },
            { QStringLiteral("path"), qstr(it.path) },
            { QStringLiteral("url"), QUrl::fromLocalFile(qstr(it.path)).toString() },
            { QStringLiteral("kind"), it.kind == bm::MediaKind::Video ? QStringLiteral("video")
                                        : it.kind == bm::MediaKind::Audio ? QStringLiteral("audio")
                                                                          : QStringLiteral("image") },
            { QStringLiteral("folder"), qstr(it.folder) },
        });
    return out;
}

QString MediaLibraryService::thumbnailFile(const QString &path, int size, int step) const
{
    if (!thumbnails_)
        return {};
    const auto kind = bm::MediaKindOf(path.toStdString());
    if (!kind)
        return {};
    auto made = step >= 0 ? thumbnails_->Frame(path.toStdString(), step, size)
                          : thumbnails_->Still(path.toStdString(), *kind, size);
    if (made.ok())
        return qstr(made.value());

    // A picture the engine's own decoders cannot read (SVG, for one) may still be readable by one of
    // Qt's image plugins. Its result goes into the engine's cache under the engine's own name for it,
    // so the engine finds it there, fresh, the next time it is asked.
    if (*kind == bm::MediaKind::Image && step < 0 && made.error().code != bps::Err::NotFound) {
        const int target = bm::ThumbnailCache::RoundSize(size);
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            const QSize source = reader.size();
            if (source.isValid() && source.width() > target)
                reader.setScaledSize(QSize(target, qMax(1, target * source.height() / source.width())));
            const QImage image = reader.read();
            const QString out = qstr(thumbnails_->CachePath(path.toStdString(), target, -1));
            if (!image.isNull() && QDir().mkpath(QFileInfo(out).absolutePath()) && image.save(out, "PNG"))
                return out;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Changing it
// ---------------------------------------------------------------------------

bool MediaLibraryService::addFolder()
{
    if (!library_)
        return false;
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().SelectFolderDialog("Add a media folder");
    if (!picked.ok() || !picked.value())
        return false;
    return addFolderPath(qstr(*picked.value()));
}

bool MediaLibraryService::addFolderPath(const QString &raw)
{
    if (!library_)
        return false;
    const QString path = cleanPath(raw);
    if (auto added = library_->AddFolder(path.toStdString()); !added.ok()) {
        const bool duplicate = added.error().code == bps::Err::AlreadyExists;
        report(tr("Media"), qstr(added.error().message), duplicate ? QStringLiteral("info") : QStringLiteral("error"));
        return false;
    }
    startScan(path);
    emit foldersChanged();
    return true;
}

void MediaLibraryService::removeFolder(const QString &path)
{
    if (!library_)
        return;
    const QString name = QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName();
    // Its cached pictures stay in the cache: they are keyed by file, so they are reused if the
    // folder is added again, and the cache is not the library's to clean.
    if (auto removed = library_->RemoveFolder(path.toStdString()); !removed.ok()) {
        report(tr("Media"), qstr(removed.error().message));
        return;
    }
    scanning_.remove(path);
    emit foldersChanged();
    report(tr("Media"), tr("Removed “%1” from the library. Its files were not touched.").arg(name),
           QStringLiteral("info"));
}

void MediaLibraryService::rescan(const QString &path)
{
    if (!library_)
        return;
    if (path.isEmpty()) {
        for (const bm::LibraryFolder &f : library_->Folders())
            startScan(qstr(f.path));
        return;
    }
    startScan(path);
}

void MediaLibraryService::startScan(const QString &path)
{
    if (!library_)
        return;
    scanning_.insert(path);
    emit foldersChanged();

    // The engine's scan is safe on any thread (its slow part runs outside its lock).
    QThreadPool::globalInstance()->start([this, path] {
        auto scanned = library_->Scan(path.toStdString());
        const QString failure = scanned.ok() ? QString() : qstr(scanned.error().message);
        QMetaObject::invokeMethod(this, [this, path, failure] {
            scanning_.remove(path);
            emit foldersChanged();
            if (!failure.isEmpty() && !failure.contains(QLatin1String("not in the library")))
                report(tr("Media"), failure);
        }, Qt::QueuedConnection);
    });
}

void MediaLibraryService::shutdown()
{
    QThreadPool::globalInstance()->waitForDone(10000);
}
