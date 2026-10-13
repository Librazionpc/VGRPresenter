#include "services/SlideBuilder.h"

#include "services/DesignLibraryService.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"

namespace pf = bps::presentation;

namespace SlideBuilder {

std::vector<pf::ContentBlock> templateBlocks(const QString &id)
{
    // LAYOUT ONLY — deliberately no background. The design's background used to
    // be copied into every slide this produced, which made the template outrank
    // the output style (a transparent style still came out opaque). The
    // background is decided by the engine's style/slide precedence instead.
    std::vector<pf::ContentBlock> out;
    const QVariantMap design = TemplateLibraryService::instance().design(id);
    for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
        out.push_back(ShowConverter::blockFromVariant(b.toMap()));
    return out;
}

QVariantList blocksToVariants(const std::vector<pf::ContentBlock> &blocks)
{
    QVariantList out;
    for (const pf::ContentBlock &b : blocks)
        out.append(ShowConverter::blockToVariant(b));
    return out;
}

// The fill's whole placeholder vocabulary, under the EXACT meta keys the engine
// reads it back by (SceneBuilder::SlideValues). A slide built here therefore
// fills every token the preview pane fills — {table_code_paragraphs} included,
// which is code + THIS slide's paragraph numbers and needs both to be carried.
// Empties are INCLUDED on purpose: "not the last slide" is an answer, and a
// missing key would be read as "the fill never said".
QVariantMap valuesToMeta(const pf::BookValues &v)
{
    QVariantMap out{
        // WHICH FAMILY the slide is, carried with the values so the engine
        // answers family-bound tokens from the same name the fill did instead of
        // re-deriving it (BookFill::ContentFamily). Without this a slide's
        // {scripture_name} would resolve on an output whenever a service had put
        // anything in that field, which is exactly how the tab's brand string
        // reached a "Bible version" slot.
        { QStringLiteral("family"),       QString::fromLatin1(pf::Tag(v.family)) },
        { QStringLiteral("ref"),          QString::fromStdString(v.reference) },
        { QStringLiteral("refFull"),      QString::fromStdString(v.referenceFull) },
        { QStringLiteral("refLast"),      QString::fromStdString(v.referenceLast) },
        { QStringLiteral("tableCode"),    QString::fromStdString(v.code) },
        { QStringLiteral("tableYear"),    QString::fromStdString(v.year) },
        { QStringLiteral("tableTitle"),   QString::fromStdString(v.title) },
        { QStringLiteral("tableNumbers"), QString::fromStdString(v.numbers) },
        { QStringLiteral("book"),         QString::fromStdString(v.book) },
        { QStringLiteral("bookAbbr"),     QString::fromStdString(v.bookAbbr) },
        { QStringLiteral("bible"),        QString::fromStdString(v.name) },
        { QStringLiteral("chapter"),      QString::fromStdString(v.chapter) },
        { QStringLiteral("copyright"),    QString::fromStdString(v.copyright) },
    };
    // THE PARALLEL COLUMNS (BookValues::bibles[1..]): a design's own parallel slots
    // ({scripture2_text}…) and an OUTPUT STYLE's template both resolve from these on
    // air, exactly as the preview pane resolves them from the fill. The selected
    // Bible itself is already carried (the body, plus `bible`/`bookAbbr`), so only
    // the EXTRA versions ride here — see SceneBuilder::SlideValues for the read-back.
    for (std::size_t i = 1; i < v.bibles.size() && i < 4; ++i) {
        const QString n = QString::number(static_cast<int>(i) + 1);   // {scripture2_*} is column 2
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Name"), QString::fromStdString(v.bibles[i].name));
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Abbr"), QString::fromStdString(v.bibles[i].bookAbbr));
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Text"), QString::fromStdString(v.bibles[i].text));
    }
    return out;
}

QVariantList partsToVariants(const std::vector<pf::BookPart> &parts)
{
    QVariantList out;
    out.reserve(static_cast<qsizetype>(parts.size()));
    for (const pf::BookPart &part : parts) {
        out.append(QVariantMap{
            { QStringLiteral("number"), part.number },
            { QStringLiteral("partIndex"), part.partIndex },
            { QStringLiteral("text"), QString::fromStdString(part.text) },
            { QStringLiteral("continuation"), part.continuation },
        });
    }
    return out;
}

QString templateId(const Profile &profile)
{
    const QString chosen = SettingsService::instance().value(QString::fromLatin1(profile.templateSetting)).toString();
    if (!chosen.isEmpty() && !TemplateLibraryService::instance().design(chosen).isEmpty())
        return chosen;
    return QString::fromLatin1(profile.defaultTemplate);
}

QVariantList templates(const Profile &profile)
{
    QVariantList out;
    for (const QVariant &d : TemplateLibraryService::instance().designs()) {
        const QVariantMap m = d.toMap();
        if (m.value(QStringLiteral("contentType")).toString() == QLatin1String(profile.contentType))
            out.append(QVariantMap{ { QStringLiteral("id"), m.value(QStringLiteral("id")) }, { QStringLiteral("name"), m.value(QStringLiteral("name")) },
                                    { QStringLiteral("color"), m.value(QStringLiteral("color")) } });
    }
    return out;
}

QString templateName(const QString &id)
{
    return TemplateLibraryService::instance().design(id).value(QStringLiteral("name")).toString();
}

bool templateHasValues(const QString &id)
{
    return pf::HasPlaceholders(templateBlocks(id));
}

QVariantMap preview(const QString &templateId, const pf::BookSource &source, const pf::BookFillOptions &options)
{
    QVariantMap out{ { QStringLiteral("blocks"), QVariantList() }, { QStringLiteral("background"), QString() },
                     { QStringLiteral("reference"), QString() }, { QStringLiteral("hasValues"), true },
                     { QStringLiteral("slideCount"), 0 }, { QStringLiteral("slides"), QVariantList() } };
    if (source.items.empty())
        return out;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId);
    const auto generated = pf::FillBookSlides(tmpl, source, options);
    if (generated.empty())
        return out;
    out[QStringLiteral("blocks")] = blocksToVariants(generated.front().blocks);
    // (background stays the empty "no background" from the shape above — the
    // template is layout only; the style/slide precedence supplies the fill.)
    out[QStringLiteral("reference")] = QString::fromStdString(generated.front().reference);
    out[QStringLiteral("hasValues")] = pf::HasPlaceholders(tmpl);
    QVariantList slideGroups;
    slideGroups.reserve(static_cast<qsizetype>(generated.size()));
    for (const pf::BookSlide &slide : generated) {
        QVariantList verses;
        verses.reserve(static_cast<qsizetype>(slide.numbers.size()));
        for (int verse : slide.numbers)
            verses.append(verse);
        slideGroups.append(QVariantMap{
            { QStringLiteral("title"), QString::fromStdString(slide.title) },
            { QStringLiteral("reference"), QString::fromStdString(slide.reference) },
            { QStringLiteral("verses"), verses },
            { QStringLiteral("parts"), partsToVariants(slide.parts) },
            { QStringLiteral("blocks"), blocksToVariants(slide.blocks) },
        });
    }
    out[QStringLiteral("slides")] = slideGroups;
    out[QStringLiteral("slideCount")] = static_cast<int>(generated.size());
    return out;
}

