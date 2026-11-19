#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <functional>
#include <memory>

#include "core/common/Common.hpp"
#include "core/threading/JobTicket.hpp"
#include "modules/presentation/PresentationTypes.hpp"

namespace bps::presentation { class ShowLibrary; }

class QJSEngine;
class QQmlEngine;

class ShowService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // ---- The working show ------------------------------------------------
    Q_PROPERTY(bool hasShow READ hasShow NOTIFY showChanged)
    Q_PROPERTY(bool showDirty READ showDirty NOTIFY showChanged)
    Q_PROPERTY(QString showPath READ showPath NOTIFY showChanged)
    Q_PROPERTY(QString showName READ showName NOTIFY showChanged)
    Q_PROPERTY(QVariantMap currentShow READ currentShow NOTIFY showChanged)

    // ---- The library -----------------------------------------------------
    // Folder the library lives in (default: Documents/VGR Presenter/Shows).
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY libraryChanged)
    Q_PROPERTY(QStringList libraryCategories READ libraryCategories NOTIFY libraryChanged)
    // Every show: { path, id, name, category, modifiedMs, slideCount }, newest first.
    Q_PROPERTY(QVariantList libraryShows READ libraryShows NOTIFY libraryChanged)
    // Files in the library that could not be read as shows ("<path>: <why>").
    Q_PROPERTY(QStringList libraryProblems READ libraryProblems NOTIFY libraryChanged)

