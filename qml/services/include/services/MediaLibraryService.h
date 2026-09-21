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

// The UI's window onto the MEDIA LIBRARY - the folders of images, videos and audio the user has
// added and the pictures made of them. The ENGINE does the work (bps::media::MediaLibrary:
// the folder list, scanning, item ids; bps::media::ThumbnailCache: the on-disk thumbnails
// with FreeShow's caching rules); this service only turns engine results into QML shapes,
// runs scans on a worker thread, and reports errors as toasts.
//
// Adding a folder is how media is brought in. Nothing is copied or moved; removing a folder
// only forgets it.
//
// Every item is { id, name, path, url, kind ("image" | "video" | "audio"), folder }. Its pictures are
// asked for through the image provider (MediaThumbnailProvider):
//   image://mediathumb/<size>/<step>/<percent-encoded path>
// where step -1 is the still (a video's middle frame) and 0..frameSteps-1 are a video's
// moving frames (the UI shows the one under the mouse as it moves across a tile).
class MediaLibraryService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // [{ path, name, count, scanning }] in the order they were added.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    // The folders as a TREE, in display order: every library folder followed by the folders inside it (at any
    // depth, each followed by what is inside it). Each row: { path, name, depth (0 = a library folder), parent,
    // count (files in it and below), scanning, root, hasChildren }. A library folder shows everything beneath it;
    // a nested folder opens on its own.
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
    Q_INVOKABLE bool addFolder();
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

    std::unique_ptr<bps::media::MediaLibrary> library_;
    std::unique_ptr<bps::media::ThumbnailCache> thumbnails_;
    QSet<QString> scanning_;
};
