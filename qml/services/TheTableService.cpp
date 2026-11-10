#include "services/CallTrace.h"
#include "services/TheTableService.h"

#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"
#include "services/SlideBuilder.h"
#include "services/ShowConverter.h"

#include <QThread>

#include "modules/library/TheTableLibrary.hpp"
#include "modules/presentation/BookFill.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QFileInfo>
#include <QJSEngine>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <optional>
#include <thread>

namespace bl = bps::library;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

bl::TheTableLibrary *tableLibrary(void *opaque)
{
    return static_cast<bl::TheTableLibrary *>(opaque);
}

void report(const QString &message, const QString &level)
{
    EventBus::instance().notify(message, level, QObject::tr("The Table"),
                                QStringLiteral("table.library"));
}

namespace pf = bps::presentation;

// The engine's default table template (the id the template catalog gives the plain
// "Table" layout) — named, one definition, exactly as ScriptureService names its
// own (kDefaultTemplate): the id used to be a literal repeated in the profile and
// again in defaultTemplateId(), so changing one left the other behind.
constexpr const char *kDefaultTemplate = "tpl-table";

// This tab's place in the shared slide builder: its own "table" templates and its own setting.
const SlideBuilder::Profile kProfile{ "table", "table.template", kDefaultTemplate };

// The engine's paragraph source for the slide builder: a "book" (year) +
// chapter (sermon) and the picked paragraphs as verses.
// A sermon's stored title carries its own leading date code ("0217 Only Believe" - the "0217" is what keeps two sermons of the same
// name apart, and what the engine matches an already-imported sermon by), which is clutter once the sermon is just being shown by name
// in a list. Display-only: nothing stored ever goes through this.
QString displayTitle(const QString &title)
{
    static const QRegularExpression re(QStringLiteral("^\\d{3,4}[A-Za-z]?\\s+"));
    const QString stripped = QString(title).remove(re);
    return stripped.isEmpty() ? title : stripped;
}

// The year book's display name is the full year ("1947") — one year groups many
// sermons, so the row itself can't carry a single sermon code; the shorthand
// "47-…" lives in the citation line only. When TYPING a reference, the short
// form ("47") still matches, see resolve()/reference().
QString displayYear(const QString &name)
{
    return name;
}

// A typed book token matches a year book: the full form ("1947") or the
// sermon-citation shorthand ("47"). Display is always the full year; this is
// for resolve()/reference() input only.
bool matchesYear(const QString &name, const QString &typed)
{
    return name == typed || (typed.size() == 2 && name == QStringLiteral("19") + typed);
}

// Citation search's engage threshold: FIVE chars, for every needle (user call:
// "all the types of search for the table should be at least 5"). The reason is the
// code shape: every citation line opens "<yy>-<dd…> - Title", so short fragments
// ("47-", "fai") match dozens of sermons and the resolve would always land on the
// first one. Five chars means the needle is already a real narrowing. (Year-shaped
// input — "47", "1948" — never reaches citation search: resolve() answers it before
// falling through.) Mirrored in ReferencePane.suggest (the inline completion +
// dropdown) so the two sides engage on the same keystroke.
int citationSearchMinChars(const QString &needle)
{
    Q_UNUSED(needle)
    return 5;
}

bool sourceFor(bl::TheTableLibrary *lib, const QString &bookId, int chapter,
               const QVariantList &verses, pf::BookSource &out)
{
    out = pf::BookSource{};
    if (!lib)
        return false;
    auto ch = lib->GetChapter(bookId.toStdString(), chapter);
    if (!ch.ok())
        return false;
    for (const auto &b : lib->BookIndex()) {   // (the index: Books() would copy every sermon's words just to find a name)
        if (b.id != bookId.toStdString())
            continue;
        // The citation line ("53-0217 - Only Believe") is what the engine's
        // reference builder takes as its "book": with the family set to Table,
        // BookReference writes the citation followed by the paragraph numbers and
        // no chapter — "47-0412 - Faith Is The Substance 1-3". (Scripture keeps
        // its own plain book name and writes its chapter.) The FAMILY is what
        // says which of the two shapes this is; it used to be chapter zero.
        out.book = lib->Citation(bookId.toStdString(), chapter, 0);
        // ...and the citation's leading code on its own ("47-0412"), which is
        // what {table_code} renders — a template with room for a date code has
        // nowhere to put the whole citation line. The year and the sermon's
        // title come off the same citation (Parts): a design with a slot for
        // each of them gets the real pieces instead of an empty {scripture_name}
        // (a sermon has no Bible version) and a title buried inside the line.
        const auto parts = lib->Parts(bookId.toStdString(), chapter);
        out.code = lib->CitationCode(bookId.toStdString(), chapter);
        out.year = parts.year;
        out.title = parts.title;
        break;
    }
    // A SERMON, NAMED (BookFill::ContentFamily): the filler shapes the reference
    // for a citation (no chapter, a space before the paragraph numbers) and
    // answers only the tokens a sermon has one for, so nothing here has to encode
    // "this is a sermon" as a missing chapter.
    out.family = pf::ContentFamily::Table;
    out.chapter = 0;   // a fact about the data: a sermon has no chapter
    for (const QVariant &v : verses) {
        const int n = v.toInt();
        for (const bl::TheTableVerse &tv : ch.value().verses) {
            if (tv.number == n) {
                out.items.push_back({ n, tv.text });
                break;
            }
        }
    }
    return !out.items.empty();
}

} // namespace

TheTableService::TheTableService(QObject *parent)
    : QObject(parent)
{
    // A sermon whose SPLIT is no longer the right one is cut again, in the background: the
    // paragraph continuations the list shows ARE that cut's rows. Two things make it wrong
    // — a key that moved (a template, an option), or the words under it (an import landed,
    // the library was reopened, a template was edited in place): the second cannot be seen
    // in the key, so libraryMoved says so. See ScriptureService's own handler for the full
    // reasoning (same contract).
    connect(this, &TheTableService::changed, this, [this] {
        if (!cutKey_ || (!cutStale_ && chapterKey(cutBookId_, cutChapter_) == *cutKey_))
            return;
        cutStale_ = false;
        SlideBuilder::dropWarm();   // what is held is not this sermon's cut any more
        chapterAsync(cutBookId_, cutChapter_, cutToken_);
    });
    // Boot may already have run (tests, reload) or may still be pending (the
    // singleton is created when QML first touches it) — build the library the
    // moment the platform layer exists, the same contract as the design
    // libraries (a service created pre-boot stayed empty without the retry).
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            loadLibrary();
    });
    if (EngineBridge::instance().booted())
        loadLibrary();
    // A template landed/changed: the preview, the slides and the template card re-derive
    // from it (Scripture's contract) — and a template edited IN PLACE (same id, different
    // blocks) is words the held cut no longer has, so libraryMoved says so first.
    connect(&TemplateLibraryService::instance(), &DesignLibraryService::changed, this, [this] {
        libraryMoved();
    });
    // The tab's own options moved: the pane re-derives (as it always did), and the cut is
    // re-made only if the key really changed — see the handler above.
    connect(&SettingsService::instance(), &SettingsService::changed, this, &TheTableService::changed);
}

// THE WORDS MOVED (a sermon imported, the library reopened, a template edited in place):
// the cut held for the open sermon was made from text or blocks that are no longer there,
// so it goes — and the sermon on screen is cut again from what replaced it, because the
// `changed` this emits is what the re-cut above hangs off. A SETTINGS write deliberately
// does NOT come through here: the split's key carries everything the division depends on
// and is compared at each use, so a write that leaves it alone (a highlight, a note, a
// column dragged) keeps the cut and the next click stays a lookup.
void TheTableService::libraryMoved()
{
    cutStale_ = true;
    SlideBuilder::dropWarm();
    emit changed();
}

