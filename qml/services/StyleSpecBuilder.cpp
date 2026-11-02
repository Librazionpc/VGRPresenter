#include "services/StyleSpecBuilder.h"

#include "FamilySlots.h"

#include <QLatin1String>
#include <QVariantList>
#include <QVariantMap>

#include "models/GroupListModel.h"
#include "models/OutputListModel.h"
#include "models/StyleListModel.h"
#include "services/DesignLibraryService.h"
#include "services/ShowConverter.h"

namespace {
namespace bp = bps::presentation;

void adoptTemplateBackground(const QVariantMap &design, bp::OutputStyleSpec &spec)
{
    const QString bg = design.value(QStringLiteral("background")).toString();
    if (!bg.isEmpty() && bg != QLatin1String("transparent")
        && (spec.backgroundColor.empty() || spec.backgroundColor == "transparent"))
        spec.backgroundColor = bg.toStdString();
}

void bakeWholeStyleTemplate(bp::OutputStyleSpec &spec)
{
    const QString key = QString::fromStdString(spec.templateKey);
    if (key.isEmpty() || key.startsWith(QLatin1String("tpl-")) == false)
        return;
    const QVariantMap design = TemplateLibraryService::instance().design(key);
    const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
    if (blocks.isEmpty())
        return;   // unknown/deleted id: keep the plain-layout fallback
    adoptTemplateBackground(design, spec);
    spec.templateBlocks.reserve(blocks.size());
    for (const QVariant &b : blocks)
        spec.templateBlocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
}

void bakeFamilyTemplates(bp::OutputStyleSpec &spec)
{
    for (size_t i = 0; i < 4; ++i) {
        const QString famKey = QString::fromStdString(spec.familyTemplateKeys[i]);
        if (famKey.isEmpty() || famKey.startsWith(QLatin1String("tpl-")) == false)
            continue;   // "" = none for this family; a preset key rides LayoutFor — no bake
        const QVariantMap design = TemplateLibraryService::instance().design(famKey);
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (blocks.isEmpty())
            continue;   // unknown/deleted id: the family falls back per SceneBuilder
        adoptTemplateBackground(design, spec);
        spec.familyTemplateBlocks[i].reserve(blocks.size());
        for (const QVariant &b : blocks)
            spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
    }
    const QString wholeKey = QString::fromStdString(spec.templateKey);
    if (wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap whole = TemplateLibraryService::instance().design(wholeKey);
        const QVariantList wholeBlocks = whole.value(QStringLiteral("blocks")).toList();
        if (!wholeBlocks.isEmpty()) {
            for (size_t i = 0; i < 4; ++i) {
                if (!spec.familyTemplateKeys[i].empty() || !spec.familyTemplateBlocks[i].empty())
                    continue;   // explicit pick (baked above) always wins
                spec.familyTemplateBlocks[i].reserve(wholeBlocks.size());
                for (const QVariant &b : wholeBlocks)
                    spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
            }
        }
    }
    if (spec.familyTemplateBlocks[0].empty()
        && QString::fromStdString(spec.familyTemplateKeys[0]).isEmpty()
        && !wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap design = TemplateLibraryService::instance().design(QStringLiteral("tpl-scripture"));
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (!blocks.isEmpty()) {
            spec.familyTemplateBlocks[0].reserve(blocks.size());
            for (const QVariant &b : blocks)
                spec.familyTemplateBlocks[0].push_back(ShowConverter::blockFromVariant(b.toMap()));
        }
    }
}

void bakeGroupTemplate(bp::OutputStyleSpec &spec, const QString &slideTag)
{
    if (slideTag.isEmpty())
        return;
    const QString key = GroupListModel::instance()->templateForTag(slideTag);
    if (key.isEmpty())
        return;
    const QVariantMap design = TemplateLibraryService::instance().design(key);
    const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
    if (blocks.isEmpty())
        return;   // unknown/deleted id: the style's own slot stands
    adoptTemplateBackground(design, spec);
    spec.familyTemplateKeys[bps::presentation::kFamilySlotShows] = key.toStdString();
    std::vector<bp::ContentBlock> &slot =
        spec.familyTemplateBlocks[bps::presentation::kFamilySlotShows];
    slot.clear();
    slot.reserve(blocks.size());
    for (const QVariant &b : blocks)
        slot.push_back(ShowConverter::blockFromVariant(b.toMap()));
}
} // namespace