QVariantList slides(const QString &templateId, const pf::BookSource &source, const pf::BookFillOptions &options,
                    const QString &contentType)
{
    QVariantList out;
    if (source.items.empty())
        return out;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId);
    for (const pf::BookSlide &s : pf::FillBookSlides(tmpl, source, options))
        // `contentType` rides along ("scripture" | "table"): the OUTPUT STYLE's
        // own engine template must not steamroll a DIFFERENT tab's layout —
        // the style's template only applies to its own content family.
        // "" background: the template is layout only (see templateBlocks).
        out.append(QVariantMap{ { QStringLiteral("title"), QString::fromStdString(s.title) }, { QStringLiteral("background"), QString() },
                                { QStringLiteral("blocks"), blocksToVariants(s.blocks) },
                                { QStringLiteral("parts"), partsToVariants(s.parts) },
                                // The BODY, beside the blocks that carry it. Once the
                                // template's slots are filled, the block list no longer
                                // says which one is the content (a table design's date
                                // code is just as text-bearing, and may come first), so
                                // the fill hands the body on and ShowConverter puts it on
                                // the slide's own `text`. Without it the engine guessed
                                // from block order and a code slot made every bound
                                // block render the CODE where the sermon text belonged.
                                { QStringLiteral("body"), QString::fromStdString(s.body) },
                                // ...and the rest of the vocabulary, so an output
                                // style's own template resolves the same tokens
                                // the preview does.
                                { QStringLiteral("fill"), valuesToMeta(s.values) },
                                { QStringLiteral("contentType"), contentType } });
    return out;
}

} // namespace SlideBuilder