TheTableService &TheTableService::instance()
{
    static TheTableService s;
    return s;
}

TheTableService *TheTableService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

void TheTableService::loadLibrary()
{
    // Once, whether it is the boot call or the first touch of the tab that gets here.
    if (library_ || loadStarted_)
        return;
    loadStarted_ = true;
    loading_ = true;
    // The pane reads `loading()` (and the still-empty `sources`); the notify is what
    // makes a pane built before the read finished say "Opening the library…" rather
    // than an empty library.
    emit changed();

    auto &platform = bps::platform::PlatformAccessor::Get();
    const std::string file = platform.Filesystem().Join(platform.Paths().UserDataDir(),
                                                        "the_table.json");
    // OPEN + PARSE OFF THE INTERFACE THREAD (user call: "make The Table's library load
    // happen off the interface thread so opening the tab never blocks on reading the
    // sermon JSON"). The library is ~1,200 sermons, and Open() reads and parses the
    // whole the_table.json — that parse IS the cost, and it used to be paid inline on
    // the interface thread (in the boot path, and again when the singleton was first
    // touched, which is what made opening the tab wait).
    //
    // The PARSE is the worker's; the PUBLISH is not. Every other method of this
    // service reads `library_` on the interface thread, so the pointer is assigned in
    // the queued callback below — the worker only builds the object.
    // The receiver is guarded: the read can outlive the service at quit (the singleton
    // is destroyed at exit), and a queued call to a destroyed object is undefined. The
    // EngineBridge's own worker uses this same QPointer idiom.
    auto *worker = QThread::create([this, file, guard = QPointer<TheTableService>(this)] {
        QElapsedTimer clock;
        clock.start();
        std::shared_ptr<bl::TheTableLibrary> opened = bl::TheTableLibrary::Open(file);
        // ONE line per open: what the parse cost, and that it cost it HERE rather than
        // on the interface thread. Paired with the pane's own "Opening the library…"
        // state, it is what says the tab was ready before the library was.
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("TheTableLoad"),
                            QStringLiteral("library opened off the interface thread in %1 ms")
                                .arg(clock.elapsed()));
        if (!guard)
            return;   // shutting down: nothing to publish to
        QMetaObject::invokeMethod(guard.data(), [this, opened] {
            libraryOwner_ = opened;   // owns the engine object for the process
            library_ = opened.get();
            loadStarted_ = false;
            loadFailed_ = library_ == nullptr;
            loading_ = false;
            if (library_ == nullptr) {
                report(tr("The sermon library could not be opened."), QStringLiteral("error"));
                emit changed();
                return;
            }
            // The sermons join the platform Search Engine's index (the Bible/Song
            // precedent): once here, the app-wide search (Ctrl+K) finds them. One
            // document per sermon — ~1,200 upserts, done in the background so the
            // first paint never waits on the index — on the ONE joinable worker, so
            // quitting can always stop + join it before the engine tears down.
            startIndexing();
            libraryMoved();   // opened: the held cut is text that may have moved (notifies)
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // Quitting while the read runs must not destroy a running thread, and must not
    // freeze the UI waiting for it — the same bounded contract as the import paths.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, worker,
            [worker] { (void)worker->wait(2000); });
    worker->start();
}

bool TheTableService::loading() const
{
    // THE TAB IS LOADING UNTIL THE ENGINE HAS BOOTED AND ITS LIBRARY IS OPEN (user
    // call: a source that is absent for a moment must read as loading, not as an
    // empty library). `sources()` answers [] in exactly that window — the library is
    // opened on `bootedChanged`, so a pane built before the engine came up would
    // otherwise say "No sermons yet — add one with New sermon" about a library it
    // simply had not read yet. `library_` is set once and held for the process
    // (see loadLibrary), so this is false forever after the one open — and false
    // after a read that FAILED (`loadFailed_`), so a library that could not be
    // opened shows its empty wording instead of a spinner that never stops.
    return loading_ || !EngineBridge::instance().booted()
           || (library_ == nullptr && !loadFailed_);
}

qlonglong TheTableService::verseCount() const
{
    if (!library_ || !EngineBridge::instance().booted())
        return 0;
    return static_cast<qlonglong>(tableLibrary(library_)->VerseCount());
}

QVariantMap TheTableService::documentFromEngine() const
{
    QVariantMap doc;
    doc.insert(QStringLiteral("name"), QStringLiteral("The Table"));
    QVariantList collections;
    if (library_ && EngineBridge::instance().booted()) {
        QVariantMap collection;
        collection.insert(QStringLiteral("name"), QStringLiteral("Sermons"));
        QVariantList books;
        for (const bl::TheTableBook &book : tableLibrary(library_)->Books()) {
            QVariantMap b;
            b.insert(QStringLiteral("name"), qstr(book.name));
            QVariantList sections;
            for (const bl::TheTableChapter &ch : book.chapters) {
                QVariantMap s;
                s.insert(QStringLiteral("title"), qstr(ch.title));
                QVariantList verses;
                for (const bl::TheTableVerse &v : ch.verses)
                    verses.append(qstr(v.text));
                s.insert(QStringLiteral("verses"), verses);
                sections.append(s);
            }
            b.insert(QStringLiteral("sections"), sections);
            books.append(b);
        }
        collection.insert(QStringLiteral("books"), books);
        collections.append(collection);
    }
    doc.insert(QStringLiteral("collections"), collections);
    return doc;
}

QVariantMap TheTableService::document() const
{
    return documentFromEngine();
}

QVariantList TheTableService::chapter(const QString &bookId, int chapter) const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto ch = tableLibrary(library_)->GetChapter(bookId.toStdString(), chapter);
    if (!ch.ok())
        return out;
    for (const bl::TheTableVerse &v : ch.value().verses) {
        QVariantMap row;
        row.insert(QStringLiteral("number"), v.number);
        row.insert(QStringLiteral("text"), qstr(v.text));
        row.insert(QStringLiteral("heading"), qstr(v.heading));
        out.append(row);
    }
    return out;
}

void TheTableService::previewChapterAsync(const QString &bookId, int chapter, int token)
{
    latestPreviewToken_ = token;
    auto *runner = QThread::create([this, bookId, chapter, token] {
        if (token != latestPreviewToken_.load(std::memory_order_acquire))
            return;
        const QVariantList rows = this->chapter(bookId, chapter);
        QMetaObject::invokeMethod(this, [this, bookId, chapter, token, rows] {
            if (token == latestPreviewToken_.load(std::memory_order_acquire))
                emit previewChapterReady(token, bookId, chapter, rows);
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void TheTableService::chapterAsync(const QString &bookId, int chapter, int token)
{
    // Read on the GUI thread before the worker starts (template + this tab's options):
    // the key built here is the one the click will build, which is what lets the two
    // meet. See ScriptureService::chapterAsync for the same contract.
    const SlideBuilder::ChapterKey key = chapterKey(bookId, chapter);
    const std::vector<pf::ContentBlock> templateBlocks = SlideBuilder::templateBlocks(key.templateId);
    // The keys of the SERMONS either side of this one in the collection — the passages a
    // step reaches next. Built here (see ScriptureService::chapterAsync: a worker thread
    // is no place for the service state a key is read from), and built by the same
    // chapterKey a step will use, so the read-ahead can actually be served.
    std::vector<SlideBuilder::ChapterKey> neighbourKeys;
    {
        const int candidates[2] = { chapter - 1, chapter + 1 };
        for (const int other : candidates)
            if (other > 0)
                neighbourKeys.push_back(chapterKey(bookId, other));
    }
    // The generation the read happens under (see SlideBuilder::WarmRequest).
    const quint64 epoch = SlideBuilder::warmEpoch();
    // What is on screen, and the key it was cut with (the guard the handler above asks).
    cutBookId_ = bookId;
    cutChapter_ = chapter;
    cutToken_ = token;
    cutKey_ = key;
    auto *runner = QThread::create([this, bookId, chapter, token, key, templateBlocks, neighbourKeys, epoch] {
        // READ AHEAD FIRST: a sermon a step already brought in is held whole (words and
        // cut), so this load skips the engine read entirely — the read is what a step
        // normally waits on (see ScriptureService::chapterAsync).
        QVariantList rows = SlideBuilder::warmedRows(key);
        const bool readAhead = !rows.isEmpty();
        if (!readAhead)
            rows = this->chapter(bookId, chapter);
        // THE HEAD OF THE SERMON IS CUT BEFORE IT IS CLICKED: the paragraphs the list can
        // paint, into their slides HERE, so picking one while presenting is a lookup rather
        // than a fill on the interface thread. A sermon's paragraphs are long, so this is
        // where the cut costs the most and buys the most (the chapter is read once for all
        // of them).
        //
        // AND ONLY THE HEAD: the real library's sermons run 171 paragraphs at the median
        // and 500 at the longest, and cutting all of one before it could be seen measured
        // 0.2 ms per paragraph (64 ms for a 277-paragraph sermon) for a list that shows the
        // first screenful. The rest is cut below, once these rows are on their way.
        int warmed = 0;
        qint64 warmMs = 0;
        bool cut = false;
        pf::BookSource all;
        {
            QElapsedTimer clock;
            clock.start();
            QVariantList numbers;
            for (const QVariant &row : rows)
                numbers.append(row.toMap().value(QStringLiteral("number")).toInt());
            if (!numbers.isEmpty()
                && sourceFor(tableLibrary(library_), bookId, chapter, numbers, all)) {
                const SlideBuilder::WarmRequest request{ epoch, key, templateBlocks, all, rows };
                // Held already (the same key) means nothing is cut again: the same key is
                // the same answer, and a step into a read-ahead sermon only moves the pin.
                SlideBuilder::warmChapter(request);
                warmed = SlideBuilder::warmedItemCount(key);
                // The sermon's own rows carry that cut, so the paragraph list reaches the
                // screen already divided (SlideBuilder::attachSplit).
                SlideBuilder::attachSplit(rows, key);
                // ...AND A CUT THAT DIVIDED NOTHING, FOR A TAB THAT SAYS DIVIDE, IS CUT
                // ONCE MORE FROM SCRATCH. A held cut is never re-made (SlideBuilder::hold
                // answers the same key with the same entry), so a division that came out
                // empty — cut before the option was turned on, or while the text under it
                // was being replaced — stayed empty for as long as that entry lived. That
                // is the reported "the divide snaps back when I leave The Table; I have to
                // toggle Divide long paragraphs off and on": the toggle works only because
                // it changes the key, and a new key is a new cut. Dropping the held cuts
                // and cutting this sermon again gives the same fresh answer without the
                // toggle. It is bounded: once per open, and only when the tab asked for a
                // division and NOT ONE row got one.
                if (key.options.splitLongItems) {
                    bool anySplit = false;
                    for (const QVariant &row : rows)
                        if (row.toMap().value(QStringLiteral("split")).toList().size() > 1) {
                            anySplit = true;
                            break;
                        }
                    if (!anySplit) {
                        SlideBuilder::dropWarm();
                        SlideBuilder::warmChapter(
                            SlideBuilder::WarmRequest{ SlideBuilder::warmEpoch(), key,
                                                       templateBlocks, all, rows });
                        warmed = SlideBuilder::warmedItemCount(key);
                        SlideBuilder::attachSplit(rows, key);
                    }
                }
                cut = true;
            }
            warmMs = clock.elapsed();
        }
        QMetaObject::invokeMethod(this, [this, bookId, chapter, token, rows, warmed, warmMs, readAhead] {
            if (token != latestChapterToken_)
                return;
            if (warmed > 0) {
                // ONE line per sermon (see ScriptureService's for the pair this makes
                // with warmFor's first-pick diagnostics). A sermon that arrived without a
                // read says so: that is a step served from the read-ahead.
                warmServeLogged_ = false;
                warmMissLogged_ = false;
                EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                    readAhead
                                        ? QStringLiteral("%1 %2: served from the sermon read and cut ahead of this step — no read and no fill for it (off the interface thread)")
                                              .arg(bookId, QString::number(chapter))
                                        : QStringLiteral("%1 %2: cut the first %3 paragraph(s) ahead of the click in %4 ms, so the list opens divided (the rest is cut with the rows already on screen)")
                                              .arg(bookId, QString::number(chapter), QString::number(warmed),
                                                   QString::number(warmMs)));
            }
            emit chapterReady(token, bookId, chapter, rows);
        }, Qt::QueuedConnection);

        // ---- THE REST OF THE SERMON, once those rows are on their way ---------
        // Every paragraph below the head still has to be cut, because the list shows a long
        // paragraph's continuations the moment the sermon is open (SlideBuilder::attachSplit)
        // — that division is the reported bug, and a head-only list would bring it back for
        // everything past the fold. So the remainder is cut HERE, in the same worker, and the
        // rows are handed over a second time when it lands: the open never waits for it.
        if (cut && latestChapterToken_.load() == token) {
            const int headItems = warmed;
            QElapsedTimer restClock;
            restClock.start();
            const bool more = SlideBuilder::warmChapterMore(
                SlideBuilder::WarmRequest{ epoch, key, templateBlocks, all, rows });
            const qint64 restMs = restClock.elapsed();
            if (more && latestChapterToken_.load() == token) {
                // The same rows, now carrying the cut of every paragraph in them.
                SlideBuilder::attachSplit(rows, key);
                const int wholeSermon = SlideBuilder::warmedItemCount(key);
                QMetaObject::invokeMethod(this, [this, bookId, chapter, token, rows, restMs,
                                                 wholeSermon, headItems] {
                    if (token != latestChapterToken_)
                        return;
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                        QStringLiteral("%1 %2: the remaining %3 paragraph(s) cut in %4 ms, with the head's %5 already on screen")
                                            .arg(bookId, QString::number(chapter))
                                            .arg(QString::number(wholeSermon - headItems))
                                            .arg(QString::number(restMs))
                                            .arg(QString::number(headItems)));
                    emit chapterReady(token, bookId, chapter, rows);
                }, Qt::QueuedConnection);
            }
        }

        // ---- AND THE SERMONS ON EITHER SIDE, so the next step is immediate ----
        // See ScriptureService::chapterAsync for the whole reasoning: two at most, only
        // ones that exist, and only while this is still the load the pane wants.
        QElapsedTimer aheadClock;
        aheadClock.start();
        QStringList ahead;
        for (const SlideBuilder::ChapterKey &otherKey : neighbourKeys) {
            if (latestChapterToken_.load() != token)
                return;
            if (!SlideBuilder::warmedRows(otherKey).isEmpty())
                continue;   // in hand already
            const QVariantList otherRows = this->chapter(bookId, otherKey.chapter);
            if (otherRows.isEmpty())
                continue;   // no such sermon (the end of the collection)
            QVariantList otherNumbers;
            for (const QVariant &row : otherRows)
                otherNumbers.append(row.toMap().value(QStringLiteral("number")).toInt());
            pf::BookSource otherSource;
            if (otherNumbers.isEmpty()
                || !sourceFor(tableLibrary(library_), bookId, otherKey.chapter, otherNumbers, otherSource))
                continue;
            SlideBuilder::warmNeighbour(
                SlideBuilder::WarmRequest{ epoch, otherKey, templateBlocks, otherSource, otherRows });
            ahead.append(QString::number(otherKey.chapter));
        }
        const qint64 aheadMs = aheadClock.elapsed();
        if (ahead.isEmpty() || latestChapterToken_.load() != token)
            return;
        QMetaObject::invokeMethod(this, [this, bookId, chapter, ahead, aheadMs, key] {
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                QStringLiteral("%1 %2: read %3 as well and cut each to its head, so the step to either one costs no read "
                                               "(%4 ms, %5 passage(s) in hand, off the interface thread)")
                                    .arg(bookId, QString::number(chapter), ahead.join(QLatin1Char(' ')),
                                         QString::number(aheadMs))
                                    .arg(SlideBuilder::warmedChapterCount(key.contentType)));
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    latestChapterToken_ = token;
    runner->start();
}

// The split identity of a sermon, as this tab asks for it. No source id: a sermon has
// no Bible-file equivalent — the book (sermon) and the template are the whole identity,
// and `contentType` keeps it apart from Scripture's keys.
SlideBuilder::ChapterKey TheTableService::chapterKey(const QString &bookId, int chapter) const
{
    SlideBuilder::ChapterKey key;
    key.contentType = QString::fromLatin1(kProfile.contentType);
    key.templateId = templateId();
    key.bookId = bookId;
    key.chapter = chapter;
    key.options = SettingsService::instance().theTableSettings();
    return key;
}

std::optional<SlideBuilder::WarmSlides> TheTableService::warmFor(const QString &bookId, int chapter, int number) const
{
    const SlideBuilder::ChapterKey key = chapterKey(bookId, chapter);
    SlideBuilder::WarmSlides warm = SlideBuilder::warmedItem(key, number);
    if (!warm.found || warm.slides.empty()) {
        // "Is anything held for this tab": this chapter NOT being held is exactly what
        // just happened, so asking that would make the diagnostic never fire.
        if (!warmMissLogged_ && SlideBuilder::warmedChapterCount(key.contentType) > 0) {
            warmMissLogged_ = true;
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                QStringLiteral("%1 %2 paragraph %3 was not in the warmed split — filled on the click "
                                               "(the template or the options changed since the sermon was cut)")
                                    .arg(bookId, QString::number(chapter), QString::number(number)));
        }
        return std::nullopt;
    }
    if (!warmServeLogged_) {
        warmServeLogged_ = true;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                            QStringLiteral("first paragraph served from the split cut ahead of time — no fill on the interface thread"));
    }
    return warm;
}

QString TheTableService::paragraphPair(const QString &bookId, int chapter, int verse, int count) const
{
    if (!library_ || !EngineBridge::instance().booted())
        return {};
    auto ch = tableLibrary(library_)->GetChapter(bookId.toStdString(), chapter);
    if (!ch.ok())
        return {};
    QString out;
    for (const auto &v : ch.value().verses) {
        if (v.number < verse)
            continue;
        if (!out.isEmpty())
            out += QStringLiteral("\n\n");   // (the sermon's own blank-line paragraph break)
        out += qstr(v.text);
        if (v.number >= verse + count - 1)
            break;   // the asked-for paragraph(s), then stop
    }
    return out;
}

QVariantList TheTableService::search(const QString &text, int limit, bool exactWords,
                                     bool includeFullText, const QString &scopeBookId,
                                     int scopeChapter) const
{
    return searchCancellable(text, limit, exactWords, includeFullText,
                             scopeBookId, scopeChapter, {});
}

QVariantList TheTableService::searchCancellable(const QString &text, int limit, bool exactWords,
                                                 bool includeFullText, const QString &scopeBookId,
                                                 int scopeChapter,
                                                 const std::function<bool()>& cancelled) const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    // The chapter lock goes INTO the scan (see TheTableLibrary::Search): a locked
    // query reads the one open sermon's paragraphs and stops, instead of scanning
    // the library and discarding everything outside the scope.
    auto hits = tableLibrary(library_)->Search(text.toStdString(), size_t(limit), exactWords,
                                               scopeBookId.toStdString(), scopeChapter, cancelled);
    if (!hits.ok())
        return out;
    for (const bl::TheTableSearchHit &h : hits.value()) {
        QVariantMap row;
        row.insert(QStringLiteral("reference"), qstr(h.reference));
        // The sermon's citation line ("47-0412 - Faith Is The Substance") — the
        // label every UI shows for a sermon (the pane's match rows, the app-wide
        // search results). `reference` is the scripture shape ("1953 12:3").
        row.insert(QStringLiteral("citation"),
                   qstr(tableLibrary(library_)->Citation(qstr(h.bookId).toStdString(), h.chapter, 0)));
        row.insert(QStringLiteral("bookId"), qstr(h.bookId));
        row.insert(QStringLiteral("chapter"), h.chapter);
        row.insert(QStringLiteral("verse"), h.verse);
        row.insert(QStringLiteral("snippet"), qstr(h.snippet));
        if (includeFullText)
            row.insert(QStringLiteral("fullText"), qstr(h.fullText));
        // Words spread across this paragraph and its next: the preview must fetch
        // BOTH (paragraphPair) — a single-paragraph hit previews alone.
        row.insert(QStringLiteral("spanned"), h.spanned);
        // The engine's ranking score: Quick search's pick-float uses it so a
        // previously-picked row stops burying a strictly better match (the
        // verbatim "Then, friends" paragraph outranking the picked ¶29).
        row.insert(QStringLiteral("score"), h.score);
        out.append(row);
    }
    return out;
}

// ASYNC search (the Quick-search pattern): the pill pane runs this per keystroke;
// the GUI-thread cost drops to a token check. The library's mutex makes the
// worker's Search() safe against the GUI's reads; stale tokens are dropped on
// delivery so a fast typist's older keystrokes answer into the void.
void TheTableService::searchAsync(const QString &text, int limit, int token, bool exactWords,
                                  const QString &scopeBookId, int scopeChapter)
{
    auto *runner = QThread::create([this, text, limit, token, exactWords, scopeBookId, scopeChapter] {
        // Typing creates newer requests faster than the full corpus scan can
        // finish. Skip queued stale work before it enters the serialized scan.
        if (token != latestSearchToken_.load(std::memory_order_acquire))
            return;
        const QVariantList rows = searchCancellable(text, limit, exactWords, false,
                                                    scopeBookId, scopeChapter, [this, token] {
            return token != latestSearchToken_.load(std::memory_order_acquire);
        });
        QMetaObject::invokeMethod(this, [this, token, rows] {
            if (token == latestSearchToken_)
                emit searchResultsReady(token, rows);
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    latestSearchToken_ = token;   // newest request wins
    runner->start();
}

// ---------------------------------------------------------------------------
// The shared reference-pane adapter (same calls ScriptureService answers)
// ---------------------------------------------------------------------------
QVariantList TheTableService::sources() const
{
    QVariantList out;
    if (library_ && EngineBridge::instance().booted() && tableLibrary(library_)->VerseCount() > 0)
        out.append(QVariantMap{ { QStringLiteral("id"), QStringLiteral("the-table") },
                                { QStringLiteral("name"), QStringLiteral("The Table") } });
    return out;
}

QVariantList TheTableService::books(const QString &sourceId) const
{
    Q_UNUSED(sourceId)   // one library: every year book lives in it
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    for (const auto &b : tableLibrary(library_)->BookIndex()) {   // the index, not Books(): only the numbers are wanted here
        QVariantList chapters;
        QVariantList titles;   // parallel to `chapters`: the sermon's own name, not just its number in the year (a Bible has none)
        for (const auto &ch : b.chapters) {
            chapters.append(ch.number);
            // Chapter rows read like the citation line minus the paragraph — "47-0412 -
            // Faith Is The Substance" (user call). Falls back to the bare stored title
            // (code stripped) when no citation can be built for it.
            const std::string cite = tableLibrary(library_)->Citation(qstr(b.id).toStdString(), ch.number, 0);
            titles.append(!cite.empty() ? qstr(cite) : displayTitle(qstr(ch.title)));
        }
        out.append(QVariantMap{ { QStringLiteral("id"), qstr(b.id) },
                                { QStringLiteral("name"), qstr(b.name) },
                                { QStringLiteral("chapters"), chapters },
                                { QStringLiteral("chapterTitles"), titles } });
    }
    return out;
}

QString TheTableService::reference(const QString &book, int chapter, const QVariantList &verses) const
{
    // The engine writes the verse ranges (runs collapse, commas between picks);
    // The Table's sermon part comes from the library's citation line
    // ("53-0217 - Only Believe 3"), not the scripture-shaped "1953 1:3".
    // `book` arrives as the pane's book NAME ("1947"); the library matches by
    // id ("Y1947") — resolve the name to the id here.
    std::vector<int> numbers;
    for (const QVariant &v : verses)
        numbers.push_back(v.toInt());
    if (library_ && EngineBridge::instance().booted()) {
        QString bookId = book;
        if (!book.startsWith(QStringLiteral("Y"))) {
            for (const auto &b : tableLibrary(library_)->BookIndex())
                if (matchesYear(qstr(b.name), book)) {
                    bookId = qstr(b.id);
                    break;
                }
        }
        const std::string citation = tableLibrary(library_)->Citation(bookId.toStdString(), chapter, 0);
        if (!citation.empty()) {
            const QString cite = qstr(citation);
            return numbers.empty() ? cite
                                   : cite + QStringLiteral(" %1").arg(qstr(pf::NumberRange(numbers)));
        }
    }
    // Unknown book/chapter (a stale pane): this tab's own shape still resolves
    // (a citation with paragraph numbers and no chapter — the family, not chapter
    // zero, is what says so).
    return qstr(pf::BookReference(book.toStdString(), chapter, numbers, pf::ContentFamily::Table));
}

QVariantMap TheTableService::resolve(const QString &text) const
{
    // "1953 12:3" / "1953 12" — and the short forms the years column shows —
    // "53 12:3" / "47 3", or a bare year ("1947" / "47") for the book itself.
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{2}|\\d{4})(?:\\s+(\\d{1,3})(?:\\s*:\\s*(\\d{1,4})(?:\\s*-\\s*(\\d{1,4}))?)?)?\\s*$"));
    const auto m = re.match(text.trimmed());
    if (!m.hasMatch())
        return resolveCitation(text);   // not a year/paragraph shape: search the citation lines
    QString year = m.captured(1);
    if (year.size() == 2)
        year.prepend(QStringLiteral("19"));   // "47" names the same year book "1947"
    QVariantList books;
    if (library_ && EngineBridge::instance().booted()) {
        for (const auto &b : tableLibrary(library_)->BookIndex())
            if (matchesYear(qstr(b.name), year))
                books.append(QVariantMap{ { QStringLiteral("id"), qstr(b.id) },
                                          { QStringLiteral("name"), qstr(b.name) } });
    }
    if (books.isEmpty())
        return resolveCitation(text);   // digits but no year match: try it as a code ("47-0412")
    const int chapter = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    const int start = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
    const int end = m.captured(4).isEmpty() ? start : m.captured(4).toInt();
    return { { QStringLiteral("bookId"), books.first().toMap().value(QStringLiteral("id")) },
             { QStringLiteral("book"), year },
             { QStringLiteral("chapter"), chapter },
             { QStringLiteral("verseStart"), start },
             { QStringLiteral("verseEnd"), end } };
}

// CITATION SEARCH (the tab search's real use, user call): typed text that is not a
// year/paragraph reference ("faith", "47-0412", "only believe") matches the sermons'
// citation lines ("47-0412 - Faith Is The Substance", CONTAINS, case-insensitive).
// UNIQUE match: the resolve returns it and the pane jumps straight into that sermon.
// MULTIPLE matches: the resolve returns NOTHING — the pane must not pick for the user
// (it always guessed the first hit); the QML side lists the matches in the dropdown
// (suggest, THE TABLE branch) and the user picks the actual sermon. The resolve
// re-runs per keystroke, so typing past the ambiguity jumps the moment one sermon is
// left. Five-char minimum for every needle (citationSearchMinChars above).
QString TheTableService::citationNeedle(const QString &handle) const
{
    // The citation line is WRITTEN by the library, so the rule for reading one
    // back lives there too (TheTableLibrary::CitationNeedle): "47-1020" and
    // "47 - 1020" name the same sermon — people write a date code either way —
    // and both sides of a comparison are squeezed onto that one form. This side
    // only asks for it, so the box that jumps and the pane's dropdown cannot
    // disagree about what was typed.
    return qstr(bl::TheTableLibrary::CitationNeedle(handle.toStdString()));
}

QVariantMap TheTableService::resolveCitation(const QString &text) const
{
    const QString needle = text.trimmed();
    // Five chars minimum, every needle (citationSearchMinChars above).
    if (needle.size() < citationSearchMinChars(needle) || !library_
        || !EngineBridge::instance().booted())
        return {};
    // A TRAILING " N" after the citation names the PARAGRAPH: the box
    // auto-completes the full citation line ("47-1102 - The Angel Of God 1"),
    // so "…God 1 7" is the user asking for ¶7 of that sermon — and since the
    // needle is no longer a substring of the citation, the search as typed
    // finds nothing. Citation lines can END in a part digit ("…Of God 1"), so
    // the number is stripped ONLY when the needle as typed matched nothing —
    // a real part-numbered title ("…The Way Of A Prophet 2" typed whole)
    // still wins on its own match first.
    const QString lower = citationNeedle(needle);
    QVariantMap best;
    int matches = 0;
    for (const auto &b : tableLibrary(library_)->BookIndex()) {
        if (b.chapters.empty())
            continue;
        const QString bookId = qstr(b.id);
        for (const auto &ch : b.chapters) {
            const QString cite = qstr(tableLibrary(library_)->Citation(bookId.toStdString(), ch.number, 0));
            if (cite.isEmpty())
                continue;
            if (citationNeedle(cite).indexOf(lower) < 0)
                continue;
            if (++matches > 1)
                return {};   // several sermons match: the dropdown lists them, the user picks
            best = { { QStringLiteral("bookId"), bookId },
                     { QStringLiteral("chapter"), ch.number },
                     { QStringLiteral("verseStart"), 0 },
                     { QStringLiteral("verseEnd"), 0 } };
        }
    }
    if (matches != 0)
        return matches == 1 ? best : QVariantMap{};
    // Zero matches as typed: strip one trailing " N" and re-search; a unique
    // sermon there jumps straight to its paragraph N.
    static const QRegularExpression trailingPara(QStringLiteral("^(.*\\S)\\s+(\\d{1,4})$"));
    const auto m = trailingPara.match(needle);
    if (!m.hasMatch())
        return {};
    const QString base = m.captured(1).trimmed();
    bool ok = false;
    const int para = m.captured(2).toInt(&ok);
    if (!ok || para <= 0 || base.size() < citationSearchMinChars(base))
        return {};
    const QString lowerBase = citationNeedle(base);
    QVariantMap paraBest;
    int paraMatches = 0;
    for (const auto &b : tableLibrary(library_)->BookIndex()) {
        if (b.chapters.empty())
            continue;
        const QString bookId = qstr(b.id);
        for (const auto &ch : b.chapters) {
            const QString cite = qstr(tableLibrary(library_)->Citation(bookId.toStdString(), ch.number, 0));
            if (cite.isEmpty())
                continue;
            if (citationNeedle(cite).indexOf(lowerBase) < 0)
                continue;
            if (++paraMatches > 1)
                return {};   // ambiguous even without the number: the dropdown handles it
            paraBest = { { QStringLiteral("bookId"), bookId },
                         { QStringLiteral("chapter"), ch.number },
                         { QStringLiteral("verseStart"), para },
                         { QStringLiteral("verseEnd"), para } };
        }
    }
    return paraMatches == 1 ? paraBest : QVariantMap{};
}

QVariantMap TheTableService::preview(const QString &bookId, int chapter, const QVariantList &verses) const
{
    // A SINGLE PARAGRAPH IS ANSWERED FROM THE SPLIT ALREADY CUT FOR IT when there is
    // one: this runs on the interface thread each time the operator picks a paragraph.
    if (verses.size() == 1)
        if (const auto warm = warmFor(bookId, chapter, verses.constFirst().toInt()))
            return SlideBuilder::previewOf(warm->slides, warm->hasPlaceholders);
    pf::BookSource source;
    if (!library_ || !EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(tableLibrary(library_), bookId, chapter, verses, source))
        return SlideBuilder::preview(templateId(), {}, {});   // (nothing to show: the empty shape)
    // (No `versionName` for a sermon: it used to be set to the tab's BRAND string
    // here, and since that field is "the Bible's version" it flowed into
    // {scripture_name} — a slot that means "King James Version" — and rendered the
    // app's own name onto the slide. A sermon has no Bible version; the family set
    // in sourceFor is what makes that answerable rather than a silent blank.)
    // The chosen template laid out by the SHARED slide builder - the same engine machinery Scripture uses, fed paragraphs instead of
    // verses, and shaped by the tab's own "table." options (splitting, per-slide counts).
    return SlideBuilder::preview(templateId(), source, SettingsService::instance().theTableSettings());
}

QVariantList TheTableService::slides(const QString &bookId, int chapter, const QVariantList &verses) const
{
    if (verses.size() == 1)
        if (const auto warm = warmFor(bookId, chapter, verses.constFirst().toInt()))
            return SlideBuilder::slidesOf(warm->slides, QStringLiteral("table"));
    pf::BookSource source;
    if (!library_ || !EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(tableLibrary(library_), bookId, chapter, verses, source))
        return {};
    // (No `versionName` — see preview() above: a sermon has no Bible version.)
    return SlideBuilder::slides(templateId(), source, SettingsService::instance().theTableSettings(),
                                QStringLiteral("table"));
}

// The Table's template chrome (the shared pane's template card + picker):
// its own setting key, its own content-type filter in the shared library.
QString TheTableService::templateId() const
{
    // The chosen layout - the default when nothing is chosen or the chosen one has been deleted.
    return SlideBuilder::templateId(kProfile);
}

QString TheTableService::defaultTemplateId() const { return QString::fromLatin1(kDefaultTemplate); }

QVariantList TheTableService::templates() const
{
    return SlideBuilder::templates(kProfile);
}

QString TheTableService::templateName(const QString &id) const
{
    return SlideBuilder::templateName(id);
}

bool TheTableService::templateHasValues(const QString &id) const
{
    return SlideBuilder::templateHasValues(id);
}

void TheTableService::setTemplate(const QString &id) const
{
    // An empty id means "back to the default layout" (the pane's × / reset).
    SettingsService::instance().setValue(QStringLiteral("table.template"), id);
}

bool TheTableService::newSermonFolder()
{
    VGR_CALL_TRACE("TheTableService.newSermonFolder");
    if (!EngineBridge::instance().booted() || !library_) {
        report(tr("The engine is still starting — try again in a moment."), QStringLiteral("warning"));
        return false;
    }
    if (importing_)
        return false;
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().SelectFolderDialog(
        tr("Pick the folder of sermons to import (every .pdf and .txt under it)").toStdString());
    if (!picked.ok() || !picked.value())
        return false;
    return importSermonFolder(QString::fromStdString(*picked.value()));
}

// The sweep itself, for a root that is already known: the picker above, and the
// env-gated self-test scenario (which drives this same path so the toast it
// reads is the one a hand would see). Everything below is unchanged by the
// split - the dialog only chose the root.
bool TheTableService::importSermonFolder(const QString &root)
{
    VGR_CALL_TRACE("TheTableService.importSermonFolder");
    if (!EngineBridge::instance().booted() || !library_) {
        report(tr("The engine is still starting — try again in a moment."), QStringLiteral("warning"));
        return false;
    }
    if (importing_)
        return false;   // one import at a time (shared with the single-file flow)
    if (root.isEmpty())
        return false;

    importing_ = true;
    emit importingChanged();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"), QStringLiteral("Folder import started: ") + root);
    report(tr("Importing every sermon under %1 …").arg(root), QStringLiteral("info"));

    // The walk + parse + save run on a real worker thread: QThread::create runs the functor THERE. (A lambda connected to started() with
    // `this` as the receiver would run on the GUI thread and freeze it.) Only the queued updates touch the service, on the GUI thread.
    // The library pointer outlives the thread (the service is a process-lifetime singleton) and the engine locks internally.
    const std::string rootUtf8 = root.toStdString();
    // Each PDF's clean text is kept beside the library (the_table_texts) so it can be opened and checked, and is reused next time.
    auto &platform = bps::platform::PlatformAccessor::Get();
    const std::string convertedRoot = platform.Filesystem().Join(platform.Paths().UserDataDir(), "the_table_texts");
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"), QStringLiteral("Converted text is kept in ") + QString::fromStdString(convertedRoot));
    auto *worker = QThread::create([this, rootUtf8, convertedRoot] {
        auto r = tableLibrary(library_)->ImportFolder(rootUtf8,
            [this](int done, int total, std::string_view current) {
                if (done == 1 || done == total || done % 100 == 0)   // (the engine log follows the import without a line per file)
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"), QStringLiteral("Folder import %1 / %2").arg(done).arg(total));
                if (done != 1 && done != total && done % 10 != 0)
                    return;   // the bar moves every tenth file, not with a queued event for each of a thousand
                const QString name = QString::fromStdString(std::string(current));
                QMetaObject::invokeMethod(this, [this, done, total, name] {
                    progress_ = { { QStringLiteral("done"), done }, { QStringLiteral("total"), total },
                                  { QStringLiteral("current"), name } };
                    emit progressChanged();
                }, Qt::QueuedConnection);
            }, convertedRoot);
        if (r.ok())
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"), QStringLiteral("Folder import done: %1 imported, %2 already in, %3 failed").arg(r.value().imported).arg(r.value().skipped).arg(r.value().failed));
        else
            EngineBridge::write(QStringLiteral("error"), QStringLiteral("Table"), QStringLiteral("Folder import failed: ") + QString::fromStdString(r.error().message));
        QMetaObject::invokeMethod(this, [this, r] {
            importing_ = false;
            emit importingChanged();
            if (r.ok()) {
                const auto &rep = r.value();
                // What the sweep DID, and what it would not take. A folder that
                // was mostly something else used to answer "40 imported" with
                // nothing said about the rest; the refused files are named now,
                // with the library's own reason (see ImportReport::rejected).
                QString summary = tr("Imported %1 sermons · %2 already in").arg(rep.imported).arg(rep.skipped);
                if (rep.failed > 0)
                    summary += tr(" · %1 failed").arg(rep.failed);
                if (rep.unsupported > 0)
                    summary += tr(" · %1 not sermon files").arg(rep.unsupported);
                report(summary, rep.imported > 0 ? QStringLiteral("success")
                                                 : (rep.failed + rep.unsupported > 0 ? QStringLiteral("warning")
                                                                                     : QStringLiteral("info")));
                if (!rep.rejected.empty()) {
                    // EVERY refused file, each with the library's own reason —
                    // not the first two. The list used to stop at two names
                    // while the count ran on ("40 not sermon files (and 38
                    // more)"), which told the operator a number and left them
                    // to guess which files it meant.
                    QStringList why;
                    for (const std::string &w : rep.rejected)
                        why << qstr(w);
                    // The notice is one line and elides when long; the log keeps
                    // the whole list verbatim, so nothing is lost either way.
                    EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Table"),
                                        QStringLiteral("Refused %1 file(s): ").arg(why.size())
                                        + why.join(QStringLiteral(" | ")));
                    report(tr("Skipped %1").arg(why.join(QStringLiteral("; "))), QStringLiteral("warning"));
                }
                if (rep.imported > 0)
                    startIndexing();   // the new sermons into the app-wide search (coalesced)
                libraryMoved();        // sermons landed: the words under the held cut may be theirs
            } else {
                report(qstr(r.error().message), QStringLiteral("error"));
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // Quitting while the import runs must not destroy a running thread: give it a moment to finish (the connection goes when the thread
    // object is deleted, so this never touches a dead pointer).
    // BOUNDED (GUI thread in aboutToQuit): 10s here froze the app past
    // Windows' ~5s hang threshold (AppHangB1 -> killed) when a large folder
    // import was still parsing. Imports are abandonable at quit.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, worker, [worker] { (void)worker->wait(2000); });
    worker->start();
    return true;
}

bool TheTableService::newSermon()
{
    VGR_CALL_TRACE("TheTableService.newSermon");
    if (!EngineBridge::instance().booted() || !library_) {
        report(tr("The engine is still starting — try again in a moment."), QStringLiteral("warning"));
        return false;
    }
    if (importing_)
        return false;   // one import at a time (shared with the folder import)
    const std::string title = tr("Add a sermon").toStdString();
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        // Sermon patterns ONLY, no "All files (*.*)": picking a spreadsheet
        // there was a guaranteed refusal (IsSermonFile reads the extension
        // first), so the filter is what makes the choice fail at PICK time
        // instead of at import — the picker is the only place the operator can
        // still change their mind for free.
        title, { std::string("Sermons (*.txt *.pdf)") });
    if (!picked.ok() || !picked.value())
        return false;
    const std::string path = *picked.value();
    const QString name = QFileInfo(QString::fromStdString(path)).fileName();

    // Read + parse OFF the GUI thread: ImportSermon parses the whole document
    // (a large PDF is not a UI-speed operation), so doing it inline here
    // stalled the UI on every "Add a sermon" — the same reason the folder
    // import beside it already runs on a worker. The library pointer outlives
    // the thread (process-lifetime singleton) and the engine locks internally.
    // Returns true = the import STARTED (the QML discards the value; the toast
    // reports the outcome).
    importing_ = true;
    emit importingChanged();
    auto *worker = QThread::create([this, path, name] {
        QFile file(QString::fromStdString(path));
        if (!file.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(this, [this, name] {
                importing_ = false;
                emit importingChanged();
                report(tr("Couldn't read %1.").arg(name), QStringLiteral("error"));
            }, Qt::QueuedConnection);
            return;
        }
        const QByteArray bytes = file.readAll();
        auto r = tableLibrary(library_)->ImportSermon(
            path, std::string_view(bytes.constData(), size_t(bytes.size())));
        const QString failure = r.ok() ? QString() : qstr(r.error().message);
        const QString added = r.ok() ? qstr(r.value()) : QString();
        QMetaObject::invokeMethod(this, [this, name, failure, added] {
            importing_ = false;
            emit importingChanged();
            if (!failure.isEmpty()) {
                report(failure, QStringLiteral("error"));
                return;
            }
            report(tr("Added %1 · %2").arg(name, added), QStringLiteral("success"));
            startIndexing();   // the new sermon into the app-wide search (coalesced)
            libraryMoved();    // a sermon landed: the words under the held cut may be theirs
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // Quitting mid-import must not destroy a running thread (same as the folder
    // import), and must not freeze the UI waiting for it (see the folder path).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, worker,
            [worker] { (void)worker->wait(2000); });
    worker->start();
    return true;
}

// Re-upserts every sermon document into the platform Search Engine (an import
// landed). The engine's IndexDocument is an incremental upsert, so other
// content's documents are never touched.
// ---------------------------------------------------------------------------
// The background indexer. ONE joinable thread serves every pass — loadLibrary's
// first index and each post-import reindex — where the three detached threads
// here before were the 0xc0000005 on quit: an orphaned pass kept upserting into
// the platform Search Engine while the kernel was tearing it down (the second
// app instance's shutdown walked the first one's indexer into a destroyed
// std::_Hashtable). Now: every indexWorker_ mutation happens on the GUI thread,
// the walk polls indexCancel_ between documents (TheTableLibrary), and
// shutdownIndexing() cancels + joins BEFORE the engine's own teardown.
// ---------------------------------------------------------------------------
void TheTableService::startIndexing()
{
    if (!library_ || !EngineBridge::instance().booted())
        return;
    if (indexWorker_.joinable()) {
        // A pass is already out: each run re-snapshots the library under the
        // engine's lock, so a request landing mid-pass is served by the pass
        // this one schedules when it ends (indexPassDone).
        indexReindex_.store(true, std::memory_order_relaxed);
        return;
    }
    indexCancel_.store(false, std::memory_order_relaxed);
    indexReindex_.store(false, std::memory_order_relaxed);
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StartupTrace"),
                        QStringLiteral("The Table search indexing worker starting"));
    indexWorker_ = std::thread([this] {
        // A pass tokenizes the whole library (tens of seconds of pure CPU and allocator
        // work). The interface and the output feed must win every contest for the cores and
        // the heap, so the pass runs below normal priority and simply takes longer when the
        // machine is busy. Through the PAL (level 2 = below normal: SetThreadPriority on Windows, a
        // nice value on Linux/macOS) — this used to be a Win32 call behind Q_OS_WIN, so on the other
        // two OSes the indexing pass ran at full priority and fought the output feed for the cores.
        (void)bps::platform::PlatformAccessor::Get().Threading().SetCurrentThreadPriority(2);
        do {
            indexReindex_.store(false, std::memory_order_relaxed);
            if (indexCancel_.load(std::memory_order_relaxed))
                break;
            const auto r = tableLibrary(library_)->IndexWithSearchEngine(&indexCancel_);
            if (indexCancel_.load(std::memory_order_relaxed))
                break;   // quitting — no log write into a tearing-down process
            if (!r.ok())
                break;   // the engine logged the failure; never spin on a broken engine
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"),
                                QStringLiteral("Sermons indexed for the app-wide search"));
        } while (indexReindex_.exchange(false, std::memory_order_relaxed));
        // GUI-thread bookend: joins the exited thread and serves any request
        // that landed after the loop's last check (a reindex between the final
        // exchange() and the thread's last statement would otherwise be lost).
        QMetaObject::invokeMethod(this, &TheTableService::indexPassDone,
                                  Qt::QueuedConnection);
    });
}

void TheTableService::indexPassDone()
{
    if (indexWorker_.joinable())
        indexWorker_.join();   // the worker has run to completion; this only reaps it
    if (indexReindex_.exchange(false, std::memory_order_relaxed))
        startIndexing();
}

// aboutToQuit, BEFORE EngineBridge::shutdown (main.cpp): set the cancel flag —
// the walk ends within one document — then join. Never blocks meaningfully; the
// engine's Search Engine outlives the very last upsert because of this call.
void TheTableService::shutdownIndexing()
{
    indexCancel_.store(true, std::memory_order_relaxed);
    if (indexWorker_.joinable())
        indexWorker_.join();
    indexCancel_.store(false, std::memory_order_relaxed);
    indexReindex_.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// User data — the ENGINE's notes and highlights (bps::library::TheTableLibrary,
// persisted in the library JSON), the ScriptureService calls so ONE pane UI
// serves both tabs. Every entry point resolves the reference through the
// service's own resolve() (year/paragraph shapes AND citation lines); a bad
// reference is a false/empty answer, never a crash.
// ---------------------------------------------------------------------------
namespace {

struct ResolvedSpot
{
    QString bookId;
    int chapter = 0;
    int verse = 0;
};

// The reference as a paragraph spot, or nullopt when it doesn't land on one.
std::optional<ResolvedSpot> resolvedSpot(const QString &reference)
{
    if (!EngineBridge::instance().booted())
        return std::nullopt;
    const QVariantMap ref = TheTableService::instance().resolve(reference);
    if (ref.isEmpty() || !ref.contains(QStringLiteral("bookId")))
        return std::nullopt;
    ResolvedSpot spot;
    spot.bookId = ref.value(QStringLiteral("bookId")).toString();
    spot.chapter = ref.value(QStringLiteral("chapter")).toInt();
    spot.verse = ref.value(QStringLiteral("verseStart")).toInt();
    if (spot.verse <= 0)   // sermon-level: notes live on paragraphs
        return std::nullopt;
    return spot;
}

QVariantMap noteToVariant(const bl::TableNote &n)
{
    return QVariantMap{
        { QStringLiteral("reference"), qstr(n.ref) },
        { QStringLiteral("bookId"), qstr(n.bookId) },
        { QStringLiteral("chapter"), n.chapter },
        { QStringLiteral("verse"), n.verse },
        { QStringLiteral("text"), qstr(n.text) },
        { QStringLiteral("modifiedMs"), qint64(n.modifiedMs) },
    };
}

} // namespace

bool TheTableService::setNote(const QString &reference, const QString &text)
{
    auto spot = resolvedSpot(reference);
    if (!spot)
        return false;
    // An empty text REMOVES the note (the delete gesture).
    auto r = text.trimmed().isEmpty()
                 ? tableLibrary(library_)->RemoveNote(spot->bookId.toStdString(), spot->chapter,
                                                      spot->verse)
                 : tableLibrary(library_)->AddNote(spot->bookId.toStdString(), spot->chapter,
                                                   spot->verse, text.toStdString());
    if (!r.ok())
        return false;
    emit userDataChanged();
    emit changed();
    return true;
}

QString TheTableService::note(const QString &reference) const
{
    auto spot = resolvedSpot(reference);
    if (!spot)
        return {};
    auto r = tableLibrary(library_)->Notes(spot->bookId.toStdString(), spot->chapter, spot->verse);
    if (!r.ok() || r.value().empty())
        return {};
    return qstr(r.value().front().text);
}

QVariantList TheTableService::notes() const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto r = tableLibrary(library_)->Notes();
    if (!r.ok())
        return out;
    for (const bl::TableNote &n : r.value())
        out.append(noteToVariant(n));
    return out;
}

bool TheTableService::setHighlighted(const QString &reference, bool on)
{
    auto spot = resolvedSpot(reference);
    if (!spot)
        return false;
    auto r = tableLibrary(library_)->SetHighlight(spot->bookId.toStdString(), spot->chapter,
                                                  spot->verse, on);
    if (!r.ok())
        return false;
    emit userDataChanged();
    emit changed();
    return true;
}

bool TheTableService::isHighlighted(const QString &reference) const
{
    auto spot = resolvedSpot(reference);
    if (!spot)
        return false;
    auto r = tableLibrary(library_)->IsHighlighted(spot->bookId.toStdString(), spot->chapter,
                                                   spot->verse);
    return r.ok() && r.value();
}

QVariantList TheTableService::highlights() const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto r = tableLibrary(library_)->Highlights();
    if (!r.ok())
        return out;
    for (const std::string &key : r.value())
        out.append(qstr(key));
    return out;
}

// ---- id-shaped reads -------------------------------------------------------
// The pane knows the book's engine id ("Y1953") and the paragraph number —
// no reference synthesis in QML (the year-book grammar lives HERE, once).

bool TheTableService::isVerseHighlighted(const QString &bookId, int chapter, int verse) const
{
    if (!library_ || !EngineBridge::instance().booted())
        return false;
    auto r = tableLibrary(library_)->IsHighlighted(bookId.toStdString(), chapter, verse);
    return r.ok() && r.value();
}

QString TheTableService::verseNote(const QString &bookId, int chapter, int verse) const
{
    if (!library_ || !EngineBridge::instance().booted())
        return {};
    auto r = tableLibrary(library_)->Notes(bookId.toStdString(), chapter, verse);
    if (!r.ok() || r.value().empty())
        return {};
    return qstr(r.value().front().text);
}

QVariantMap TheTableService::chapterUserData(const QString &bookId, int chapter) const
{
    QVariantMap out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    // ONE engine sweep per open sermon: every note and highlight landing on
    // this chapter's paragraphs, keyed "<verse>" for the rows.
    const std::string prefix = bookId.toStdString() + ":" + std::to_string(chapter) + ":";
    auto marks = tableLibrary(library_)->Highlights();
    if (marks.ok()) {
        for (const std::string &key : marks.value()) {
            if (key.rfind(prefix, 0) != 0)
                continue;
            const int verse = std::atoi(key.c_str() + prefix.size());
            if (verse <= 0)
                continue;
            const QString qkey = QString::number(verse);
            QVariantMap mark = out.value(qkey).toMap();   // QVariant: merge explicitly
            mark[QStringLiteral("highlight")] = true;
            out[qkey] = mark;
        }
    }
    auto all = tableLibrary(library_)->Notes();
    if (all.ok()) {
        for (const bl::TableNote &n : all.value()) {
            if (n.bookId != bookId.toStdString() || n.chapter != chapter || n.verse <= 0)
                continue;
            const QString qkey = QString::number(n.verse);
            QVariantMap mark = out.value(qkey).toMap();
            mark[QStringLiteral("note")] = qstr(n.text);
            out[qkey] = mark;
        }
    }
    return out;
}
