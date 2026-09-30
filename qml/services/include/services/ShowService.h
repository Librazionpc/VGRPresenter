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
    // Saves `show` to `path` (a file path or file:// URL; "" = the file it was
    // opened from / last saved to). Verified + atomic — a failure never damages
    // the previous file. Returns false and raises an error toast on failure.
    Q_INVOKABLE bool saveShowFile(const QVariantMap &show, const QString &path);
    // Saves the engine's own copy of the show (what the edit calls below maintain).
    // "" = the file it was opened from / last saved to. Prefer this to saveShowFile
    // once the show is edited through the engine — it cannot drop engine-side data.
    // `quiet` skips the "Show saved" toast (autosave: a failure is still reported).
    Q_INVOKABLE bool saveCurrentShow(const QString &path = QString(), bool quiet = false);
    // Writes the show as it is now to `path` WITHOUT making that its file (backups and the crash-recovery copy): the show's
    // own path and unsaved state are left alone, and nothing is announced.
    Q_INVOKABLE bool saveShowCopy(const QString &path);
    // Makes sure a working show exists (creating an empty one named `name`).
    Q_INVOKABLE void ensureShow(const QString &name);
    // Native file pickers (engine platform layer). "" when the user cancels.
    Q_INVOKABLE QString pickShowToOpen();
    Q_INVOKABLE QString pickShowSavePath(const QString &defaultName);
    // Opens a .vgr show. Returns { ok, error, show }; on failure the current
    // document is untouched and `error` says why (missing, damaged, wrong type).
    Q_INVOKABLE QVariantMap openShowFile(const QString &path);
    // Reads a show file WITHOUT opening it: the open show, its unsaved changes and its path are left alone (the centre page shows a
    // project's show this way). { ok, error, show }.
    Q_INVOKABLE QVariantMap peekShow(const QString &path) const;
    // The next timer (FreeShow's): every slide of the show at `path` moves on by itself after `seconds` (0 clears it). A show that is
    // open in the editor takes it as an unsaved edit; any other is changed in its file.
    Q_INVOKABLE bool setNextTimer(const QString &path, double seconds);

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
    // Returns { ok, error, made, replaced }.
    Q_INVOKABLE QVariantMap editSlides(const QString &path, const QString &op, const QStringList &slideIds, const QVariantMap &args = {});
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
    // Slide templates as their own .vgr files.
    Q_INVOKABLE bool saveTemplateFile(const QVariantMap &tmpl, const QString &path);
    Q_INVOKABLE QVariantMap loadTemplateFile(const QString &path);   // { ok, error, template }

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
    // re-edit today. Returns false (and toasts why) on failure.
    Q_INVOKABLE bool rewriteShowFromText(const QString &path, const QString &text);

signals:
    void showChanged();
    void libraryChanged();

private:
    explicit ShowService(QObject *parent = nullptr);
    bps::presentation::ShowLibrary *library();
    // Runs one engine edit on the working show (atomic), then announces the change or
    // reports why it was refused.
    bool applyEdit(const QString &title,
                   const std::function<bps::Result<void>(bps::presentation::Presentation &)> &fn);
    QString applyEditForId(const QString &title,
                           const std::function<bps::Result<std::string>(bps::presentation::Presentation &)> &fn);
    void reportError(const QString &title, const QString &message) const;
    // One engine edit on the show at `path`: the open show takes it as an unsaved edit, any other is loaded, changed and saved back.
    bool editShow(const QString &path, const QString &title, const std::function<bps::Result<void>(bps::presentation::Presentation &)> &change);
    void publishLibrary();

    QString libraryPath_;
    std::unique_ptr<bps::presentation::ShowLibrary> library_;
    QStringList categories_;
    QVariantList shows_;
    QStringList problems_;
};
