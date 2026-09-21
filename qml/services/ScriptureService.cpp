#include "services/ScriptureService.h"

#include "modules/bible/BibleEngine.hpp"
#include "modules/presentation/ScriptureSlides.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/SearchService.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"

#include <QJSEngine>
#include <QQmlEngine>

namespace bb = bps::bible;
namespace pf = bps::presentation;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The engine's default scripture template (the id the template catalog gives the plain "Scripture" layout).
constexpr const char *kDefaultTemplate = "tpl-scripture";

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

QVariantList blocksToVariants(const std::vector<pf::ContentBlock> &blocks)
{
    QVariantList out;
    for (const pf::ContentBlock &b : blocks)
        out.append(ShowConverter::blockToVariant(b));
    return out;
}

// The blocks of a template from the engine's template library.
std::vector<pf::ContentBlock> templateBlocks(const QString &id, QString *background = nullptr)
{
    std::vector<pf::ContentBlock> out;
    const QVariantMap design = TemplateLibraryService::instance().design(id);
    for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
        out.push_back(ShowConverter::blockFromVariant(b.toMap()));
    if (background)
        *background = design.value(QStringLiteral("background")).toString();
    return out;
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

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

QString ScriptureService::defaultTemplateId() const { return QString::fromLatin1(kDefaultTemplate); }

QString ScriptureService::templateId() const
{
    const QString chosen = SettingsService::instance().value(QStringLiteral("scripture.template")).toString();
    if (!chosen.isEmpty() && !TemplateLibraryService::instance().design(chosen).isEmpty())
        return chosen;
    return defaultTemplateId();
}

QVariantList ScriptureService::templates() const
{
    QVariantList out;
    for (const QVariant &d : TemplateLibraryService::instance().designs()) {
        const QVariantMap m = d.toMap();
        if (m.value(QStringLiteral("contentType")).toString() == QLatin1String("scripture"))
            out.append(QVariantMap{ { QStringLiteral("id"), m.value(QStringLiteral("id")) }, { QStringLiteral("name"), m.value(QStringLiteral("name")) },
                                    { QStringLiteral("color"), m.value(QStringLiteral("color")) } });
    }
    return out;
}

QString ScriptureService::templateName(const QString &id) const
{
    return TemplateLibraryService::instance().design(id).value(QStringLiteral("name")).toString();
}

bool ScriptureService::templateHasValues(const QString &id) const
{
    return pf::HasScriptureValues(templateBlocks(id));
}

// ---------------------------------------------------------------------------
// A passage on slides
// ---------------------------------------------------------------------------

QVariantMap ScriptureService::preview(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const
{
    QVariantMap out{ { QStringLiteral("blocks"), QVariantList() }, { QStringLiteral("background"), QString() },
                     { QStringLiteral("reference"), QString() }, { QStringLiteral("hasValues"), true }, { QStringLiteral("slideCount"), 0 } };
    if (!EngineBridge::instance().booted() || verses.isEmpty())
        return out;
    pf::ScriptureSource source;
    if (!sourceFor(bibleId, bookId, chapter, verses, source) || source.verses.empty())
        return out;

    QString background;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId(), &background);
    const pf::ScriptureSettings options = SettingsService::instance().scriptureSettings();
    const auto first = pf::BuildScriptureSlides(tmpl, source, options, /*onlyFirst=*/true);
    if (first.empty())
        return out;
    out[QStringLiteral("blocks")] = blocksToVariants(first.front().blocks);
    out[QStringLiteral("background")] = background;
    out[QStringLiteral("reference")] = qstr(first.front().reference);
    out[QStringLiteral("hasValues")] = pf::HasScriptureValues(tmpl);
    out[QStringLiteral("slideCount")] = static_cast<int>(pf::BuildScriptureSlides(tmpl, source, options).size());
    return out;
}

QVariantList ScriptureService::slides(const QString &bibleId, const QString &bookId, int chapter, const QVariantList &verses) const
{
    QVariantList out;
    if (!EngineBridge::instance().booted() || verses.isEmpty())
        return out;
    pf::ScriptureSource source;
    if (!sourceFor(bibleId, bookId, chapter, verses, source) || source.verses.empty())
        return out;
    QString background;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId(), &background);
    for (const pf::ScriptureSlide &s : pf::BuildScriptureSlides(tmpl, source, SettingsService::instance().scriptureSettings()))
        out.append(QVariantMap{ { QStringLiteral("title"), qstr(s.title) }, { QStringLiteral("background"), background },
                                { QStringLiteral("blocks"), blocksToVariants(s.blocks) } });
    return out;
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
