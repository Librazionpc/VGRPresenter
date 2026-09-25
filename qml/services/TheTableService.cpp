#include "services/TheTableService.h"

#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"
#include "services/SlideBuilder.h"
#include "services/ShowConverter.h"

#include <QThread>

#include "modules/library/TheTableLibrary.hpp"
#include "modules/presentation/ScriptureSlides.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

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

// This tab's place in the shared slide builder: its own "table" templates and its own setting.
const SlideBuilder::Profile kProfile{ "table", "table.template", "tpl-table" };

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
               const QVariantList &verses, pf::ScriptureSource &out)
{
    out = pf::ScriptureSource{};
    if (!lib)
        return false;
    auto ch = lib->GetChapter(bookId.toStdString(), chapter);
    if (!ch.ok())
        return false;
    for (const auto &b : lib->BookIndex()) {   // (the index: Books() would copy every sermon's words just to find a name)
        if (b.id != bookId.toStdString())
            continue;
        // The citation line ("53-0217 - Only Believe") is what the engine's
        // reference builder prepends: ScriptureReference(book, chapter, verses)
        // writes `<book> <chapter>:<verses>`, and with the citation as book +
        // chapter 0 that becomes exactly the sermon citation + paragraph
        // numbers. (Scripture keeps its own plain book name.)
        out.book = lib->Citation(bookId.toStdString(), chapter, 0);
        break;
    }
    // The engine skips the chapter and ":" for chapter 0 (ScriptureReference);
    // the verses still come out as "1-3" after the citation.
    out.chapter = 0;
    for (const QVariant &v : verses) {
        const int n = v.toInt();
        for (const bl::TheTableVerse &tv : ch.value().verses) {
            if (tv.number == n) {
                out.verses.push_back({ n, tv.text });
                break;
            }
        }
    }
    return !out.verses.empty();
}

} // namespace

TheTableService::TheTableService(QObject *parent)
    : QObject(parent)
{
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
    // A template landed/changed or the tab's options moved: the preview,
    // slides and template card re-derive from them (Scripture's contract).
    connect(&TemplateLibraryService::instance(), &DesignLibraryService::changed, this, &TheTableService::changed);
    connect(&SettingsService::instance(), &SettingsService::changed, this, &TheTableService::changed);
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
    if (library_)
        return;
    auto &platform = bps::platform::PlatformAccessor::Get();
    const std::string file = platform.Filesystem().Join(platform.Paths().UserDataDir(),
                                                        "the_table.json");
    // The Open'd shared_ptr is deliberately released into the service's
    // lifetime: the singleton lives for the whole process (CppOwnership), so
    // the raw pointer is safe and keeps the engine header out of this one.
    static std::shared_ptr<bl::TheTableLibrary> keepAlive = bl::TheTableLibrary::Open(file);
    library_ = keepAlive.get();
    // The sermons join the platform Search Engine's index (the Bible/Song
    // precedent): once here, the app-wide search (Ctrl+K) finds them. One
    // document per sermon — ~1,200 upserts, done in the background so the
    // first paint never waits on the index.
    std::thread([] {
        if (keepAlive->IndexWithSearchEngine().ok())
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("Table"),
                                QStringLiteral("Sermons indexed for the app-wide search"));
    }).detach();
    emit changed();
}

