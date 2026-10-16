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

// The UI's window onto SHOWS — the working show document and its .vgr file,
// slide templates, and the show library. Everything real lives in the engine
// (PresentationDocument, SlideResolver, TemplateFile, ShowLibrary); this service
// only converts between QML shapes (see ShowConverter.h) and engine types and
// turns engine errors into toasts. QML never touches files or engine types.
//
// (Engine lifecycle, undo and devices are EngineBridge's job; this service needs
// the engine booted and does nothing useful before that.)
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
    // The engine's copy of the working show as a QML object (see ShowConverter.h).
    // The engine is the source of truth for the show's structure: every edit below
    // goes through it, and this property re-reads after each one.
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
    //
    // EVERY show-document read or write below is handed to the engine worker and
    // answers with a TOKEN. The verdict arrives on the interface thread as
    // documentJobFinished(token, kind, ok, error, payload) — match the token to
    // the call that returned it (`kind` is "save", "saveCopy", "open", "timer",
    // "slides", "templateSave", "templateLoad", "rewrite").
    //
    // WHY THIS IS SAFE. The engine's document API is built for exactly this:
    // PresentationDocument::Save takes the snapshot and the model's revision under
    // its own lock, releases the lock for the write, and re-checks the revision
    // before clearing the dirty flag — "rather than silently marking those edits
    // saved", i.e. an edit that lands while the file is being written leaves the
    // show DIRTY. So moving these calls off the interface thread changes nothing
    // about their correctness; it changes who waits. The file itself is written
    // atomically (one complete .vgr on disk at every instant), so even a save cut
    // short by exit cannot damage the previous file.
    //
    // WHAT THE CALLER OWES THE UI. The call returns before the file is touched, so
    // a caller that must not continue until the write landed (ShowSession's
    // "save then close", the recovery copy, the centre page's next timer) waits
    // for its token instead of reading a return value. Nothing in QML may assume
    // the file exists the moment one of these returns.
    //
    // WHAT CAN BE TAKEN BACK. A job still in flight can be ABANDONED
    // (cancelDocumentJob / cancelDocumentJobs): the screen that asked is gone, or
    // another show was opened, so nobody wants the answer. What that does to the
    // WORK depends on the job (JobDurability below): a job that only READS never
    // runs if it had not started, while a job that WRITES a file the operator
    // asked for still writes it and loses only its VERDICT (no toast, no
    // documentJobFinished) — cancelling must never be a way to lose an edit.
    // Either way the answer stops coming, and the poster hears that as
    // documentJobCancelled(token, kind) so a caller waiting on a token stops
    // waiting instead of hanging on a verdict that will never arrive.
    // Saves `show` to `path` (a file path or file:// URL; "" = the file it was
    // opened from / last saved to). Verified + atomic — a failure never damages
    // the previous file; a failure raises an error toast when the verdict lands.
    Q_INVOKABLE int saveShowFile(const QVariantMap &show, const QString &path);
    // Saves the engine's own copy of the show (what the edit calls below maintain).
    // "" = the file it was opened from / last saved to. Prefer this to saveShowFile
    // once the show is edited through the engine — it cannot drop engine-side data.
    // `quiet` skips the "Show saved" toast (autosave: a failure is still reported).
    Q_INVOKABLE int saveCurrentShow(const QString &path = QString(), bool quiet = false);
    // Writes the show as it is now to `path` WITHOUT making that its file (backups and the crash-recovery copy): the show's
    // own path and unsaved state are left alone, and nothing is announced.
    Q_INVOKABLE int saveShowCopy(const QString &path);
    // Makes sure a working show exists (creating an empty one named `name`).
    Q_INVOKABLE void ensureShow(const QString &name);
    // Native file pickers (engine platform layer). "" when the user cancels.
    Q_INVOKABLE QString pickShowToOpen();   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    Q_INVOKABLE QString pickShowSavePath(const QString &defaultName);   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    // Abandons an in-flight document job by the token its call returned. True when
    // it was still in flight, i.e. this call is what stopped it from reporting (see
    // the note above for what is left of the work). False = it had already ended.
    Q_INVOKABLE bool cancelDocumentJob(int token);
    // The same for every in-flight job whose kind is in `kinds` (empty = every
    // kind): the "the screen that asked for these is gone" call. Returns how many
    // were still in flight. Kinds are the ones the note above lists.
    Q_INVOKABLE int cancelDocumentJobs(const QStringList &kinds = QStringList());
    // Opens a .vgr show. The verdict carries { ok, error, show } in its payload;
    // on failure the current document is untouched and `error` says why (missing,
    // damaged, wrong type).
    Q_INVOKABLE int openShowFile(const QString &path);
    // Reads a show file WITHOUT opening it: the open show, its unsaved changes and
    // its path are left alone (the centre page shows a project's show this way).
    //
    // PUBLISHED, like ScriptureService's corpus reads (see its header): the answer
    // is a worker's, kept here keyed by path, and this is a LOOKUP — it queues the
    // read and returns what is known (a `{ ok: false, pending: true }` map while
    // one is in flight). What gets the caller its answer is `peekRevisionChanged`,
    // which bumps when a read lands and whenever this app WRITES that path (so the
    // centre page never keeps showing a show the next-timer just changed).
    Q_INVOKABLE QVariantMap peekShow(const QString &path) const;
    Q_PROPERTY(int peekRevision READ peekRevision NOTIFY peekRevisionChanged)
    int peekRevision() const { return peekRevision_; }
    // The next timer (FreeShow's): every slide of the show at `path` moves on by itself after `seconds` (0 clears it). A show that is
    // open in the editor takes it as an unsaved edit; any other is changed in its file.
    Q_INVOKABLE int setNextTimer(const QString &path, double seconds);

    // What FreeShow's slide menu does, on the slides `slideIds` of the show at `path` (the open show takes it as an unsaved edit, any
    // other is changed in its file). `op`:
    //   "hidden"      { hidden: bool }                 disable / enable
    //   "group"       { group: "verse"... | "" }      change group
    //   "title"       { title }                        rename (the label under the slide)
    //   "format"      { kind: "uppercase"|"lowercase"|"capitalize"|"trim" }
    //   "replace"     { find, replace, caseSensitive }  -> { replaced }
    //   "split"                                        cut in half -> { made: [ids] }
    //   "merge"                                        the later slides join the first
    //   "duplicate"                                    -> { made: [ids] }
    //   "remove"                                       delete
    //   "transition"  { kind: "fade"|"slide"|..., ms }
    //   "outputs"     { outputs: [ids] }               specific outputs (empty = all)
    //   "timer"       { seconds }                      next timer (0 clears)
    // The verdict's payload carries { ok, error, made, replaced }.
    Q_INVOKABLE int editSlides(const QString &path, const QString &op, const QStringList &slideIds, const QVariantMap &args = {});
    // The UI edited the show — drives showDirty (the modified marker and the
    // "save changes?" prompt).
    Q_INVOKABLE void markShowDirty();
    Q_INVOKABLE void markShowClean();   // nothing worth saving yet (a fresh show's first empty slide)

    // ---- Editing the show: create / update / delete / duplicate ----------------
    // The ENGINE does all of it (ShowEditor): it validates, generates ids, applies
    // the cascades and refuses anything that would leave the show inconsistent --
    // the UI only asks. Each call is atomic (a refusal changes nothing), marks the
    // show modified, and reports failures as an error toast. Ids come back as the
    // return value ("" on failure); `index` -1 means "at the end". After any of
    // these, read the result from `currentShow`.
    //
    // Categories (songs / notes / pastor notes...) and their templates:
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
    // PATCH semantics: only the keys present in `slide` change (title, tag, tagColor,
    // line1, line2, ref, background, categoryId, blocks) — everything else the
    // engine stores on the slide (notes, transitions, tags, ...) is left alone. This
    // is how a UI syncs its edited slide content into the engine safely.
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
    // What `slideId` looks like on `outputId` ("" = the editor, no filtering):
    // its category's template applied, per-slide overrides, and the overlays
    // in scope. Returns { slideId, categoryId, templateId, visible, background,
    // blocks, overlayBlocks }; { visible: false } when the slide's category is
    // not shown on that output; {} if the slide is not in `show`.
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
    Q_INVOKABLE bool removeLibraryCategory(const QString &name);              // only when empty
    // Moves every show in `name` to the recoverable .deleted bin (a re-import
    // calls this to replace the previous batch). True when the folder is clear.
    Q_INVOKABLE bool clearLibraryCategory(const QString &name);
    // Same clear WITHOUT republishing the QML-facing lists — for calls from a
    // worker thread (the async import sweep). publishLibrary() writes the
    // service's QList members and emits, which must stay on the GUI thread;
    // the import ends with one refreshLibrary() there anyway.
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

    // Live-edit autosave for the Show screen's lyrics view (FreeShow's own
    // TextEditor.svelte behavior): `text` is the WHOLE show's lyrics as one
    // blank-line-separated block (same convention Quick Lyrics uses) — this
    // re-splits it into sections/slides via the engine's own song-text
    // splitter and OVERWRITES the show's slides with the result, keeping
    // its name/category/other metadata untouched, then saves straight to
    // its own file (no open-document/editing-session involved, so it can't
    // collide with whatever the Edit screen has open elsewhere) and emits
    // showChanged() so any preview reading this path picks it up. Slide ids
    // regenerate on every edit (no FreeShow-style per-slide diffing yet) —
    // fine for lyrics text; per-slide custom styling would not survive a
    // re-edit today. A failure toasts why when the verdict lands.
    Q_INVOKABLE int rewriteShowFromText(const QString &path, const QString &text);

