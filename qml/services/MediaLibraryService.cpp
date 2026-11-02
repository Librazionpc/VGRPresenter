#include "services/CallTrace.h"
#include "services/MediaLibraryService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "modules/media/MediaLibrary.hpp"
#include "modules/media/ThumbnailCache.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QImageReader>
#include <QJSEngine>
#include <QQmlEngine>
#include <QThreadPool>
#include <QUrl>

#include <vector>

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

void report(const QString &title, const QString &message, const QString &level = QStringLiteral("error"),
            const QVariant &crux = QVariant(), const QVariant &action = QVariant())
{
    EventBus::instance().notify(message, level, title, QStringLiteral("media.library"), crux, action);
}

QString leafName(const QString &path)
{
    QString name = path;
    while (name.endsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('\\')))
        name.chop(1);
    name = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
    name = name.mid(name.lastIndexOf(QLatin1Char('\\')) + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        name = name.left(dot);
    return name.isEmpty() ? path : name;
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
            { QStringLiteral("fit"), qstr(it.fit) },
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
    QString name = path;
    while (name.endsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('\\')))
        name.chop(1);
    name = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
    name = name.mid(name.lastIndexOf(QLatin1Char('\\')) + 1);
    if (name.isEmpty())
        name = path;
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

void MediaLibraryService::renameItem(const QString &path, const QString &newName)
{
    editItem(path, ItemOp::Rename, newName.trimmed());
}

void MediaLibraryService::removeItem(const QString &path)
{
    editItem(path, ItemOp::Remove, QString());
}

void MediaLibraryService::removeItems(const QStringList &paths)
{
    VGR_CALL_TRACE("MediaLibraryService.removeItems");
    if (!library_)
        return;
    QStringList clean;
    for (const QString &p : paths)
        if (!p.trimmed().isEmpty() && !clean.contains(p))
            clean.append(p);
    if (clean.isEmpty())
        return;
    if (clean.size() == 1) {
        removeItem(clean.first());
        return;
    }
    QStringList names;
    for (const QString &p : clean)
        names.append(leafName(p));
    QThreadPool::globalInstance()->start([this, clean, names] {
        QStringList tokens;
        QString firstFailure;
        int failed = 0;
        for (const QString &p : clean) {
            if (auto trashed = library_->TrashItem(p.toStdString()); trashed.ok()) {
                tokens.append(qstr(trashed.value()));
            } else {
                ++failed;
                if (firstFailure.isEmpty())
                    firstFailure = qstr(trashed.error().message);
            }
        }
        QMetaObject::invokeMethod(this, [this, tokens, failed, firstFailure, names] {
            if (!tokens.isEmpty())
                emit foldersChanged();
            if (!tokens.isEmpty()) {
                const QString joined = tokens.join(QLatin1Char('\n'));
                QString message = tr("%1 files were deleted \u2014 Undo puts them back.")
                                      .arg(tokens.size());
                if (failed > 0)
                    message = tr("%1 of %2 files were deleted \u2014 Undo puts those back. %3")
                                  .arg(tokens.size()).arg(tokens.size() + failed)
                                  .arg(firstFailure);
                report(tr("Media"), message, failed > 0 ? QStringLiteral("warning") : QStringLiteral("info"),
                       QVariant(),
                       QVariantMap{{ QStringLiteral("id"), QStringLiteral("media.undoRemove.") + joined },
                                   { QStringLiteral("label"), tr("Undo") },
                                   { QStringLiteral("token"), joined }});
            } else if (!firstFailure.isEmpty()) {
                report(tr("Media"), firstFailure);
            }
        }, Qt::QueuedConnection);
    });
}

