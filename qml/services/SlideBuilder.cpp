#include "services/SlideBuilder.h"

#include "services/DesignLibraryService.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"

namespace pf = bps::presentation;

namespace SlideBuilder {

std::vector<pf::ContentBlock> templateBlocks(const QString &id, QString *background)
{
    std::vector<pf::ContentBlock> out;
    const QVariantMap design = TemplateLibraryService::instance().design(id);
    for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
        out.push_back(ShowConverter::blockFromVariant(b.toMap()));
    if (background)
        *background = design.value(QStringLiteral("background")).toString();
    return out;
}

QVariantList blocksToVariants(const std::vector<pf::ContentBlock> &blocks)
{
    QVariantList out;
    for (const pf::ContentBlock &b : blocks)
        out.append(ShowConverter::blockToVariant(b));
    return out;
}

QVariantList partsToVariants(const std::vector<pf::ScriptureSlidePart> &parts)
{
    QVariantList out;
    out.reserve(static_cast<qsizetype>(parts.size()));
    for (const pf::ScriptureSlidePart &part : parts) {
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
    return pf::HasScriptureValues(templateBlocks(id));
}

QVariantMap preview(const QString &templateId, const pf::ScriptureSource &source, const pf::ScriptureSettings &options)
{
    QVariantMap out{ { QStringLiteral("blocks"), QVariantList() }, { QStringLiteral("background"), QString() },
                     { QStringLiteral("reference"), QString() }, { QStringLiteral("hasValues"), true },
                     { QStringLiteral("slideCount"), 0 }, { QStringLiteral("slides"), QVariantList() } };
    if (source.verses.empty())
        return out;
    QString background;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId, &background);
    const auto generated = pf::BuildScriptureSlides(tmpl, source, options);
    if (generated.empty())
        return out;
    out[QStringLiteral("blocks")] = blocksToVariants(generated.front().blocks);
    out[QStringLiteral("background")] = background;
    out[QStringLiteral("reference")] = QString::fromStdString(generated.front().reference);
    out[QStringLiteral("hasValues")] = pf::HasScriptureValues(tmpl);
    QVariantList slideGroups;
    slideGroups.reserve(static_cast<qsizetype>(generated.size()));
    for (const pf::ScriptureSlide &slide : generated) {
        QVariantList verses;
        verses.reserve(static_cast<qsizetype>(slide.verses.size()));
        for (int verse : slide.verses)
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

QVariantList slides(const QString &templateId, const pf::ScriptureSource &source, const pf::ScriptureSettings &options,
                    const QString &contentType)
{
    QVariantList out;
    if (source.verses.empty())
        return out;
    QString background;
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId, &background);
    for (const pf::ScriptureSlide &s : pf::BuildScriptureSlides(tmpl, source, options))
        // `contentType` rides along ("scripture" | "table"): the OUTPUT STYLE's
        // own engine template must not steamroll a DIFFERENT tab's layout —
        // the style's template only applies to its own content family.
        out.append(QVariantMap{ { QStringLiteral("title"), QString::fromStdString(s.title) }, { QStringLiteral("background"), background },
                                { QStringLiteral("blocks"), blocksToVariants(s.blocks) },
                                { QStringLiteral("parts"), partsToVariants(s.parts) },
                                { QStringLiteral("contentType"), contentType } });
    return out;
}

} // namespace SlideBuilder