bool TheTableService::loading() const
{
    return loading_;
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

QVariantList TheTableService::search(const QString &text, int limit) const
{
    QVariantList out;
    if (!library_ || !EngineBridge::instance().booted())
        return out;
    auto hits = tableLibrary(library_)->Search(text.toStdString(), size_t(limit));
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
void TheTableService::searchAsync(const QString &text, int limit, int token)
{
    auto *runner = QThread::create([this, text, limit, token] {
        const QVariantList rows = search(text, limit);
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
                                   : cite + QStringLiteral(" %1").arg(qstr(pf::ScriptureVerseRange(numbers)));
        }
    }
    // Unknown book/chapter (a stale pane): the scripture shape still resolves.
    return qstr(pf::ScriptureReference(book.toStdString(), chapter, numbers));
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
    const QString lower = needle.toLower();
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
            if (cite.indexOf(lower, 0, Qt::CaseInsensitive) < 0)
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
    const QString lowerBase = base.toLower();
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
            if (cite.indexOf(lowerBase, 0, Qt::CaseInsensitive) < 0)
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
    pf::ScriptureSource source;
    if (!library_ || !EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(tableLibrary(library_), bookId, chapter, verses, source))
        return SlideBuilder::preview(templateId(), {}, {});   // (nothing to show: the empty shape)
    source.versionName = "The Table";
    // The chosen template laid out by the SHARED slide builder - the same engine machinery Scripture uses, fed paragraphs instead of
    // verses, and shaped by the tab's own "table." options (splitting, per-slide counts).
    return SlideBuilder::preview(templateId(), source, SettingsService::instance().theTableSettings());
}

QVariantList TheTableService::slides(const QString &bookId, int chapter, const QVariantList &verses) const
{
    pf::ScriptureSource source;
    if (!library_ || !EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(tableLibrary(library_), bookId, chapter, verses, source))
        return {};
    source.versionName = "The Table";
    return SlideBuilder::slides(templateId(), source, SettingsService::instance().theTableSettings());
}

// The Table's template chrome (the shared pane's template card + picker):
// its own setting key, its own content-type filter in the shared library.
QString TheTableService::templateId() const
{
    // The chosen layout - the default when nothing is chosen or the chosen one has been deleted.
    return SlideBuilder::templateId(kProfile);
}

QString TheTableService::defaultTemplateId() const { return QStringLiteral("tpl-table"); }

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
    const QString root = QString::fromStdString(*picked.value());

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
                report(tr("Imported %1 sermons · %2 already in · %3 failed")
                           .arg(rep.imported).arg(rep.skipped).arg(rep.failed),
                       QStringLiteral("success"));
                if (rep.imported > 0)
                    std::thread([] { (void)TheTableService::instance().reindex(); }).detach();
                emit changed();
            } else {
                report(qstr(r.error().message), QStringLiteral("error"));
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // Quitting while the import runs must not destroy a running thread: give it a moment to finish (the connection goes when the thread
    // object is deleted, so this never touches a dead pointer).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, worker, [worker] { worker->wait(10000); });
    worker->start();
    return true;
}

bool TheTableService::newSermon()
{
    if (!EngineBridge::instance().booted()) {
        report(tr("The engine is still starting — try again in a moment."), QStringLiteral("warning"));
        return false;
    }
    const std::string title = tr("Add a sermon").toStdString();
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        title, { std::string("Sermons (*.txt *.pdf)"), std::string("All files (*.*)") });
    if (!picked.ok() || !picked.value())
        return false;
    const std::string path = *picked.value();

    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        report(tr("Couldn't read %1.").arg(qstr(path)), QStringLiteral("error"));
        return false;
    }
    const QByteArray bytes = file.readAll();

    auto r = tableLibrary(library_)->ImportSermon(path,
                                                  std::string_view(bytes.constData(), size_t(bytes.size())));
    if (!r.ok()) {
        report(qstr(r.error().message), QStringLiteral("error"));
        return false;
    }
    report(tr("Added %1 · %2").arg(QFileInfo(QString::fromStdString(path)).fileName(),
                                   qstr(r.value())),
           QStringLiteral("success"));
    std::thread([] { (void)TheTableService::instance().reindex(); }).detach();
    emit changed();
    return true;
}

// Re-upserts every sermon document into the platform Search Engine (an import
// landed). The engine's IndexDocument is an incremental upsert, so other
// content's documents are never touched.
void TheTableService::reindex()
{
    if (library_ && EngineBridge::instance().booted())
        (void)tableLibrary(library_)->IndexWithSearchEngine();
}
