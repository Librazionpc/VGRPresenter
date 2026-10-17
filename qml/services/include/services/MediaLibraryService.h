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
    // The library lists files where they lie, so every one of these is a REAL
    // filesystem operation on the user's disk — and each therefore runs on the
    // worker pool and reports by emitting foldersChanged() once it has landed,
    // exactly like a scan (never on the interface thread: a name on a share that
    // has gone away would park the window). A failure is a toast and the grid
    // keeps showing what is still on disk. A rename or a move is its own
    // confirmation (the grid shows the result); a DELETE is the one that can be
    // taken back, so the confirmation belongs to the caller (`path` is the item's
    // own path).
    //
    // Renames `path` to `newName`: a NAME, not a path, and the old extension is
    // kept when `newName` carries none.
    Q_INVOKABLE void renameItem(const QString &path, const QString &newName);
    // Deletes the file by moving it into the library's trash, and raises the
    // notice that offers the Undo — the file is not destroyed, so the operator's
    // one-click answer is enough of a confirmation. `undoRemove(token)` puts it
    // back; the token rides the notice's own action ("action.token"), which is
    // what lets two deletions on screen each undo THEIR OWN file.
    Q_INVOKABLE void removeItem(const QString &path);
    // The same delete for SEVERAL files (the grid's multi-selection): every file is
    // trashed on the worker, and the batch raises ONE notice with ONE Undo that puts
    // the whole selection back — N files must not mean N notices. A file that could
    // not go is counted and named in the notice; the rest still go.
    Q_INVOKABLE void removeItems(const QStringList &paths);
    // Puts a deleted file back where it came from (the token from the notice), or
    // every file of a batch when the token is the batch's own (tokens joined with a
    // newline — a path never contains one). A failure — the name has been taken
    // again, or this run no longer holds the deletion — is a toast, and changes
    // nothing.
    Q_INVOKABLE void undoRemove(const QString &token);
    // Moves the file into `targetFolder`, which must be a library folder or a
    // folder inside one (its row path from folderTree).
    Q_INVOKABLE void moveItem(const QString &path, const QString &targetFolder);

    // ---- THIS FILE'S OWN FILL (Contain / Cover / Fill) -----------------------
    // A choice made on the FILE, not on an output: it wins over the output's own
    // Fill (Settings · Outputs → Fill) wherever that file is shown. `fit` is
    // "contain", "cover", "fill", or "" for "whatever the output says". Kept
    // beside the index rather than in it, so a rescan can never lose it.
    // Runs on the worker like the other file operations; foldersChanged() says
    // when it landed, so every card and the on-air take re-read it.
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
    // The one worker hop behind the three Q_INVOKABLEs above: the engine call runs
    // on the pool, its verdict comes back on this thread, and the pane re-reads
    // through foldersChanged() when it landed.
    void editItem(const QString &path, ItemOp op, const QString &argument);

    std::unique_ptr<bps::media::MediaLibrary> library_;
    std::unique_ptr<bps::media::ThumbnailCache> thumbnails_;
    QSet<QString> scanning_;
};