public:
    static ShowService &instance();
    static ShowService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~ShowService() override;

    bool hasShow() const;
    bool showDirty() const;
    QString showPath() const;
    QString showName() const;
    QVariantMap currentShow() const;

    // Starts an empty, unsaved show ("New show"). The UI resets its own canvas;
    // this keeps the engine's document in step.
    Q_INVOKABLE void newShowDocument(const QString &name);
    // ---- THE QUEUED DOCUMENT JOBS (see the note below) -------------------
    Q_INVOKABLE int saveShowFile(const QVariantMap &show, const QString &path);
    Q_INVOKABLE int saveCurrentShow(const QString &path = QString(), bool quiet = false);
    // Writes the show as it is now to `path` WITHOUT making that its file (backups and the crash-recovery copy): the show's
    // own path and unsaved state are left alone, and nothing is announced.
    Q_INVOKABLE int saveShowCopy(const QString &path);
    // Makes sure a working show exists (creating an empty one named `name`).
    Q_INVOKABLE void ensureShow(const QString &name);
    // Native file pickers (engine platform layer). "" when the user cancels.
    Q_INVOKABLE QString pickShowToOpen();   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    Q_INVOKABLE QString pickShowSavePath(const QString &defaultName);   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    Q_INVOKABLE bool cancelDocumentJob(int token);
    Q_INVOKABLE int cancelDocumentJobs(const QStringList &kinds = QStringList());
    Q_INVOKABLE int openShowFile(const QString &path);
    Q_INVOKABLE QVariantMap peekShow(const QString &path) const;
    Q_PROPERTY(int peekRevision READ peekRevision NOTIFY peekRevisionChanged)
    int peekRevision() const { return peekRevision_; }
    // The next timer (FreeShow's): every slide of the show at `path` moves on by itself after `seconds` (0 clears it). A show that is
    // open in the editor takes it as an unsaved edit; any other is changed in its file.
    Q_INVOKABLE int setNextTimer(const QString &path, double seconds);

    Q_INVOKABLE int editSlides(const QString &path, const QString &op, const QStringList &slideIds, const QVariantMap &args = {});
    // The UI edited the show — drives showDirty (the modified marker and the
    // "save changes?" prompt).
    Q_INVOKABLE void markShowDirty();
    Q_INVOKABLE void markShowClean();   // nothing worth saving yet (a fresh show's first empty slide)

    // ---- Editing the show: create / update / delete / duplicate ----------------
    Q_INVOKABLE QString addCategory(const QVariantMap &category);
    // `patch` may hold any of: name, contentType, templateId, outputs, meta -- only
    // the keys present change.
    Q_INVOKABLE bool updateCategory(const QString &id, const QVariantMap &patch);
    // Its slides become uncategorised; overlays scoped to it are removed with it.
    Q_INVOKABLE bool removeCategory(const QString &id);
    Q_INVOKABLE bool moveCategory(const QString &id, int index);
    Q_INVOKABLE bool assignCategoryTemplate(const QString &categoryId, const QString &templateId);
    Q_INVOKABLE bool setSlideCategory(const QString &slideId, const QString &categoryId);
    // Templates:
    Q_INVOKABLE QString addTemplate(const QVariantMap &tmpl);
    Q_INVOKABLE bool updateTemplate(const QString &id, const QVariantMap &tmpl);
    // Refused while categories still use it, unless `force` (they lose it).
    Q_INVOKABLE bool removeTemplate(const QString &id, bool force = false);
    Q_INVOKABLE QString duplicateTemplate(const QString &id);
    // Overlays:
    Q_INVOKABLE QString addOverlay(const QVariantMap &overlay);
    Q_INVOKABLE bool updateOverlay(const QString &id, const QVariantMap &overlay);
    Q_INVOKABLE bool removeOverlay(const QString &id);
    Q_INVOKABLE bool setOverlayEnabled(const QString &id, bool enabled);
    // Slides:
    Q_INVOKABLE QString addSlide(const QVariantMap &slide, int index = -1);
    Q_INVOKABLE bool updateSlide(const QString &id, const QVariantMap &slide);
    Q_INVOKABLE QVariantMap slideOf(const QString &id) const;        // {} if unknown
    Q_INVOKABLE QVariantMap blockOf(const QString &slideId, const QString &blockId) const;
    Q_INVOKABLE bool removeSlide(const QString &id);      // its scoped overlays go with it
    Q_INVOKABLE bool moveSlide(const QString &id, int index);
    Q_INVOKABLE QString duplicateSlide(const QString &id);   // placed right after the original
    // Content blocks on a slide (what the canvas edits):
    Q_INVOKABLE QString addBlock(const QString &slideId, const QVariantMap &block, int index = -1);
    Q_INVOKABLE bool updateBlock(const QString &slideId, const QString &blockId, const QVariantMap &block);
    Q_INVOKABLE bool removeBlock(const QString &slideId, const QString &blockId);
    Q_INVOKABLE bool moveBlock(const QString &slideId, const QString &blockId, int index);
    Q_INVOKABLE QString duplicateBlock(const QString &slideId, const QString &blockId);   // offset copy above

    // ---- Categories, templates, overlays at work --------------------------
    Q_INVOKABLE QVariantMap resolveSlide(const QVariantMap &show, const QString &slideId,
                                         const QString &outputId = QString());
    // Problems with the show's structure (a category pointing at a missing
    // template, an overlay targeting a deleted slide...). Empty = consistent.
    Q_INVOKABLE QStringList validateShow(const QVariantMap &show);
    // Slide templates as their own .vgr files. Both queued (see above); the load's
    // verdict carries { ok, error, template }.
    Q_INVOKABLE int saveTemplateFile(const QVariantMap &tmpl, const QString &path);
    Q_INVOKABLE int loadTemplateFile(const QString &path);

    // ---- Library ----------------------------------------------------------
    QString libraryPath() const { return libraryPath_; }
    void setLibraryPath(const QString &path);
    QStringList libraryCategories() const { return categories_; }
    QVariantList libraryShows() const { return shows_; }
    QStringList libraryProblems() const { return problems_; }

    Q_INVOKABLE void refreshLibrary();
    Q_INVOKABLE QVariantList libraryShowsIn(const QString &category) const;   // "" = uncategorised
    Q_INVOKABLE QVariantList searchLibrary(const QString &text) const;
    Q_INVOKABLE bool createLibraryCategory(const QString &name);
    Q_INVOKABLE bool renameLibraryCategory(const QString &from, const QString &to);
    Q_INVOKABLE bool removeLibraryCategory(const QString &name);              // keeps shows in Unlabeled
    // Moves every show in `name` to the recoverable .deleted bin (a re-import
    // calls this to replace the previous batch). True when the folder is clear.
    Q_INVOKABLE bool clearLibraryCategory(const QString &name);
    bool clearLibraryCategoryNoPublish(const QString &name);
    // Moves a show file into `category` ("" = out of every category); returns its
    // new path, or "" on failure (toast explains).
    Q_INVOKABLE QString moveShowToCategory(const QString &path, const QString &category);
    // Show CRUD in the library -- the engine does the file work.
    // Renames the show (its stored name and its file); returns the new path or "".
    Q_INVOKABLE QString renameShow(const QString &path, const QString &newName);
    // A copy with its own identity in the same category ("<name> copy"); returns its path.
    Q_INVOKABLE QString duplicateShow(const QString &path, const QString &newName = QString());
    // Recoverable: moves the file into the library's .deleted folder; returns where.
    Q_INVOKABLE QString deleteShow(const QString &path);
    Q_INVOKABLE bool emptyDeletedShows();   // removes the .deleted folder for good
    // A free .vgr path for a new show called `name` in `category` — sanitised
    // file name, never an existing file. "" on failure.
    Q_INVOKABLE QString newLibraryShowPath(const QString &category, const QString &name);
    // The path of a show already in `category` with this show's name AND exactly its words ("" = none):
    // an import that finds one skips the write instead of adding "Name (2).vgr". Opens only the few
    // same-named candidates, never the library.
    QString findSameLibraryShow(const QString &category, const bps::presentation::Presentation &show);
    // The show name to save under: `name`, or "name (2)", "name (3)"... when that name is taken in `category`.
    QString numberedLibraryShowName(const QString &category, const QString &name);
    // Finds shows that exist more than once (same name, same words) and moves the extra copies to the
    // library's .deleted bin, keeping one of each. Runs on a worker; a toast reports the result.
    Q_INVOKABLE void removeDuplicateShows();

    Q_INVOKABLE int rewriteShowFromText(const QString &path, const QString &text);
    // Quick edit for a slide's existing text blocks. Other blocks (images,
    // shapes, layout and style) stay as-authored in the show file.
    Q_INVOKABLE int updateShowTextItems(const QString &path, const QString &slideId,
                                        const QVariantMap &textByBlockId);
    // Applies a whole-show Text edit in one worker-thread document transaction.
    // Each row has { slideId, textByBlockId, newItems, fontFamily }.
    Q_INVOKABLE int applyTextEditBatch(const QVariantList &slideEdits);

