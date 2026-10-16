#pragma once

#include <atomic>
#include <optional>

#include "services/SlideBuilder.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// The UI's window onto the ENGINE's scripture: the installed Bibles (bps::bible::BibleEngine), their books / chapters / verses,
// reference lookup and search, and - the FreeShow way - the slides a chosen passage becomes when a scripture template is
// filled with it (bps::presentation::FillBookSlides). The rules (how a reference is written, how many verses a slide
// takes, the placeholders a template may use, what the options do) are the engine's; this hands the results to QML.
//
// Blocks come out in the Edit screen's block shape, so the Scripture tab's preview is drawn by the same DesignPreview as
// every other slide and design.
class ScriptureService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Installed Bibles: [{ id, name, abbreviation, language, verseCount }] (a Bible still being read is not listed yet).
    Q_PROPERTY(QVariantList bibles READ bibles NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)

public:
    static ScriptureService &instance();
    static ScriptureService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantList bibles() const;
    bool loading() const;

    // A Bible's books in order: [{ id, name, testament, chapters: [1, 2, ...], verseCounts: [31, 25, ...] }]
    Q_INVOKABLE QVariantList books(const QString &bibleId) const;
    // One chapter: [{ number, text, heading }]
    Q_INVOKABLE QVariantList chapter(const QString &bibleId, const QString &bookId, int chapter) const;
    // Loads a chapter off the UI thread; stale requests are discarded.
    Q_INVOKABLE void chapterAsync(const QString &bibleId, const QString &bookId, int chapter, int token);
    // "John 3:16", "Ps 23" -> { bookId, book, chapter, verseStart, verseEnd }, or {} when it is not a reference.
    Q_INVOKABLE QVariantMap resolve(const QString &text, const QString &bibleId) const;
    // Searches the verse text: [{ reference, bookId, book, chapter, verse, snippet }], best first.
    // `exactWords` (the search boxes' padlock): every typed word must appear in the
    // verse as a WHOLE word — the engine's own fuzzy/typo matching is filtered out
    // here (its index has no exact switch), so "word" stops finding "words".
    // `scopeBookId` + `scopeChapter` are the pane's CHAPTER LOCK: when the search
    // is locked to the passage on screen, only that book's chapter is searched
    // (Revelation 1's verses, not the rest of Revelation and not another book).
    // Empty `scopeBookId` = the whole Bible; `scopeChapter` 0 = the whole book.
    Q_INVOKABLE QVariantList search(const QString &text, const QString &bibleId, int limit = 60,
                                    bool exactWords = false,
                                    const QString &scopeBookId = QString(),
                                    int scopeChapter = 0) const;
    // ASYNC search (the Quick-search pattern): the same rows on a worker thread
    // — the pill search pane runs this per keystroke and the GUI never blocks.
    // `token` = newest request wins; answers arrive as searchResultsReady(token, rows).
    Q_INVOKABLE void searchAsync(const QString &text, const QString &bibleId, int limit, int token,
                                 bool exactWords = false, const QString &scopeBookId = QString(),
                                 int scopeChapter = 0);
    // "Genesis 1:1-3, 5" (the engine's way of writing it). No verses = the whole chapter.
    Q_INVOKABLE QString reference(const QString &book, int chapter, const QVariantList &verses) const;

    // ---- PARALLEL VERSIONS (the "Scripture 2 / 3 / 4" layouts) ----------------
    //
    // The EXTRA Bibles shown BESIDE the selected one, in slot order: the first is
    // {scripture2_*}, the second {scripture3_*}, the third {scripture4_*}. Kept in
    // the session (so the choice comes back with the tab) and read by the fill,
    // which gives every version the same verses of the same passage in its OWN
    // wording — see BookFill's ParallelBible.
    Q_INVOKABLE QStringList parallelBibles() const;
    // Replace that selection. False when an id is not an installed Bible, or the
    // list is longer than the four-Bible vocabulary — the caller can say so,
    // instead of a layout quietly rendering empty columns.
    Q_INVOKABLE bool setParallelBibles(const QStringList &bibleIds);
    // Add or drop one (what a chip in the version picker does).
    Q_INVOKABLE bool toggleParallelBible(const QString &bibleId);
    // Is this id one of the extra versions? (the picker's tick)
    Q_INVOKABLE bool isParallelBible(const QString &bibleId) const;

    // ---- scripture templates ----
    // The templates made for scripture: [{ id, name, color }] (the engine's own set plus the user's).
    Q_INVOKABLE QVariantList templates() const;
    // The template in use: the one chosen in the tab's options, else the engine's default "Scripture".
    Q_INVOKABLE QString templateId() const;
    Q_INVOKABLE QString templateName(const QString &id) const;
    // Does the template use the scripture placeholders? (No: it is an old-style template, and the tab says so.)
    Q_INVOKABLE bool templateHasValues(const QString &id) const;
    Q_INVOKABLE QString defaultTemplateId() const;

    // ---- a passage on slides ----
    // The preview: the picked verses (of one chapter) on ONE slide of the template in use, plus what the panel shows about it:
    //   { blocks, background, reference, hasValues, slideCount }
    // `verses` = the verse numbers picked. `blocks` is empty when nothing is picked.
    Q_INVOKABLE QVariantMap preview(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const;
    // Every slide the passage needs (verses shared out by the options): [{ title, blocks }] - what "Convert to show" builds.
    Q_INVOKABLE QVariantList slides(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const;

    // ---- user data (REAL, engine-stored — docs/specs/24 §User data) -------
    // Notes and highlights live in the ENGINE (AddNote/SetHighlight, stored
    // separately from Scripture text and persisted with it) — never in QML
    // state. A note replaces the reference's previous one ("" text removes).
    // All calls resolve `reference` through the engine ("John 3:16"); a bad
    // reference is a false return / empty list, never a crash.
    Q_INVOKABLE bool setNote(const QString &bibleId, const QString &reference, const QString &text);
    // One reference's note text ("" = none).
    Q_INVOKABLE QString note(const QString &bibleId, const QString &reference) const;
    // Every note: [{ reference, bookId, book, chapter, verseStart, verseEnd, text, modifiedMs }], newest first.
    Q_INVOKABLE QVariantList notes(const QString &bibleId) const;
    // Highlight on/off (a re-toggle removes it).
    Q_INVOKABLE bool setHighlighted(const QString &bibleId, const QString &reference, bool on);
    // ONE call per open chapter: { "16": { highlight: true, note: "..." } } —
    // the verse rows read this instead of each re-resolving a reference.
    Q_INVOKABLE QVariantMap chapterUserData(const QString &bibleId, const QString &bookId,
                                            int chapter) const;
    Q_INVOKABLE bool isHighlighted(const QString &bibleId, const QString &reference) const;
    // Every highlighted reference: ["JHN 3:16", ...] (the engine's canonical form).
    Q_INVOKABLE QVariantList highlights(const QString &bibleId) const;

    // ---- installing ----
    // Asks for a Bible file (json / xml / osis / usfm / txt) and installs it. Returns true when one was installed.
    Q_INVOKABLE bool importBible();   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread

signals:
    void changed();
    // A note or highlight was written — verse rows re-read their marks.
    void userDataChanged();
    // searchAsync's answer: token matches the request, rows are the hits
    // (search()'s shape). Stale tokens never emit.
    void searchResultsReady(int token, const QVariantList &rows);
    void chapterReady(int token, const QString &bibleId, const QString &bookId,
                      int chapter, const QVariantList &rows);
    // ---- the published corpus reads (see the private note below) -------------
    // A worker's answer landed — one chapter's rows, or a typed reference's
    // resolution. The panes re-read on these: the same "publish on a worker, let
    // the UI re-read" contract the mark rosters use (userDataChanged), and the
    // reason a sync read can answer without touching the engine.
    void chapterCached(const QString &bibleId, const QString &bookId, int chapter);
    void referenceCached(const QString &bibleId, const QString &text);
    // A published SEARCH answer landed (the sync `search()`'s worker read). The
    // sync entry point has no token to match — it answers `[]` for a query nobody
    // has run yet, exactly like peekShow's `pending`, which is a different fact
    // from "no rows" — so a caller that used it re-asks on this signal. A caller
    // that can wait uses searchAsync (token-guarded) and never needs it.
    void searchCached(const QString &text);

private:
    // ---- THE PUBLISHED CORPUS READS -----------------------------------------
    // `bibles`, `books`, `chapter`, `resolve` and `search` used to ask the ENGINE
    // on the interface thread — a lock and a copy per question, taken from QML
    // bindings (`sources`, `visibleBooks`), from the text field's completer (per
    // keystroke) and from the hover peek. The corpus is immutable between
    // imports, so each answer is now READ ONCE ON A WORKER and kept here as the
    // published value; the entry points are lookups, and a miss queues the read
    // and returns what is known. What gets the pane its answer is the notify that
    // lands with it (`changed` for the lists, `chapterCached` for a chapter,
    // `referenceCached` for a resolution) — the panes re-read on those, exactly
    // as they already do for a note or a highlight.
    //
    // The `read*` half is the OLD BODY, unchanged, called from a worker: it is
    // what chapterAsync and searchAsync already ran off the interface thread.
    QVariantList readBibles() const;
    QVariantList readBooks(const QString &bibleId) const;
    QVariantList readChapter(const QString &bibleId, const QString &bookId, int chapter) const;
    QVariantMap readResolve(const QString &text, const QString &bibleId) const;
    QVariantList readSearch(const QString &text, const QString &bibleId, int limit, bool exactWords,
                            const QString &scopeBookId, int scopeChapter) const;

    static QString chapterCacheKey(const QString &bibleId, const QString &bookId, int chapter);
    static QString referenceCacheKey(const QString &bibleId, const QString &text);

    void refreshBiblesOffThread() const;
    void refreshBooksOffThread(const QString &bibleId) const;
    void refreshChapterOffThread(const QString &bibleId, const QString &bookId, int chapter) const;
    void refreshReferenceOffThread(const QString &bibleId, const QString &text) const;
    void refreshSearchOffThread(const QString &text, const QString &bibleId, int limit, bool exactWords,
                                const QString &scopeBookId, int scopeChapter) const;
    // An import can replace a Bible under the same id, so every published answer
    // is dropped with it (see the textMoved handler in the .cpp).
    void dropCorpusCaches() const;
    // THE CORPUS GENERATION (see dropCorpusCaches). A read that STARTED before a
    // drop must not publish into the cache the drop emptied: the pane's own
    // re-read — the drop announces itself, so one is already under way — finds the
    // cache filled again and therefore queues nothing, which makes the pre-import
    // answer the last word on the subject for the rest of the session. A stale
    // landing is dropped and its read re-queued instead (self-healing), the way
    // SlideBuilder's warm requests already discard a cut whose request was
    // replaced rather than publishing it.
    mutable int corpusGeneration_ = 0;

    mutable QVariantList biblesCache_;
    mutable bool biblesInFlight_ = false;
    mutable QHash<QString, QVariantList> booksCache_;      // bibleId -> outline
    mutable QSet<QString> booksInFlight_;
    mutable QHash<QString, QVariantList> chapterCache_;    // "bibleId\nbookId\nn" -> verse rows
    mutable QSet<QString> chapterInFlight_;
    mutable QHash<QString, QVariantMap> referenceCache_;   // "bibleId\ntext" -> resolved reference
    mutable QSet<QString> referenceInFlight_;
    mutable QHash<QString, QVariantList> searchCache_;     // the whole query, as its key
    mutable QSet<QString> searchInFlight_;

    std::atomic<int> latestSearchToken_{0};   // searchAsync: only the newest emits
    std::atomic<int> latestChapterToken_{0};  // chapterAsync: only the newest emits
    explicit ScriptureService(QObject *parent = nullptr);

    // ---- THE PUBLISHED MARKS (every note and highlight of a Bible) ---------
    // The five mark READS below — `notes`, `highlights`, `note`, `isHighlighted`,
    // `chapterUserData` — used to answer by walking the ENGINE's whole-Bible
    // rosters on the interface thread: a lock and a copy per question, and
    // `chapterUserData` is asked once per open chapter while `isHighlighted` was
    // asked per row (the guard's soft inventory). The rosters are now read ONCE
    // PER BIBLE on a worker (refreshMarksOffThread) and kept here as the
    // published answer; the reads are scans of these lists, and the panes already
    // re-read on `userDataChanged` — emitted when a refresh lands and after every
    // write, so what the verse rows draw can never be older than the last thing
    // the UI did.
    //
    // The two WRITES (`setNote`, `setHighlighted`) queue the engine call for the
    // same reason — a mark is a small sidecar write, not interface-thread work —
    // and re-refresh the Bible's rosters when it lands, which is what publishes
    // the change instead of the write doing it inline.
    struct CachedMark {
        QString reference;   // the engine's canonical form (highlights() returns these)
        QString bookId;
        int chapter = 0;
        int verseStart = 0;
    };
    struct CachedNote {
        QString reference, bookId, book, text;
        int chapter = 0;
        int verseStart = 0;
        int verseEnd = 0;
        qint64 modifiedMs = 0;
    };
    // A reference the caller typed, as the ENGINE resolves it (canonical "JHN 3:16"
    // form), resolved on a worker: the marks above are keyed by the engine's own
    // spelling, so matching needs this instead of a resolver on the interface
    // thread. Empty value = the engine says it is not a reference.
    mutable QHash<QString, QString> refCanonical_;   // "bibleId\ntext" -> canonical
    mutable QSet<QString> refPending_;
    // nullptr = "no answer yet" (a resolution is in flight); an empty string =
    // the engine says it is not a reference at all.
    const QString *canonicalRef(const QString &bibleId, const QString &text) const;

    mutable QHash<QString, QList<CachedMark>> marksByBible_;
    mutable QHash<QString, QList<CachedNote>> notesByBible_;
    mutable QSet<QString> marksWarm_;      // rosters read at least once
    mutable QSet<QString> marksInFlight_;  // one read per Bible at a time
    void refreshMarksOffThread(const QString &bibleId, bool force = false) const;
    void publishMarks(const QString &bibleId, QList<CachedMark> marks,
                      QList<CachedNote> notes) const;
    const QList<CachedMark> &marksFor(const QString &bibleId) const;
    const QList<CachedNote> &notesFor(const QString &bibleId) const;
    // The queued writes (see the .cpp). Both mean "accepted": the engine call
    // runs on a worker and the roster is republished when it lands, which is the
    // `userDataChanged` the verse rows re-read on.
    void writeNoteOffThread(const QString &bibleId, const QString &reference,
                            const QString &text, bool remove) const;
    void writeHighlightOffThread(const QString &bibleId, const QString &reference, bool on) const;

    // A chapter's split identity (SlideBuilder::ChapterKey) as this tab asks for it:
    // its template, its own options, and which Bible / book / chapter.
    SlideBuilder::ChapterKey chapterKey(const QString &bibleId, const QString &bookId, int chapter) const;
    // THE SPLIT CUT AHEAD OF THE CLICK for ONE item of this chapter, or nothing when
    // there is none (never warmed, or the template / options moved since) — the caller
    // then fills exactly as it always did. Never a wrong or empty answer.
    //
    // It also writes the two ONE-SHOT diagnostics a chapter is worth: the first pick
    // served from the warm split, and the first that had to be filled on the click.
    // Together they say whether the pre-cut is actually carrying the picks — a warm
    // that silently stops being used (a template changed under it) is visible in the
    // log instead of only being felt. The latches are `mutable` because they are a
    // log-once mark, not part of the answer.
    std::optional<SlideBuilder::WarmSlides> warmFor(const QString &bibleId, const QString &bookId,
                                                    int chapter, int number) const;
    mutable bool warmServeLogged_ = false;
    mutable bool warmMissLogged_ = false;

    // THE CHAPTER THE TAB HAS OPEN, AND THE KEY ITS SPLIT WAS CUT WITH. A later change
    // to that key (a template, an option the division depends on) means the rows on
    // screen were cut by a recipe that no longer holds, so the chapter is cut again —
    // in the background, exactly as the open cut it. See the changed handler in the .cpp.
    QString cutSourceId_;   // the Bible the open chapter was read from
    QString cutBookId_;
    int cutChapter_ = 0;
    int cutToken_ = 0;
    std::optional<SlideBuilder::ChapterKey> cutKey_;
    // The WORDS under the open chapter moved (a Bible reinstalled, a template edited in
    // place): the key cannot see that, so it is said here instead, and the chapter is cut
    // again. Cleared when that happens.
    bool cutStale_ = false;
};