signals:
    void showChanged();
    void libraryChanged();
    // A queued document job's verdict (see the note above). `token` is the value
    // the call returned; `payload` carries that op's own fields (see each entry
    // point). `ok` false always means the file was not written / the show was not
    // opened, and `error` says why.
    void documentJobFinished(int token, const QString &kind, bool ok,
                             const QString &error, const QVariantMap &payload);
    // A peeked show landed (or was dropped because this app wrote its file):
    // peekShow()'s callers re-read. The centre page binds to it.
    void peekRevisionChanged();
    // A queued job's verdict is NOT coming: it was abandoned (see
    // cancelDocumentJob) before it could report — either it never ran, or its
    // verdict was withdrawn after the work had already happened (a Durable job).
    // Whoever was waiting on `token` must stop waiting. This is a signal of its own
    // rather than a documentJobFinished with ok=false, which says "the op failed,
    // and here is why" — and would be a lie about a job that was cancelled
    // precisely because nobody wants to hear about it.
    void documentJobCancelled(int token, const QString &kind);

private:
    explicit ShowService(QObject *parent = nullptr);
    bps::presentation::ShowLibrary *library();
    // ---- THE QUEUED DOCUMENT JOBS (see the public note) ------------------
    // What one job's engine work found: everything the interface thread needs to
    // finish it (the toast, the signals, the library sweep). A job never touches
    // QML-facing state or emits from the worker — it fills this in.
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
    // Everything a job needs that lives on the INTERFACE thread, read before it is
    // posted: EngineWorker's rule is that a lambda captures BY VALUE, and neither
    // the bridge's boot flag nor the library pointer may be re-read from another
    // thread (setLibraryPath can reset the latter).
    struct JobContext {
        bool booted = false;
        QString libraryRoot;   // "" = this run has no library object yet
        bool inLibrary(const QString &path) const;
    };
    JobContext jobContext() const;
    // Whether a job's WORK survives an abandonment. Not a preference — what the
    // job does to the world decides it:
    //   * Durable     — it writes a file the operator asked for (save, saveCopy,
    //                   rewrite, templateSave, and the timer/slide edits that
    //                   change another show's file). Cancelling one takes the
    //                   verdict and leaves the write, because the file is the point
    //                   and a screen closing must not be able to lose their edit.
    //   * Abandonable — it only reads (open, templateLoad). With nobody waiting for
    //                   the answer, the work itself is pointless, so an abandoned
    //                   job that has not started never touches the disk.
    enum class JobDurability { Durable, Abandonable };
    // One in-flight queued job, by the token its call returned (interface thread
    // only). Held for as long as the job can still report anything: a cancellation
    // does not remove it, because a Durable job's work outlives its verdict and
    // its landing is what applies the file's consequences.
    struct LiveJob {
        QString kind;
        JobDurability durability = JobDurability::Abandonable;
        bps::JobTicketPtr ticket;
    };
    // Queues `work` on the engine worker and returns its token; the verdict lands
    // on the interface thread through finishDocumentJob. The name says what it
    // does and is what makes the hand-off visible to
    // tools/check_gui_thread_blocks.mjs — a hop-named helper is that guard's
    // recognized signal that the blocking call inside it is not running on the
    // interface thread (the same shape SettingsService's and EngineBridge's own
    // ...OffThread helpers use).
    int runDocumentJobOffThread(const QString &kind, JobDurability durability,
                                const std::function<JobResult()> &work);
    // The consequences of a job that RAN, applied whether or not its verdict is
    // still wanted: a written file must show up in the library even when the screen
    // that asked has closed (a withdrawn verdict must not leave stale state).
    void applyJobState(const JobResult &r);
    void finishDocumentJob(const QString &kind, const JobResult &r);
    // The ONE body each edit has, answering instead of reporting: the
    // interface-thread entry points below are thin wrappers over these (so the
    // queued and the direct path can never drift apart).
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
    // (editShow's body is editShowOutcome above; nothing calls a synchronous
    // version any more — the two entry points that used it queue their jobs.)
    // The published peek (peekShow): a miss queues the read off-thread, an answer
    // publishes it and bumps peekRevision, and a write to that path drops it.
    QVariantMap peekAnswer(const QString &key) const;   // the read itself (worker)
    void peekOffThread(const QString &key) const;
    void dropPeek(const QString &key) const;
    void dropPeekForOpenPath() const;
    void publishPeek(const QString &key, const QVariantMap &answer) const;
    void publishLibrary();

    QString libraryPath_;
    // shared_ptr, not unique_ptr: refreshLibrary() hands a copy to a worker so
    // the scan can outlive a library-path change (setLibraryPath resets the
    // member) without the worker's pointer dangling.
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
    // The jobs still in flight, by token (interface thread only): the ticket is
    // what cancelDocumentJob abandons with, and the two flags outlive a cancel
    // because a Durable job's work lands after its verdict is gone.
    QHash<int, LiveJob> jobs_;
    // Library scan state (GUI-thread owned): one filesystem pass at a time, and
    // a rescan that arrives while one is in flight runs after it.
    bool refreshBusy_ = false;
    bool refreshPending_ = false;
};