namespace StyleSpecBuilder {
StyleListModel *uiStyleRoster()
{
    if (StyleListModel *adopted = OutputListModel::adoptedStyleRoster())
        return adopted;
    // The C++ singleton is only the fallback for a run where QML never handed
    // one over (a headless test, or a read before the hand-over).
    return StyleListModel::instance();
}

bps::presentation::OutputStyleSpec specForStyleRow(int row, const StyleListModel *styles,
                                                    const QString &slideTag)
{
    bps::presentation::OutputStyleSpec spec;
    if (!styles || row < 0 || row >= styles->rowCount())
        return spec;
    const QVariantMap style = styles->getStyle(row);
    spec.name = style.value(QStringLiteral("name")).toString().toStdString();
    spec.contentType = style.value(QStringLiteral("contentType")).toString().toStdString();
    spec.templateKey = style.value(QStringLiteral("templateKey")).toString().toStdString();
    spec.backgroundColor = style.value(QStringLiteral("backgroundColor")).toString().toStdString();
    spec.backgroundImage = style.value(QStringLiteral("backgroundImage")).toString().toStdString();
    spec.clearBackgroundOnText =
        style.value(QStringLiteral("clearBackgroundOnText")).toBool();
    for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
        const FamilySlots::Family &fam = FamilySlots::Rows()[static_cast<std::size_t>(slot)];
        spec.SetFamilyGate(static_cast<std::size_t>(slot),
                           style.value(QString::fromLatin1(fam.showProperty)).toBool());
        spec.familyTemplateKeys[static_cast<std::size_t>(slot)] =
            style.value(QString::fromLatin1(fam.templateProperty)).toString().toStdString();
    }
    const QVariantList categoryTemplates = style.value(QStringLiteral("showCategoryTemplates")).toList();
    spec.showCategoryTemplates.reserve(static_cast<std::size_t>(categoryTemplates.size()));
    for (const QVariant &value : categoryTemplates) {
        const QVariantMap entry = value.toMap();
        bps::presentation::OutputStyleSpec::ShowCategoryTemplate categoryTemplate;
        categoryTemplate.category = entry.value(QStringLiteral("category")).toString().trimmed().toStdString();
        categoryTemplate.templateKey = entry.value(QStringLiteral("templateKey")).toString().trimmed().toStdString();
        categoryTemplate.overrideEnabled = entry.value(QStringLiteral("overrideEnabled")).toBool();
        if (categoryTemplate.category.empty() || categoryTemplate.templateKey.empty())
            continue;
        const QVariantMap design = TemplateLibraryService::instance().design(
            QString::fromStdString(categoryTemplate.templateKey));
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        categoryTemplate.blocks.reserve(static_cast<std::size_t>(blocks.size()));
        for (const QVariant &block : blocks)
            categoryTemplate.blocks.push_back(ShowConverter::blockFromVariant(block.toMap()));
        if (!categoryTemplate.blocks.empty())
            spec.showCategoryTemplates.push_back(std::move(categoryTemplate));
    }
    spec.showOverlay = style.value(QStringLiteral("showOverlay")).toBool();
    spec.category = style.value(QStringLiteral("category")).toString().toStdString();
    bakeWholeStyleTemplate(spec);
    bakeFamilyTemplates(spec);
    bakeGroupTemplate(spec, slideTag);
    return spec;
}

bps::presentation::OutputStyleSpec specForStyleId(const QString &styleId, const QString &slideTag)
{
    StyleListModel *styles = uiStyleRoster();
    if (!styles)
        return {};
    return specForStyleRow(styles->rowForId(styleId), styles, slideTag);
}
} // namespace StyleSpecBuilder