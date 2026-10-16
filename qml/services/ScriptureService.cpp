#include "services/ScriptureService.h"

#include "modules/bible/BibleEngine.hpp"
#include "modules/bible/BibleTypes.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/search/TextMatching.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/SettingsService.h"
#include "services/SlideBuilder.h"
#include "services/ShowConverter.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>

#include <QJSEngine>
#include <QQmlEngine>

namespace bb = bps::bible;
namespace pf = bps::presentation;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The query's words, lower-cased (a word = a letter/digit run) — what the
// padlock's whole-word filter tests each verse against.
QStringList queryWords(const QString &text)
{
    QStringList out;
    QString cur;
    for (const QChar &c : text) {
        if (c.isLetterOrNumber()) cur += c.toLower();
        else if (!cur.isEmpty()) { out << cur; cur.clear(); }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

// The engine's default scripture template (the id the template catalog gives the plain "Scripture" layout).
constexpr const char *kDefaultTemplate = "tpl-scripture";

// The session key that keeps the parallel-version selection (declared in AppSettings: an
// undeclared key refuses every write).
constexpr const char *kParallelKey = "session.scriptureParallelBibles";
// The vocabulary stops at the FOURTH Bible ({scripture4_text}), so three extras is
// the whole of what a layout can ask for.
constexpr int kMaxParallel = 3;

// The stored extra versions, in slot order: an empty entry or a duplicate is dropped
// (a hand-edited settings file must not shift every column by one).
QStringList storedParallelBibles()
{
    QStringList out;
    const QString stored = SettingsService::instance().value(QString::fromLatin1(kParallelKey)).toString();
    for (const QString &part : stored.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString id = part.trimmed();
        if (!id.isEmpty() && !out.contains(id) && out.size() < kMaxParallel)
            out << id;
    }
    return out;
}

// Every id this app can actually read, so a stored selection cannot point at a Bible
// that has since been removed.
QStringList installedBibleIds()
{
    QStringList out;
    if (!EngineBridge::instance().booted())
        return out;
    for (const std::string &id : bb::BibleEngine::Instance().BibleIds())
        out << QString::fromStdString(id);
    return out;
}
// This tab's place in the shared slide builder (its templates, its setting).
const SlideBuilder::Profile kProfile{ "scripture", "scripture.template", kDefaultTemplate };

std::vector<int> numbersOf(const QVariantList &verses)
{
    std::vector<int> out;
    for (const QVariant &v : verses)
        out.push_back(v.toInt());
    return out;
}

// The picked verses of one chapter as the slide builder takes them. False when the Bible / book / chapter is not there.
//
// `parallelBibles` is passed IN rather than read here (the default keeps every
// GUI-side caller exactly as it was): the stored extra versions live in
// SettingsService, and the chapter's background load builds this same source on a
// WORKER thread to cut the chapter ahead of the click — a QML singleton's state is
// not a worker-thread surface, so the ids are captured on the GUI thread instead.
bool sourceFor(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses,
               pf::BookSource &out, const QStringList &parallelBibles = storedParallelBibles())
{
    auto &engine = bb::BibleEngine::Instance();
    const std::string bible = bibleId.toStdString();
    auto meta = engine.Metadata(bible);
    auto book = engine.GetBook(bible, bookId.toStdString());
    auto ch = engine.GetChapter(bible, bookId.toStdString(), chapter);
    if (!meta.ok() || !book.ok() || !ch.ok())
        return false;
    out = pf::BookSource{};
    // WHICH FAMILY this selection is, NAMED (BookFill::ContentFamily): the filler
    // reads it to shape the reference (a Bible joins with a colon and writes its
    // chapter) and to answer only the tokens a Bible has one for. No branch
    // downstream has to infer "a Bible" from the presence of a chapter.
    out.family = pf::ContentFamily::Scripture;
    out.versionName = meta.value().name;
    out.copyright = meta.value().copyright;
    out.book = book.value().name;
    out.bookAbbr = book.value().aliases.empty() ? book.value().id : book.value().aliases.front();
    out.chapter = chapter;
    const std::vector<int> wanted = numbersOf(verses);
    for (const bb::BibleVerse &v : ch.value().verses)
        if (std::find(wanted.begin(), wanted.end(), v.verse) != wanted.end())
            out.items.push_back({ v.verse, v.text });
    // EVERY OTHER SELECTED VERSION'S OWN VERSES OF THE SAME PASSAGE (BookFill's
    // ParallelBible), so a parallel layout's second, third and fourth columns carry
    // that translation's wording of the very verses the first column carries. A
    // version that has gone missing, or whose book ids do not line up with this
    // Bible's, simply contributes no column — the layout's slot then renders empty
    // rather than the wrong verses.
    for (const QString &extraId : parallelBibles) {
        if (out.parallels.size() >= static_cast<size_t>(kMaxParallel) || extraId == bibleId)
            continue;   // the same file twice is not a second version
        const std::string extra = extraId.toStdString();
        auto extraMeta = engine.Metadata(extra);
        auto extraChapter = engine.GetChapter(extra, bookId.toStdString(), chapter);
        if (!extraMeta.ok() || !extraChapter.ok())
            continue;
        pf::ParallelBible column;
        column.name = extraMeta.value().name;
        if (auto extraBook = engine.GetBook(extra, bookId.toStdString()); extraBook.ok())
            column.bookAbbr = extraBook.value().aliases.empty() ? extraBook.value().id
                                                                : extraBook.value().aliases.front();
        else
            column.bookAbbr = book.value().aliases.empty() ? book.value().id : book.value().aliases.front();
        for (const bb::BibleVerse &v : extraChapter.value().verses)
            if (std::find(wanted.begin(), wanted.end(), v.verse) != wanted.end())
                column.items.push_back({ v.verse, v.text });
        if (!column.items.empty())
            out.parallels.push_back(std::move(column));
    }
    return true;
}

} // namespace

ScriptureService &ScriptureService::instance()
{
    static ScriptureService s;
    return s;
}

ScriptureService *ScriptureService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

ScriptureService::ScriptureService(QObject *parent) : QObject(parent)
{
    // THE WORDS OR THE BLOCKS MOVED: the cut was made from something that is no longer
    // there, and its KEY cannot see that — a Bible is reinstalled under the same id, a
    // template is edited in place. So the cut goes here, by SOURCE (this is not a rule on
    // `changed` as a whole: a settings write reaching `changed` usually changes nothing
    // the cut depends on), and the chapter on screen is cut again — emitting `changed` is
    // what the re-cut below hangs off.
    const auto textMoved = [this] {
        cutStale_ = true;
        SlideBuilder::dropWarm();
        // A Bible can be replaced under the same id (a reinstall, a new file for
        // an installed one), so the published corpus answers go with it — every
        // one of them is stale the moment this fires.
        dropCorpusCaches();
        emit changed();
    };
    connect(&SearchService::instance(), &SearchService::bibleChanged, this, textMoved);
    connect(&TemplateLibraryService::instance(), &DesignLibraryService::changed, this, textMoved);
    // A SETTINGS WRITE IS NOT THAT. Everything the division depends on is IN the key —
    // every option, and the parallel versions on show — and the key is compared again at
    // every use. So the writes an operator makes while working (a highlight, a note, a
    // column dragged, another tab's option) must not throw the cut away: that would serve
    // the next click on the interface thread for nothing, the very cost this path exists to
    // remove. Only the pane's own re-derive hangs off them, exactly as before.
    connect(&SettingsService::instance(), &SettingsService::changed, this, &ScriptureService::changed);
    // THE CHAPTER ON SCREEN IS CUT AGAIN whenever the cut behind it is no longer the right
    // one — the key moved (a template, an option, a parallel version), or the words under it
    // did (see textMoved above). The verse list's continuations ARE that cut's rows, so a
    // change here has to reach them: without this the list would keep showing the old
    // division until the chapter was reopened. The re-cut is the SAME background pass the
    // open made (see chapterAsync), so nothing of it lands on the interface thread.
    connect(this, &ScriptureService::changed, this, [this] {
        if (!cutKey_ || (!cutStale_ && chapterKey(cutSourceId_, cutBookId_, cutChapter_) == *cutKey_))
            return;
        cutStale_ = false;
        SlideBuilder::dropWarm();   // what is held is not this chapter's cut any more
        chapterAsync(cutSourceId_, cutBookId_, cutChapter_, cutToken_);
    });
}

bool ScriptureService::loading() const { return SearchService::instance().bibleLoading(); }

// THE PUBLISHED READS (see the header): each entry point answers from what a
// worker has already published and queues the read when it has nothing yet. The
// `read*` functions below them are the old bodies, unchanged — the engine work,
// called from a worker.
QVariantList ScriptureService::bibles() const
{
    if (!biblesInFlight_ && biblesCache_.isEmpty())
        refreshBiblesOffThread();
    return biblesCache_;
}

QVariantList ScriptureService::books(const QString &bibleId) const
{
    if (bibleId.isEmpty())
        return {};
    const auto it = booksCache_.constFind(bibleId);
    if (it == booksCache_.constEnd()) {
        refreshBooksOffThread(bibleId);
        return {};
    }
    return *it;
}

QVariantList ScriptureService::chapter(const QString &bibleId, const QString &bookId, int chapter) const
{
    if (bibleId.isEmpty() || bookId.isEmpty() || chapter <= 0)
        return {};
    const auto it = chapterCache_.constFind(chapterCacheKey(bibleId, bookId, chapter));
    if (it == chapterCache_.constEnd()) {
        refreshChapterOffThread(bibleId, bookId, chapter);
        return {};
    }
    return *it;
}

QString ScriptureService::chapterCacheKey(const QString &bibleId, const QString &bookId, int chapter)
{
    return bibleId + QLatin1Char('\n') + bookId + QLatin1Char('\n') + QString::number(chapter);
}

QString ScriptureService::referenceCacheKey(const QString &bibleId, const QString &text)
{
    return bibleId + QLatin1Char('\n') + text.trimmed();
}

void ScriptureService::dropCorpusCaches() const
{
    biblesCache_.clear();
    booksCache_.clear();
    chapterCache_.clear();
    referenceCache_.clear();
    searchCache_.clear();
    // The drop is a GENERATION, not just an erasure: a read that started before it
    // is describing a corpus that no longer exists, and publishing it would fill
    // the cache the caller's re-read has already looked at (see corpusGeneration_
    // in the header — each refresh below drops such an answer and reads again).
    ++corpusGeneration_;
}

// CORPUS-CACHE DIAGNOSTIC. The published reads have a failure mode nothing else
// in this chain can show: the pane asks for an outline, the read is declined or
// its answer is empty, and the pane renders the SAME empty column as before —
// with no error and no second chance until something else emits. One line per
// COLD read (a miss, a decline, an answer), never per binding evaluation, so it
// cannot spam: `build/logs/engine.log` then says whether the outline was asked
// for, whether a worker took it, and how many rows came back.
static void corpusDiag(const QString &message)
{
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScriptureCorpus"), message);
}

// ---- the workers (one read at a time per key, answers marshalled back) -------
void ScriptureService::refreshBiblesOffThread() const
{
    if (biblesInFlight_ || !EngineBridge::instance().booted())
        return;
    biblesInFlight_ = true;
    const int gen = corpusGeneration_;   // the corpus this read describes (see dropCorpusCaches)
    corpusDiag(QStringLiteral("bibles: read queued"));
    auto *runner = QThread::create([this, gen] {
        const QVariantList rows = readBibles();
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this), [this, gen, rows]() {
            biblesInFlight_ = false;
            if (gen != corpusGeneration_) {
                // The corpus moved under this read: drop the answer and read again,
                // so the re-read the drop announced gets the new world rather than
                // one that would never be re-asked (the cache is no longer a miss).
                refreshBiblesOffThread();
                return;
            }
            biblesCache_ = rows;
            corpusDiag(QStringLiteral("bibles: %1 answered").arg(rows.size()));
            // The const_cast is the .cpp's standing idiom for a signal from a
            // const reader (see publishMarks): the reader is const because it
            // mutates nothing visible, not because the answer cannot be
            // published.
            emit const_cast<ScriptureService *>(this)->changed();   // the pane's sources binding re-reads
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::refreshBooksOffThread(const QString &bibleId) const
{
    if (booksInFlight_.contains(bibleId))
        return;   // already reading; the pane keeps waiting on that answer
    if (!EngineBridge::instance().booted()) {
        corpusDiag(QStringLiteral("books '%1': read DECLINED — the engine is not booted yet").arg(bibleId));
        return;
    }
    booksInFlight_.insert(bibleId);
    const int gen = corpusGeneration_;   // the corpus this read describes (see dropCorpusCaches)
    corpusDiag(QStringLiteral("books '%1': read queued").arg(bibleId));
    auto *runner = QThread::create([this, bibleId, gen] {
        const QVariantList rows = readBooks(bibleId);
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this), [this, bibleId, gen, rows]() {
            booksInFlight_.remove(bibleId);
            if (gen != corpusGeneration_) {
                // See refreshBiblesOffThread: the outline describes a corpus that
                // moved, so it is dropped and read again instead of published.
                refreshBooksOffThread(bibleId);
                return;
            }
            booksCache_.insert(bibleId, rows);
            corpusDiag(QStringLiteral("books '%1': %2 answered").arg(bibleId).arg(rows.size()));
            emit const_cast<ScriptureService *>(this)->changed();   // the pane's openSource re-reads the outline
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::refreshChapterOffThread(const QString &bibleId, const QString &bookId,
                                               int chapter) const
{
    const QString key = chapterCacheKey(bibleId, bookId, chapter);
    if (chapterInFlight_.contains(key) || !EngineBridge::instance().booted())
        return;
    chapterInFlight_.insert(key);
    const int gen = corpusGeneration_;   // the corpus this read describes (see dropCorpusCaches)
    auto *runner = QThread::create([this, bibleId, bookId, chapter, key, gen] {
        const QVariantList rows = readChapter(bibleId, bookId, chapter);
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this),
                                  [this, bibleId, bookId, chapter, key, gen, rows]() {
            chapterInFlight_.remove(key);
            if (gen != corpusGeneration_) {
                // See refreshBiblesOffThread: drop the stale rows, read again.
                refreshChapterOffThread(bibleId, bookId, chapter);
                return;
            }
            chapterCache_.insert(key, rows);
            emit const_cast<ScriptureService *>(this)->chapterCached(bibleId, bookId, chapter);
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::refreshReferenceOffThread(const QString &bibleId, const QString &text) const
{
    const QString key = referenceCacheKey(bibleId, text);
    if (referenceInFlight_.contains(key) || !EngineBridge::instance().booted())
        return;
    referenceInFlight_.insert(key);
    const int gen = corpusGeneration_;   // the corpus this read describes (see dropCorpusCaches)
    auto *runner = QThread::create([this, bibleId, text, key, gen] {
        const QVariantMap ref = readResolve(text, bibleId);
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this), [this, bibleId, text, key, gen, ref]() {
            referenceInFlight_.remove(key);
            if (gen != corpusGeneration_) {
                // See refreshBiblesOffThread: drop the stale resolution, ask again.
                refreshReferenceOffThread(bibleId, text);
                return;
            }
            referenceCache_.insert(key, ref);
            // the pane re-runs its completion/jump
            emit const_cast<ScriptureService *>(this)->referenceCached(bibleId, text);
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::refreshSearchOffThread(const QString &text, const QString &bibleId, int limit,
                                              bool exactWords, const QString &scopeBookId,
                                              int scopeChapter) const
{
    const QString key = QString::number(limit) + QLatin1Char('\n') + (exactWords ? QStringLiteral("1") : QStringLiteral("0"))
                        + QLatin1Char('\n') + scopeBookId + QLatin1Char('\n') + QString::number(scopeChapter)
                        + QLatin1Char('\n') + bibleId + QLatin1Char('\n') + text.trimmed();
    if (searchInFlight_.contains(key) || !EngineBridge::instance().booted())
        return;
    searchInFlight_.insert(key);
    const int gen = corpusGeneration_;   // the corpus this read describes (see dropCorpusCaches)
    auto *runner = QThread::create([this, text, bibleId, limit, exactWords, scopeBookId, scopeChapter, key, gen] {
        const QVariantList rows = readSearch(text, bibleId, limit, exactWords, scopeBookId, scopeChapter);
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this), [this, text, bibleId, limit, exactWords, scopeBookId, scopeChapter, key, gen, rows]() {
            searchInFlight_.remove(key);
            if (gen != corpusGeneration_) {
                // See refreshBiblesOffThread: the hits describe a corpus that moved.
                refreshSearchOffThread(text, bibleId, limit, exactWords, scopeBookId, scopeChapter);
                return;
            }
            searchCache_.insert(key, rows);
            // The sync search()'s answer has landed: what re-asks it (see the
            // header — the caller of that entry point has no token to match).
            emit const_cast<ScriptureService *>(this)->searchCached(text.trimmed());
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

// ---- the reads themselves (off the interface thread) ------------------------
QVariantList ScriptureService::readBibles() const
{
    QVariantList out;
    if (!EngineBridge::instance().booted())
        return out;
    auto &engine = bb::BibleEngine::Instance();
    for (const std::string &id : engine.BibleIds()) {
        auto meta = engine.Metadata(id);
        if (!meta.ok())
            continue;
        auto count = engine.VerseCount(id);
        out.append(QVariantMap{
            { QStringLiteral("id"), qstr(id) },
            { QStringLiteral("name"), qstr(meta.value().name.empty() ? id : meta.value().name) },
            { QStringLiteral("abbreviation"), qstr(meta.value().abbreviation.empty() ? id : meta.value().abbreviation) },
            { QStringLiteral("language"), qstr(meta.value().language) },
            { QStringLiteral("verseCount"), count.ok() ? static_cast<qlonglong>(count.value()) : 0 },
        });
    }
    return out;
}

QVariantList ScriptureService::readBooks(const QString &bibleId) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted())
        return out;
    auto outline = bb::BibleEngine::Instance().Outline(bibleId.toStdString());
    if (!outline.ok())
        return out;
    for (const bb::BookOutline &o : outline.value()) {
        QVariantList chapters, counts;
        for (int c : o.chapters) chapters.append(c);
        for (int v : o.verseCounts) counts.append(v);
        out.append(QVariantMap{
            { QStringLiteral("id"), qstr(o.book.id) },
            { QStringLiteral("name"), qstr(o.book.name) },
            { QStringLiteral("testament"), qstr(o.book.testament) },
            { QStringLiteral("chapters"), chapters },
            { QStringLiteral("verseCounts"), counts },
        });
    }
    return out;
}

QVariantList ScriptureService::readChapter(const QString &bibleId, const QString &bookId, int chapter) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted())
        return out;
    auto ch = bb::BibleEngine::Instance().GetChapter(bibleId.toStdString(), bookId.toStdString(), chapter);
    if (!ch.ok())
        return out;
    for (const bb::BibleVerse &v : ch.value().verses)
        out.append(QVariantMap{ { QStringLiteral("number"), v.verse }, { QStringLiteral("text"), qstr(v.text) },
                                { QStringLiteral("heading"), qstr(v.heading) } });
    return out;
}

void ScriptureService::chapterAsync(const QString &bibleId, const QString &bookId, int chapter, int token)
{
    // Everything the warm needs that lives on the GUI thread is read HERE, before the
    // worker starts: the template's blocks and the stored extra versions are QML
    // service state, and a worker thread is no place to touch it. The key is built the
    // same way the click will build it, which is what makes the answer reusable.
    const SlideBuilder::ChapterKey key = chapterKey(bibleId, bookId, chapter);
    const std::vector<pf::ContentBlock> templateBlocks = SlideBuilder::templateBlocks(key.templateId);
    const QStringList parallelIds = storedParallelBibles();
    // THE NEIGHBOURS' KEYS, built here for the same reason `key` is (see the note above):
    // they are what the worker reads and cuts next, and they have to be the very keys a
    // step will ask for when it arrives, or the read-ahead is wasted. A number below 1
    // is not a chapter, and an end chapter simply reads as empty below.
    std::vector<SlideBuilder::ChapterKey> neighbourKeys;
    {
        const int candidates[2] = { chapter - 1, chapter + 1 };
        for (const int other : candidates)
            if (other > 0)
                neighbourKeys.push_back(chapterKey(bibleId, bookId, other));
    }
    // The generation the read happens under: a load that was already reading when the text
    // under it was replaced is dropped rather than published (SlideBuilder::WarmRequest).
    const quint64 epoch = SlideBuilder::warmEpoch();
    // What is on screen, and the key it was cut with: the guard the changed handler
    // above asks before cutting the chapter a second time. The Bible is part of it — a
    // tab can switch translations, and the other one's chapter is a different chapter.
    cutSourceId_ = bibleId;
    cutBookId_ = bookId;
    cutChapter_ = chapter;
    cutToken_ = token;
    cutKey_ = key;
    auto *runner = QThread::create([this, bibleId, bookId, chapter, token, key, templateBlocks, parallelIds,
                                    neighbourKeys, epoch] {
        // READ AHEAD FIRST. A chapter a step already brought in is held whole — its words
        // AND its cut — so this load needs no engine read at all (SlideBuilder::warmedRows).
        // The READ is the expensive half of a step; the cut is milliseconds. Skipping it is
        // what makes stepping to the next passage immediate instead of a second away.
        QVariantList rows = SlideBuilder::warmedRows(key);
        const bool readAhead = !rows.isEmpty();
        if (!readAhead)
            rows = this->readChapter(bibleId, bookId, chapter);
        // THE HEAD OF THE CHAPTER IS CUT BEFORE IT IS CLICKED (SlideBuilder::warmChapter):
        // the rows the pane can paint, into their slides, on THIS thread — so choosing one
        // while presenting is a lookup instead of a read and a fill on the interface
        // thread. The chapter is read ONCE for all of them (BookFill::ItemOf narrows that
        // read per item).
        //
        // AND ONLY THE HEAD, deliberately: the cut is priced per item, so cutting a whole
        // book before it can be seen made an open cost scale with the passage's length
        // (measured: 64 ms of fill for a 277-paragraph sermon whose list shows the first
        // screenful). The rest is cut below, after the rows are on their way.
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
            if (!numbers.isEmpty() && sourceFor(bibleId, bookId, chapter, numbers, all, parallelIds)) {
                // Holding a chapter that is held ALREADY (same key) cuts nothing: the same
                // key is the same answer, so a step into a read-ahead chapter is only the
                // pin moving (SlideBuilder::warmChapter).
                SlideBuilder::warmChapter(
                    SlideBuilder::WarmRequest{ epoch, key, templateBlocks, all, rows });
                warmed = SlideBuilder::warmedItemCount(key);
                // The chapter's own rows carry that cut, so the list reaches the screen
                // already divided (SlideBuilder::attachSplit) — same thread, same key,
                // nothing left for the interface thread to work out.
                SlideBuilder::attachSplit(rows, key);
                cut = true;
            }
            warmMs = clock.elapsed();
        }
        QMetaObject::invokeMethod(this, [this, bibleId, bookId, chapter, token, rows, warmed, warmMs, readAhead] {
            if (token != latestChapterToken_)
                return;
            if (warmed > 0) {
                // ONE line per chapter: what was cut ahead of the click, and how long
                // that took OFF the interface thread. Paired with the first-pick lines
                // warmFor logs, it says whether the pre-cut is carrying the picks — and a
                // chapter that arrived without a read says so, which is the step being
                // served from the read-ahead rather than from the engine.
                warmServeLogged_ = false;
                warmMissLogged_ = false;
                EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                    readAhead
                                        ? QStringLiteral("%1 %2: served from the chapter read and cut ahead of this step — no read and no fill for it (off the interface thread)")
                                              .arg(bookId, QString::number(chapter))
                                        : QStringLiteral("%1 %2: cut the first %3 item(s) ahead of the click in %4 ms, so the list opens divided (the rest is cut with the rows already on screen)")
                                              .arg(bookId, QString::number(chapter), QString::number(warmed),
                                                   QString::number(warmMs)));
            }
            emit chapterReady(token, bibleId, bookId, chapter, rows);
        }, Qt::QueuedConnection);

        // ---- THE REST OF THE CHAPTER, once those rows are on their way ---------
        // The head covers what the operator can see; every item below it still has to be
        // cut, because the list shows a paragraph's continuations the moment the chapter
        // is open (SlideBuilder::attachSplit) — that division is the reported bug. So the
        // remainder is cut HERE, in the same worker, and the rows are handed over a second
        // time when it lands. The open never waits for this: the rows it needs are already
        // posted, and this runs behind them.
        if (cut && latestChapterToken_.load() == token) {
            const int headItems = warmed;
            QElapsedTimer restClock;
            restClock.start();
            const bool more = SlideBuilder::warmChapterMore(
                SlideBuilder::WarmRequest{ epoch, key, templateBlocks, all, rows });
            const qint64 restMs = restClock.elapsed();
            if (more && latestChapterToken_.load() == token) {
                // The same rows, now carrying the cut of every item in them.
                SlideBuilder::attachSplit(rows, key);
                const int wholeChapter = SlideBuilder::warmedItemCount(key);
                QMetaObject::invokeMethod(this, [this, bibleId, bookId, chapter, token, rows, restMs,
                                                 wholeChapter, headItems] {
                    if (token != latestChapterToken_)
                        return;
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                        QStringLiteral("%1 %2: the remaining %3 item(s) cut in %4 ms, with the head's %5 already on screen")
                                            .arg(bookId, QString::number(chapter))
                                            .arg(QString::number(wholeChapter - headItems))
                                            .arg(QString::number(restMs))
                                            .arg(QString::number(headItems)));
                    emit chapterReady(token, bibleId, bookId, chapter, rows);
                }, Qt::QueuedConnection);
            }
        }

        // ---- AND THE PASSAGES ON EITHER SIDE, so the next step is immediate ----
        // A service is a walk: after this passage the operator steps to the one beside it,
        // and the load that costs a second is the READ. Both neighbours are therefore read
        // and cut here, on this thread, after the chapter that was asked for has been
        // handed over (the posting above returns at once, so nothing waits on this work).
        // Bounded three ways: two chapters at most, only chapters that exist, and only
        // while this load is still the one the pane is waiting for — an operator stepping
        // quickly has already asked for their chapter, and THAT load reads and cuts its
        // own neighbours.
        QElapsedTimer aheadClock;
        aheadClock.start();
        QStringList ahead;
        for (const SlideBuilder::ChapterKey &otherKey : neighbourKeys) {
            if (latestChapterToken_.load() != token)
                return;
            if (!SlideBuilder::warmedRows(otherKey).isEmpty())
                continue;   // in hand already (the chapter just stepped from, or the other side)
            const QVariantList otherRows = this->readChapter(bibleId, bookId, otherKey.chapter);
            if (otherRows.isEmpty())
                continue;   // no such chapter (the end of a book)
            QVariantList otherNumbers;
            for (const QVariant &row : otherRows)
                otherNumbers.append(row.toMap().value(QStringLiteral("number")).toInt());
            pf::BookSource otherSource;
            if (otherNumbers.isEmpty()
                || !sourceFor(bibleId, bookId, otherKey.chapter, otherNumbers, otherSource, parallelIds))
                continue;
            SlideBuilder::warmNeighbour(
                SlideBuilder::WarmRequest{ epoch, otherKey, templateBlocks, otherSource, otherRows });
            ahead.append(QString::number(otherKey.chapter));
        }
        const qint64 aheadMs = aheadClock.elapsed();
        if (ahead.isEmpty() || latestChapterToken_.load() != token)
            return;
        QMetaObject::invokeMethod(this, [this, bookId, chapter, ahead, aheadMs, key] {
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),                                    QStringLiteral("%1 %2: read %3 as well and cut each to its head, so the step to either one costs no read "
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

// The split identity of a chapter, as this tab asks for it. Built identically by the
// chapter's background load (which cuts the chapter) and by the click (which looks it
// up) — that identity is the only reason the two can meet.
SlideBuilder::ChapterKey ScriptureService::chapterKey(const QString &bibleId, const QString &bookId, int chapter) const
{
    SlideBuilder::ChapterKey key;
    key.contentType = QString::fromLatin1(kProfile.contentType);
    key.templateId = templateId();
    key.sourceId = bibleId;
    key.bookId = bookId;
    key.chapter = chapter;
    key.parallelIds = parallelBibles().join(QLatin1Char(','));   // part of the words (see ChapterKey)
    key.options = SettingsService::instance().scriptureSettings();
    return key;
}

std::optional<SlideBuilder::WarmSlides> ScriptureService::warmFor(const QString &bibleId, const QString &bookId,
                                                                  int chapter, int number) const
{
    const SlideBuilder::ChapterKey key = chapterKey(bibleId, bookId, chapter);
    SlideBuilder::WarmSlides warm = SlideBuilder::warmedItem(key, number);
    if (!warm.found || warm.slides.empty()) {
        // A MISREAD WARM IS REPORTED, not silently worked around: the split is cut
        // with this tab's template and options, so if a pick misses it, one of them
        // moved after the chapter was cut. The click still fills correctly — it just
        // fills on the thread this whole path exists to spare. The test is "is anything
        // held at all" (not "is THIS chapter held", which is exactly what just failed).
        if (!warmMissLogged_ && SlideBuilder::warmedChapterCount(key.contentType) > 0) {
            warmMissLogged_ = true;
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                                QStringLiteral("%1 %2 item %3 was not in the warmed split — filled on the click "
                                               "(the template or the options changed since the chapter was cut)")
                                    .arg(bookId, QString::number(chapter), QString::number(number)));
        }
        return std::nullopt;
    }
    if (!warmServeLogged_) {
        warmServeLogged_ = true;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("SlideWarm"),
                            QStringLiteral("first pick served from the split cut ahead of time — no fill on the interface thread"));
    }
    return warm;
}

// "Is what is in the box a reference?" — asked by the pane's completer on every
// keystroke and by the filter's jump. Answered from the published resolution; a
// text nobody has asked about yet is queued and reads as NOT a reference until
// the answer lands (the pane re-runs the question on referenceCached), which is
// the one visible cost of not parsing on the thread that draws.
QVariantMap ScriptureService::resolve(const QString &text, const QString &bibleId) const
{
    const QString typed = text.trimmed();
    if (typed.isEmpty() || bibleId.isEmpty())
        return {};
    const auto it = referenceCache_.constFind(referenceCacheKey(bibleId, typed));
    if (it == referenceCache_.constEnd()) {
        refreshReferenceOffThread(bibleId, typed);
        return {};
    }
    return *it;
}

QVariantMap ScriptureService::readResolve(const QString &text, const QString &bibleId) const
{
    if (!EngineBridge::instance().booted() || text.trimmed().isEmpty())
        return {};
    auto ref = bb::BibleEngine::Instance().ResolveReference(text.toStdString(), bibleId.toStdString());
    if (!ref.ok() || !ref.value().Valid())
        return {};
    return { { QStringLiteral("bookId"), qstr(ref.value().bookId) }, { QStringLiteral("book"), qstr(ref.value().bookName) },
             { QStringLiteral("chapter"), ref.value().chapter }, { QStringLiteral("verseStart"), ref.value().verseStart },
             { QStringLiteral("verseEnd"), ref.value().verseEnd } };
}

// The SYNC search answers from a published result for the same query; a query
// nobody has run yet is queued and reads as no rows. The pane does not use this
// one (it prefers searchAsync, whose worker runs readSearch below), so what is
// here is the contract for a caller that cannot wait: ask, then ask again when
// searchAsync's answer has landed.
QVariantList ScriptureService::search(const QString &text, const QString &bibleId, int limit,
                                     bool exactWords, const QString &scopeBookId,
                                     int scopeChapter) const
{
    const QString typed = text.trimmed();
    if (typed.isEmpty() || bibleId.isEmpty())
        return {};
    const QString key = QString::number(limit) + QLatin1Char('\n') + (exactWords ? QStringLiteral("1") : QStringLiteral("0"))
                        + QLatin1Char('\n') + scopeBookId + QLatin1Char('\n') + QString::number(scopeChapter)
                        + QLatin1Char('\n') + bibleId + QLatin1Char('\n') + typed;
    const auto it = searchCache_.constFind(key);
    if (it == searchCache_.constEnd()) {
        refreshSearchOffThread(text, bibleId, limit, exactWords, scopeBookId, scopeChapter);
        return {};
    }
    return *it;
}

QVariantList ScriptureService::readSearch(const QString &text, const QString &bibleId, int limit,
                                         bool exactWords, const QString &scopeBookId,
                                         int scopeChapter) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted() || text.trimmed().isEmpty())
        return out;
    auto &engine = bb::BibleEngine::Instance();
    // The chapter lock rides along into the engine, which narrows the search to
    // that one passage before it ranks (see BibleEngine::Search) — so a locked
    // query is answered from the open chapter, not from the whole Bible filtered
    // down to whatever survived the engine's candidate cap.
    auto hits = engine.Search(text.toStdString(), bibleId.toStdString(),
                              scopeBookId.toStdString(), scopeChapter);
    if (!hits.ok())
        return out;
    // THE PADLOCK: the engine's index matches fuzzily (completions, corrections)
    // and has no exact switch, so the locked search filters its hits here — the
    // verse itself must carry every typed word as a WHOLE word. The verse text is
    // fetched per surviving hit (capped by `limit`), never per candidate verse.
    const QStringList words = exactWords ? queryWords(text) : QStringList();
    if (exactWords && words.isEmpty())
        return out;
    for (const bb::BibleSearchHit &h : hits.value()) {
        if (!bibleId.isEmpty() && qstr(h.bibleId) != bibleId)
            continue;
        if (exactWords) {
            auto verse = engine.GetVerse(h.bibleId, h.bookId, h.chapter, h.verse);
            if (!verse.ok())
                continue;
            const std::string hay = bps::search::textmatch::Lower(qstr(verse.value().text).toStdString());
            bool all = true;
            for (const QString &w : words)
                if (!bps::search::textmatch::ContainsWord(hay, w.toStdString())) { all = false; break; }
            if (!all)
                continue;
        }
        auto book = engine.GetBook(h.bibleId, h.bookId);
        const QString bookName = book.ok() ? qstr(book.value().name) : qstr(h.bookId);
        out.append(QVariantMap{
            { QStringLiteral("reference"), QStringLiteral("%1 %2:%3").arg(bookName).arg(h.chapter).arg(h.verse) },
            { QStringLiteral("bookId"), qstr(h.bookId) }, { QStringLiteral("book"), bookName },
            { QStringLiteral("chapter"), h.chapter }, { QStringLiteral("verse"), h.verse },
            { QStringLiteral("snippet"), qstr(h.snippet) },
        });
        if (out.size() >= limit)
            break;
    }
    return out;
}

QString ScriptureService::reference(const QString &book, int chapter, const QVariantList &verses) const
{
    return qstr(pf::BookReference(book.toStdString(), chapter, numbersOf(verses), pf::ContentFamily::Scripture));
}

// ASYNC search (the Quick-search pattern): the pill pane runs this per keystroke;
// the GUI-thread cost drops to a token check. Stale tokens are dropped on
// delivery — a fast typist's older keystrokes answer into the void.
void ScriptureService::searchAsync(const QString &text, const QString &bibleId,
                                   int limit, int token, bool exactWords,
                                   const QString &scopeBookId, int scopeChapter)
{
    auto *runner = QThread::create([this, text, bibleId, limit, token, exactWords, scopeBookId, scopeChapter] {
        const QVariantList rows = readSearch(text, bibleId, limit, exactWords, scopeBookId, scopeChapter);
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
// Templates
// ---------------------------------------------------------------------------

QString ScriptureService::defaultTemplateId() const { return QString::fromLatin1(kDefaultTemplate); }

QStringList ScriptureService::parallelBibles() const
{
    // Only the versions this app can still read: a Bible removed since the selection
    // was made must not appear in the picker as if it were there.
    const QStringList installed = installedBibleIds();
    QStringList out;
    for (const QString &id : storedParallelBibles())
        if (installed.contains(id))
            out << id;
    return out;
}

bool ScriptureService::isParallelBible(const QString &bibleId) const
{
    return parallelBibles().contains(bibleId);
}

bool ScriptureService::setParallelBibles(const QStringList &bibleIds)
{
    const QStringList installed = installedBibleIds();
    QStringList clean;
    for (const QString &id : bibleIds) {
        if (!installed.contains(id) || clean.contains(id))
            return false;   // a version that is not installed (or the same one twice)
        clean << id;
    }
    if (clean.size() > kMaxParallel)
        return false;       // the vocabulary carries four Bibles, no more
    if (!SettingsService::instance().setValue(QString::fromLatin1(kParallelKey), clean.join(QLatin1Char(','))))
        return false;
    emit changed();         // the preview, the slides and every layout re-derive
    return true;
}

bool ScriptureService::toggleParallelBible(const QString &bibleId)
{
    QStringList next = parallelBibles();
    if (next.contains(bibleId))
        next.removeAll(bibleId);
    else if (!next.contains(bibleId))
        next << bibleId;
    return setParallelBibles(next);
}

QString ScriptureService::templateId() const
{
    return SlideBuilder::templateId(kProfile);
}

QVariantList ScriptureService::templates() const
{
    return SlideBuilder::templates(kProfile);
}

QString ScriptureService::templateName(const QString &id) const
{
    return SlideBuilder::templateName(id);
}

bool ScriptureService::templateHasValues(const QString &id) const
{
    return SlideBuilder::templateHasValues(id);
}

// ---------------------------------------------------------------------------
// A passage on slides
// ---------------------------------------------------------------------------

QVariantMap ScriptureService::preview(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const
{
    // A SINGLE PICK IS ANSWERED FROM THE SPLIT ALREADY CUT FOR IT, when there is one:
    // this runs on the interface thread every time the operator chooses a verse, which
    // is the moment the machine must stay free. An un-warmed pick (a multi-verse
    // selection, or a chapter that arrived before the warm) falls through to the read
    // and the fill exactly as before.
    if (verses.size() == 1)
        if (const auto warm = warmFor(bibleId, bookId, chapter, verses.constFirst().toInt()))
            return SlideBuilder::previewOf(warm->slides, warm->hasPlaceholders);
    pf::BookSource source;
    if (!EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(bibleId, bookId, chapter, verses, source))
        return SlideBuilder::previewOf({}, true);   // (nothing to show: the empty shape)
    return SlideBuilder::preview(templateId(), source, SettingsService::instance().scriptureSettings());
}

QVariantList ScriptureService::slides(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const
{
    if (verses.size() == 1)
        if (const auto warm = warmFor(bibleId, bookId, chapter, verses.constFirst().toInt()))
            return SlideBuilder::slidesOf(warm->slides, QStringLiteral("scripture"));
    pf::BookSource source;
    if (!EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(bibleId, bookId, chapter, verses, source))
        return {};
    return SlideBuilder::slides(templateId(), source, SettingsService::instance().scriptureSettings(),
                                QStringLiteral("scripture"));
}

bool ScriptureService::importBible()
{
    if (!EngineBridge::instance().booted())
        return false;
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        // Bible formats only — the installer validates the document it is
        // handed, so "All files" only invited a refusal after the fact.
        "Install a Bible", { "Bible files (*.json *.xml *.osis *.usfm *.sfm *.txt)" });
    if (!picked.ok() || !picked.value())
        return false;
    return SearchService::instance().importBibleFile(QString::fromStdString(*picked.value()));
}

// ---------------------------------------------------------------------------
// User data — the ENGINE's notes and highlights (bps::bible::BibleEngine,
// stored separately from Scripture text, docs/specs/24 §User data). Every
// entry point resolves the reference through the engine so QML only ever
// passes what the UI shows ("John 3:16"); a bad reference is a false/empty
// answer, never a crash.
// ---------------------------------------------------------------------------
namespace {

// resolvedRef lived here — a text reference resolved through the engine. It is gone
// with the reads that used it: resolution now happens on a worker and its answer is
// published (ScriptureService::canonicalRef), so no mark question on the interface
// thread runs the engine's resolver any more.

// The id-shaped counterpart: the pane knows the book's id ("JHN"), chapter
// and verse — building a display reference in QML just so the service can
// re-parse it re-implemented the engine's reference grammar in JavaScript.
std::optional<bb::PassageRef> resolvedRefForBookId(const QString &bibleId, const QString &bookId,
                                                   int chapter, int verse)
{
    if (!EngineBridge::instance().booted() || chapter <= 0 || verse <= 0)
        return std::nullopt;
    bb::PassageRef ref;
    ref.bookId = bookId.toStdString();
    ref.bookName = bookId.toStdString();
    ref.chapter = chapter;
    ref.verseStart = verse;
    ref.verseEnd = verse;
    if (!ref.Valid())
        return std::nullopt;
    return ref;
}

QVariantMap noteToVariant(const bb::UserNote &n)
{
    return QVariantMap{
        { QStringLiteral("reference"), qstr(n.ref.ToString()) },
        { QStringLiteral("bookId"), qstr(n.ref.bookId) },
        { QStringLiteral("book"), qstr(n.ref.bookName) },
        { QStringLiteral("chapter"), n.ref.chapter },
        { QStringLiteral("verseStart"), n.ref.verseStart },
        { QStringLiteral("verseEnd"), n.ref.verseEnd },
        { QStringLiteral("text"), qstr(n.text) },
        { QStringLiteral("modifiedMs"), qint64(n.modifiedMs) },
    };
}

} // namespace

// ---- The published marks (see the header) -----------------------------------
//
// One worker read per Bible fills marksByBible_/notesByBible_; the five mark
// reads below scan those lists instead of walking the engine's rosters on this
// thread, and the two writes queue the engine's own write and republish when it
// lands. `userDataChanged` is the re-read signal the panes already listen to, so
// a published (or corrected) answer reaches the verse rows through the wiring
// that was already there.
const QList<ScriptureService::CachedMark> &ScriptureService::marksFor(const QString &bibleId) const
{
    const auto it = marksByBible_.constFind(bibleId);
    if (it != marksByBible_.constEnd())
        return it.value();
    // COLD: publish it and say so — the pane re-reads when the answer lands
    // (userDataChanged), so an open chapter's rows correct themselves a beat after
    // the first question instead of the read blocking on the engine here.
    refreshMarksOffThread(bibleId);
    static const QList<CachedMark> kNone;
    return kNone;
}

const QList<ScriptureService::CachedNote> &ScriptureService::notesFor(const QString &bibleId) const
{
    const auto it = notesByBible_.constFind(bibleId);
    if (it != notesByBible_.constEnd())
        return it.value();
    refreshMarksOffThread(bibleId);
    static const QList<CachedNote> kNone;
    return kNone;
}

void ScriptureService::refreshMarksOffThread(const QString &bibleId, bool force) const
{
    const QString id = bibleId.trimmed();
    if (id.isEmpty() || !EngineBridge::instance().booted())
        return;
    if (marksWarm_.contains(id) && !force)
        return;   // already read once; only a write asks again
    if (marksInFlight_.contains(id))
        return;   // one read per Bible at a time
    marksInFlight_.insert(id);
    QThread *runner = QThread::create([this, id]() {
        QList<CachedMark> marks;
        QList<CachedNote> notes;
        auto &engine = bb::BibleEngine::Instance();
        auto hot = engine.Highlights(id.toStdString());
        if (hot.ok())
            for (const bb::PassageRef &m : hot.value())
                marks.append(CachedMark{ qstr(m.ToString()), qstr(m.bookId), m.chapter, m.verseStart });
        auto written = engine.Notes(id.toStdString());
        if (written.ok())
            for (const bb::UserNote &n : written.value())
                notes.append(CachedNote{ qstr(n.ref.ToString()), qstr(n.ref.bookId),
                                         qstr(n.ref.bookName), qstr(n.text), n.ref.chapter,
                                         n.ref.verseStart, n.ref.verseEnd, qint64(n.modifiedMs) });
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this),
                                  [this, id, marks, notes]() { publishMarks(id, marks, notes); },
                                  Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::publishMarks(const QString &bibleId, QList<CachedMark> marks,
                                    QList<CachedNote> notes) const
{
    marksInFlight_.remove(bibleId);
    marksWarm_.insert(bibleId);
    marksByBible_.insert(bibleId, std::move(marks));
    notesByBible_.insert(bibleId, std::move(notes));
    // The re-read: every verse row's mark question is answered from what just
    // landed, so this is what makes a highlight or a note appear (the same signal
    // the old inline write emitted).
    emit const_cast<ScriptureService *>(this)->userDataChanged();
}

const QString *ScriptureService::canonicalRef(const QString &bibleId, const QString &text) const
{
    // The marks above are keyed by the ENGINE's spelling of a reference ("JHN
    // 3:16"), so a caller's own wording has to be resolved the same way — on a
    // worker, once per (Bible, text), because the reference grammar lives in the
    // engine and this thread must not run a resolver inside a row's binding.
    const QString key = bibleId.trimmed() + QLatin1Char('\n') + text.trimmed();
    const auto it = refCanonical_.constFind(key);
    if (it != refCanonical_.constEnd())
        return &it.value();
    if (refPending_.contains(key))
        return nullptr;   // in flight: no answer for this question yet
    refPending_.insert(key);
    QThread *runner = QThread::create([this, key, bibleId, text]() {
        const QString id = bibleId.trimmed();
        auto ref = bb::BibleEngine::Instance().ResolveReference(text.trimmed().toStdString(),
                                                                id.toStdString());
        const QString canonical = (ref.ok() && ref.value().Valid()) ? qstr(ref.value().ToString())
                                                                    : QString();
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this), [this, key, canonical]() {
            refPending_.remove(key);
            const bool first = !refCanonical_.contains(key);
            refCanonical_.insert(key, canonical);
            // Only a FIRST answer is worth re-inviting the readers for (a repeat
            // asks the same question and gets the same answer).
            if (first)
                emit const_cast<ScriptureService *>(this)->userDataChanged();
        }, Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
    return nullptr;
}

void ScriptureService::writeNoteOffThread(const QString &bibleId, const QString &reference,
                                          const QString &text, bool remove) const
{
    QThread *runner = QThread::create([this, bibleId, reference, text, remove]() {
        auto &engine = bb::BibleEngine::Instance();
        auto ref = engine.ResolveReference(reference.toStdString(), bibleId.toStdString());
        if (!ref.ok() || !ref.value().Valid()) {
            EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Notes"),
                                QStringLiteral("'%1' is not a reference in this Bible — the note was not written")
                                    .arg(reference));
            return;
        }
        // An empty text REMOVES the note (the delete gesture), not a zero-length one.
        auto r = remove ? engine.RemoveNote(bibleId.toStdString(), ref.value())
                        : engine.AddNote(bibleId.toStdString(), ref.value(), text.toStdString());
        if (!r.ok()) {
            EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Notes"),
                                QStringLiteral("the note on '%1' could not be written")
                                    .arg(reference));
            return;
        }
        // Republish THIS Bible's rosters (the write moved them): the refresh emits
        // userDataChanged, which is what the verse rows re-read on.
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this),
                                  [this, bibleId]() { refreshMarksOffThread(bibleId, true); },
                                  Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void ScriptureService::writeHighlightOffThread(const QString &bibleId, const QString &reference,
                                               bool on) const
{
    QThread *runner = QThread::create([this, bibleId, reference, on]() {
        auto &engine = bb::BibleEngine::Instance();
        auto ref = engine.ResolveReference(reference.toStdString(), bibleId.toStdString());
        if (!ref.ok() || !ref.value().Valid()) {
            EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Highlights"),
                                QStringLiteral("'%1' is not a reference in this Bible — nothing was marked")
                                    .arg(reference));
            return;
        }
        auto r = engine.SetHighlight(bibleId.toStdString(), ref.value(), on);
        if (!r.ok()) {
            EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Highlights"),
                                QStringLiteral("the mark on '%1' could not be %2")
                                    .arg(reference, on ? QStringLiteral("set")
                                                       : QStringLiteral("cleared")));
            return;
        }
        QMetaObject::invokeMethod(const_cast<ScriptureService *>(this),
                                  [this, bibleId]() { refreshMarksOffThread(bibleId, true); },
                                  Qt::QueuedConnection);
    });
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

bool ScriptureService::setNote(const QString &bibleId, const QString &reference, const QString &text)
{
    // ACCEPTED, then corrected: the engine's mark write is a small sidecar write
    // and it runs on a worker now (see writeNoteOffThread), so `true` means "this is
    // a request" — the roster landing is what shows the note. A malformed request
    // is still refused here, with no worker started at all.
    if (bibleId.trimmed().isEmpty() || reference.trimmed().isEmpty())
        return false;
    writeNoteOffThread(bibleId.trimmed(), reference.trimmed(), text, text.trimmed().isEmpty());
    return true;
}

QString ScriptureService::note(const QString &bibleId, const QString &reference) const
{
    const QString want = reference.trimmed();
    if (want.isEmpty())
        return {};
    // The caller's OWN spelling first: the panes pass references the engine wrote
    // (a verse row's reference, the pane's own reference()), and those are already
    // canonical — so the common case is an exact string match with no resolution.
    const QString *canonical = &want;
    for (const CachedNote &n : notesFor(bibleId))
        if (n.reference == want)
            return n.text;
    // Otherwise the engine's own resolution of that wording, on a worker; until it
    // answers there is nothing to report (the read is re-invited when it lands).
    canonical = canonicalRef(bibleId, want);
    if (!canonical || canonical->isEmpty())
        return {};
    for (const CachedNote &n : notesFor(bibleId))
        if (n.reference == *canonical)
            return n.text;
    return {};
}

QVariantList ScriptureService::notes(const QString &bibleId) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted())
        return out;
    // The published roster (see the header): the engine's whole-Bible sweep is
    // read once per Bible on a worker, and sorted most-recently-modified first
    // here — the old highlights-roster walk dropped every note that carried no
    // highlight of its own, and that is exactly what this list is not.
    for (const CachedNote &n : notesFor(bibleId))
        out.append(QVariantMap{
            { QStringLiteral("reference"), n.reference },
            { QStringLiteral("bookId"), n.bookId },
            { QStringLiteral("book"), n.book },
            { QStringLiteral("chapter"), n.chapter },
            { QStringLiteral("verseStart"), n.verseStart },
            { QStringLiteral("verseEnd"), n.verseEnd },
            { QStringLiteral("text"), n.text },
            { QStringLiteral("modifiedMs"), n.modifiedMs },
        });
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("modifiedMs")).toLongLong()
             > b.toMap().value(QStringLiteral("modifiedMs")).toLongLong();
    });
    return out;
}

bool ScriptureService::setHighlighted(const QString &bibleId, const QString &reference, bool on)
{
    // Accepted-then-published, exactly like setNote above.
    if (bibleId.trimmed().isEmpty() || reference.trimmed().isEmpty())
        return false;
    writeHighlightOffThread(bibleId.trimmed(), reference.trimmed(), on);
    return true;
}

QVariantMap ScriptureService::chapterUserData(const QString &bibleId, const QString &bookId,
                                              int chapter) const
{
    QVariantMap out;
    if (!EngineBridge::instance().booted() || chapter <= 0)
        return out;
    // ONE published sweep for the chapter — the same two lists the engine used to
    // hand over per open chapter, now scanned here (see the header), which is what
    // lets the verse rows read this map without the engine on this thread.
    const QString book = bookId;
    for (const CachedMark &m : marksFor(bibleId)) {
        if (m.bookId != book || m.chapter != chapter || m.verseStart <= 0)
            continue;
        const QString key = QString::number(m.verseStart);
        QVariantMap mark = out.value(key).toMap();   // QVariant: merge explicitly
        mark[QStringLiteral("highlight")] = true;
        out[key] = mark;
    }
    for (const CachedNote &n : notesFor(bibleId)) {
        // These lists only ever carry THIS bible's marks, so the reference's own
        // fields are the whole test — no key parsing.
        if (n.bookId != book || n.chapter != chapter || n.verseStart <= 0)
            continue;
        const QString key = QString::number(n.verseStart);
        QVariantMap mark = out.value(key).toMap();
        mark[QStringLiteral("note")] = n.text;
        out[key] = mark;
    }
    return out;
}

bool ScriptureService::isHighlighted(const QString &bibleId, const QString &reference) const
{
    const QString want = reference.trimmed();
    if (want.isEmpty())
        return false;
    // The caller's spelling first (see note()): a canonical reference matches a
    // published mark exactly, with no resolution at all.
    for (const CachedMark &m : marksFor(bibleId))
        if (m.reference == want)
            return true;
    const QString *canonical = canonicalRef(bibleId, want);
    if (!canonical || canonical->isEmpty())
        return false;
    for (const CachedMark &m : marksFor(bibleId))
        if (m.reference == *canonical)
            return true;
    return false;
}

QVariantList ScriptureService::highlights(const QString &bibleId) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted())
        return out;
    // The published roster, in the engine's own canonical spelling (see the
    // header) — the same strings the old inline walk produced.
    for (const CachedMark &m : marksFor(bibleId))
        out.append(m.reference);
    return out;
}