signals:
    void showChanged();
    void libraryChanged();
    void documentJobFinished(int token, const QString &kind, bool ok,
                             const QString &error, const QVariantMap &payload);
    // A peeked show landed (or was dropped because this app wrote its file):
    // peekShow()'s callers re-read. The centre page binds to it.
    void peekRevisionChanged();
    void documentJobCancelled(int token, const QString &kind);

private:
    explicit ShowService(QObject *parent = nullptr);
    bps::presentation::ShowLibrary *library();
    // ---- THE QUEUED DOCUMENT JOBS (see the public note) ------------------
    struct JobResult {
        bool ok = false;
        QString title;                  // the toast's title when !ok
        QString error;                  // "" when ok (or when there is nothing to say)
        QVariantMap payload;             // the op's own fields (see the header)
        QString logLine;                 // the engine log's one line, written on landing
        QString savedToast;              // non-empty: announce "Saved to <this>"
        QString touchedPath;             // a file this job WROTE: its published peek goes
        bool documentChanged = false;    // emit showChanged() when it lands
        bool libraryChanged = false;     // refreshLibrary() when it lands
        bool openDocumentEdited = false;  // an EDIT of the working show: the centre
                                         // page's peek of it is stale now
    };
    struct JobContext {
        bool booted = false;
        QString libraryRoot;   // "" = this run has no library object yet
        bool inLibrary(const QString &path) const;
    };
    JobContext jobContext() const;
    enum class JobDurability { Durable, Abandonable };
    struct LiveJob {
        QString kind;
        JobDurability durability = JobDurability::Abandonable;
        bps::JobTicketPtr ticket;
    };
    int runDocumentJobOffThread(const QString &kind, JobDurability durability,
                                const std::function<JobResult()> &work);
    void applyJobState(const JobResult &r);
    void finishDocumentJob(const QString &kind, const JobResult &r);
    JobResult applyEditOutcome(const QString &title,
                               const std::function<bps::Result<void>(bps::presentation::Presentation &)> &fn) const;
    JobResult editShowOutcome(const JobContext &ctx, const QString &path, const QString &title,
                              const std::function<bps::Result<void>(bps::presentation::Presentation &)> &change) const;
    // The slide menu's whole dispatch (editSlides), answering instead of
    // reporting: one op runs, and its outcome is what the verdict carries.
    JobResult editSlidesOutcome(const JobContext &ctx, const QString &path, const QString &op,
                                const QStringList &slideIds, const QVariantMap &args) const;
    // Finishes an outcome that was produced ON THIS THREAD (the wrapper path).
    bool finishHere(const JobResult &r);
    // Runs one engine edit on the working show (atomic), then announces the change or
    // reports why it was refused.
    bool applyEdit(const QString &title,
                   const std::function<bps::Result<void>(bps::presentation::Presentation &)> &fn);
    QString applyEditForId(const QString &title,
                           const std::function<bps::Result<std::string>(bps::presentation::Presentation &)> &fn);
    void reportError(const QString &title, const QString &message) const;
    QVariantMap peekAnswer(const QString &key) const;   // the read itself (worker)
    void peekOffThread(const QString &key) const;
    void dropPeek(const QString &key) const;
    void dropPeekForOpenPath() const;
    void publishPeek(const QString &key, const QVariantMap &answer) const;
    void publishLibrary();

    QString libraryPath_;
    std::shared_ptr<bps::presentation::ShowLibrary> library_;
    QStringList categories_;
    QVariantList shows_;
    QStringList problems_;
    // The published peeked reads (see peekShow): keyed by the cleaned path.
    mutable QHash<QString, QVariantMap> peekCache_;
    mutable QSet<QString> peekInFlight_;
    mutable int peekRevision_ = 0;
    // The queued document jobs' token source (interface thread only).
    int jobToken_ = 0;
    QHash<int, LiveJob> jobs_;
    // Library scan state (GUI-thread owned): one filesystem pass at a time, and
    // a rescan that arrives while one is in flight runs after it.
    bool refreshBusy_ = false;
    bool cachedListPending_ = false;   // the persisted index was loaded and not yet shown
    bool refreshPending_ = false;
    bool dedupeBusy_ = false;
};