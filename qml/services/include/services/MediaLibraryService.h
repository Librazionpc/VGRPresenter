#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::media {
class MediaLibrary;
class ThumbnailCache;
}

class QQmlEngine;
class QJSEngine;

class MediaLibraryService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // [{ path, name, count, scanning }] in the order they were added.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList folderTree READ folderTree NOTIFY foldersChanged)
    // Files found across every folder.
    Q_PROPERTY(int totalCount READ totalCount NOTIFY foldersChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY foldersChanged)
    // How many moving frames a video has (0..frameSteps-1).
    Q_PROPERTY(int frameSteps READ frameSteps CONSTANT)

public:
    static MediaLibraryService &instance();
    static MediaLibraryService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~MediaLibraryService() override;

    QVariantList folders() const;
    QVariantList folderTree() const;
    int totalCount() const;
    bool scanning() const { return !scanning_.isEmpty(); }
    int frameSteps() const;

    // Asks for a folder with the native folder picker, then adds it. false if cancelled or
    // refused (a toast says why).
    Q_INVOKABLE bool addFolder();   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    // Adds `path` (a folder path or file:// URL). false + a toast if it isn't a folder or is
    // already in the library.
    Q_INVOKABLE bool addFolderPath(const QString &path);
    // Forgets a folder (its files are untouched).
    Q_INVOKABLE void removeFolder(const QString &path);
    // Scans again: one folder, or every folder when `path` is empty.
    Q_INVOKABLE void rescan(const QString &path = QString());
    // The files of one folder, or of every folder when `folderPath` is empty. With `query`, only the
    // files whose NAME matches it (the engine's search: every word, any order, best first).
    Q_INVOKABLE QVariantList items(const QString &folderPath = QString(), const QString &query = QString()) const;

    // ---- the FILES themselves: rename, delete, move ---------------------------
    Q_INVOKABLE void renameItem(const QString &path, const QString &newName);
    Q_INVOKABLE void removeItem(const QString &path);
    Q_INVOKABLE void removeItems(const QStringList &paths);
    Q_INVOKABLE void undoRemove(const QString &token);
    // Moves the file into `targetFolder`, which must be a library folder or a
    // folder inside one (its row path from folderTree).
    Q_INVOKABLE void moveItem(const QString &path, const QString &targetFolder);

    // ---- THIS FILE'S OWN FILL (Contain / Cover / Fill) -----------------------
    Q_INVOKABLE void setItemFit(const QString &path, const QString &fit);
    // The stored fit for `path` ("" when it has none): an in-memory read, cheap
    // enough for the interface thread and for the take path.
    Q_INVOKABLE QString itemFit(const QString &path) const;

    // The cached picture file for the image provider - any thread, may block while the engine
    // decodes it. "" when there is none (missing file, no decoder for it).
    QString thumbnailFile(const QString &path, int size, int step) const;

    // Waits for scans in progress; call before the engine shuts down.
    void shutdown();

signals:
    void foldersChanged();

private:
    explicit MediaLibraryService(QObject *parent = nullptr);
    void startScan(const QString &path);
    // Which file operation editItem() runs on the pool.
    enum class ItemOp { Rename, Move, Remove, Fit };
    void editItem(const QString &path, ItemOp op, const QString &argument);

    std::unique_ptr<bps::media::MediaLibrary> library_;
    std::unique_ptr<bps::media::ThumbnailCache> thumbnails_;
    QSet<QString> scanning_;
};