void MediaLibraryService::undoRemove(const QString &token)
{
    VGR_CALL_TRACE("MediaLibraryService.undoRemove");
    if (!library_ || token.isEmpty())
        return;
    const QStringList tokens = token.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    std::vector<std::string> keys;
    keys.reserve(size_t(tokens.size()));
    for (const QString &t : tokens)
        keys.push_back(t.toStdString());
    QThreadPool::globalInstance()->start([this, keys] {
        int restoredCount = 0;
        QString firstFailure;
        QString lastName;
        for (const std::string &key : keys) {
            auto restored = library_->RestoreTrashedItem(key);
            if (restored.ok()) {
                ++restoredCount;
                lastName = qstr(restored.value());
            } else if (firstFailure.isEmpty()) {
                firstFailure = qstr(restored.error().message);
            }
        }
        QMetaObject::invokeMethod(this, [this, restoredCount, firstFailure, lastName] {
            if (restoredCount > 0)
                emit foldersChanged();
            if (!firstFailure.isEmpty()) {
                report(tr("Media"), firstFailure);
                return;
            }
            report(tr("Media"), restoredCount == 1
                                      ? tr("Restored \u201c%1\u201d.").arg(leafName(lastName))
                                      : tr("%1 files were restored.").arg(restoredCount),
                   QStringLiteral("info"));
        }, Qt::QueuedConnection);
    });
}

void MediaLibraryService::moveItem(const QString &path, const QString &targetFolder)
{
    editItem(path, ItemOp::Move, targetFolder);
}

void MediaLibraryService::setItemFit(const QString &path, const QString &fit)
{
    editItem(path, ItemOp::Fit, fit);
}

QString MediaLibraryService::itemFit(const QString &path) const
{
    if (!library_ || path.isEmpty())
        return {};
    return qstr(library_->ItemFit(path.toStdString()));
}

void MediaLibraryService::editItem(const QString &path, ItemOp op, const QString &argument)
{
    VGR_CALL_TRACE("MediaLibraryService.editItem");
    if (!library_ || path.isEmpty())
        return;
    const std::string target = path.toStdString();
    const std::string arg = argument.toStdString();
    const QString name = leafName(path);   // for the delete's notice (see below)
    QThreadPool::globalInstance()->start([this, target, arg, op, name] {
        QString failure;
        QString token;   // the trash token of a delete — empty for a rename/move
        switch (op) {
        case ItemOp::Rename:
            if (auto r = library_->RenameItem(target, arg); !r.ok())
                failure = qstr(r.error().message);
            break;
        case ItemOp::Move:
            if (auto r = library_->MoveItem(target, arg); !r.ok())
                failure = qstr(r.error().message);
            break;
        case ItemOp::Fit:
            if (auto r = library_->SetItemFit(target, arg); !r.ok())
                failure = qstr(r.error().message);
            break;
        case ItemOp::Remove:
            // TRASHED, not destroyed: the notice below offers the Undo that puts
            // it back, so the token is what makes a delete takeable back.
            if (auto trashed = library_->TrashItem(target); !trashed.ok())
                failure = qstr(trashed.error().message);
            else
                token = qstr(trashed.value());
            break;
        }
        QMetaObject::invokeMethod(this, [this, failure, token, name] {
            if (!failure.isEmpty()) {
                report(tr("Media"), failure);
                return;
            }
            emit foldersChanged();
            if (token.isEmpty())
                return;
            // The Undo is this notice's own: the id carries the token, so a second
            // deletion on screen cannot steal the first one's button.
            report(tr("Media"), tr("\u201c%1\u201d was deleted \u2014 Undo puts it back.").arg(name),
                   QStringLiteral("info"), QVariant(),
                   QVariantMap{{ QStringLiteral("id"), QStringLiteral("media.undoRemove.") + token },
                               { QStringLiteral("label"), tr("Undo") },
                               { QStringLiteral("token"), token }});
        }, Qt::QueuedConnection);
    });
}

void MediaLibraryService::startScan(const QString &path)
{
    VGR_CALL_TRACE("MediaLibraryService.startScan");
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
    (void)QThreadPool::globalInstance()->waitForDone(2000);
}