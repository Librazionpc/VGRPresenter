#include "services/ScriptureService.h"

#include "modules/bible/BibleEngine.hpp"
#include "modules/presentation/ScriptureSlides.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/SettingsService.h"
#include "services/SlideBuilder.h"
#include "services/ShowConverter.h"

#include <QThread>

#include <QJSEngine>
#include <QQmlEngine>

namespace bb = bps::bible;
namespace pf = bps::presentation;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The engine's default scripture template (the id the template catalog gives the plain "Scripture" layout).
constexpr const char *kDefaultTemplate = "tpl-scripture";
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
bool sourceFor(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses, pf::ScriptureSource &out)
{
    auto &engine = bb::BibleEngine::Instance();
    const std::string bible = bibleId.toStdString();
    auto meta = engine.Metadata(bible);
    auto book = engine.GetBook(bible, bookId.toStdString());
    auto ch = engine.GetChapter(bible, bookId.toStdString(), chapter);
    if (!meta.ok() || !book.ok() || !ch.ok())
        return false;
    out = pf::ScriptureSource{};
    out.versionName = meta.value().name;
    out.copyright = meta.value().copyright;
    out.book = book.value().name;
    out.bookAbbr = book.value().aliases.empty() ? book.value().id : book.value().aliases.front();
    out.chapter = chapter;
    const std::vector<int> wanted = numbersOf(verses);
    for (const bb::BibleVerse &v : ch.value().verses)
        if (std::find(wanted.begin(), wanted.end(), v.verse) != wanted.end())
            out.verses.push_back({ v.verse, v.text });
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
    // A Bible finished installing (or the template library / settings changed): everything derived from them is stale.
    connect(&SearchService::instance(), &SearchService::bibleChanged, this, &ScriptureService::changed);
    connect(&TemplateLibraryService::instance(), &DesignLibraryService::changed, this, &ScriptureService::changed);
    connect(&SettingsService::instance(), &SettingsService::changed, this, &ScriptureService::changed);
}

bool ScriptureService::loading() const { return SearchService::instance().bibleLoading(); }

QVariantList ScriptureService::bibles() const
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

QVariantList ScriptureService::books(const QString &bibleId) const
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

QVariantList ScriptureService::chapter(const QString &bibleId, const QString &bookId, int chapter) const
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

QVariantMap ScriptureService::resolve(const QString &text, const QString &bibleId) const
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

QVariantList ScriptureService::search(const QString &text, const QString &bibleId, int limit) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted() || text.trimmed().isEmpty())
        return out;
    auto &engine = bb::BibleEngine::Instance();
    auto hits = engine.Search(text.toStdString(), bibleId.toStdString());
    if (!hits.ok())
        return out;
    for (const bb::BibleSearchHit &h : hits.value()) {
        if (!bibleId.isEmpty() && qstr(h.bibleId) != bibleId)
            continue;
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
    return qstr(pf::ScriptureReference(book.toStdString(), chapter, numbersOf(verses)));
}

// ASYNC search (the Quick-search pattern): the pill pane runs this per keystroke;
// the GUI-thread cost drops to a token check. Stale tokens are dropped on
// delivery — a fast typist's older keystrokes answer into the void.
void ScriptureService::searchAsync(const QString &text, const QString &bibleId,
                                   int limit, int token)
{
    auto *runner = QThread::create([this, text, bibleId, limit, token] {
        const QVariantList rows = search(text, bibleId, limit);
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
    pf::ScriptureSource source;
    if (!EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(bibleId, bookId, chapter, verses, source))
        return SlideBuilder::preview(templateId(), {}, {});   // (nothing to show: the empty shape)
    return SlideBuilder::preview(templateId(), source, SettingsService::instance().scriptureSettings());
}

QVariantList ScriptureService::slides(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const
{
    pf::ScriptureSource source;
    if (!EngineBridge::instance().booted() || verses.isEmpty() || !sourceFor(bibleId, bookId, chapter, verses, source))
        return {};
    return SlideBuilder::slides(templateId(), source, SettingsService::instance().scriptureSettings());
}

bool ScriptureService::importBible()
{
    if (!EngineBridge::instance().booted())
        return false;
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        "Install a Bible", { "Bible files (*.json *.xml *.osis *.usfm *.sfm *.txt)", "All files (*.*)" });
    if (!picked.ok() || !picked.value())
        return false;
    return SearchService::instance().importBibleFile(QString::fromStdString(*picked.value()));
}
