#include "services/SlidePreviewMap.h"

#include "services/MediaOnAirState.h"
#include "services/ShowConverter.h"
#include "services/StyleSpecBuilder.h"

#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/SlideRules.hpp"

#include <QFile>

namespace pl = bps::presentation;

using services::media_on_air::g_mediaFitOnAir;

namespace services::preview {
pl::OutputStyleSpec outputStyleForPreview(const QString &styleId, const QString &slideTag)
{
    return StyleSpecBuilder::specForStyleId(styleId, slideTag);
}

QVariantMap slideToVariantMap(const pl::Slide *slide, const pl::OutputStyleSpec &style)
{
    if (!slide)
        return {};
    const pl::SceneBuilder::SlideComposition comp =
        pl::SceneBuilder::ComposeSlide(*slide, style, /*applyGates=*/true);

    const QString styleImage = QString::fromStdString(style.backgroundImage);
    const bool styleImageOnDisk = !styleImage.isEmpty() && QFile::exists(styleImage);
    const bool stylePaintsColor = !style.backgroundColor.empty()
        && pl::StyleBuilder::ParseColor(style.backgroundColor).a > 0.0f;
    const bool stylePaints = stylePaintsColor || styleImageOnDisk;
    const bool stylePaintless = comp.clearedByTake || !stylePaints
        || (styleImageOnDisk && comp.styleImageSuppressed);
    const bool styleImagePainted = !comp.clearedByTake && styleImageOnDisk
        && !comp.styleImageSuppressed;
    const std::vector<pl::ContentBlock> &drawn =
        comp.source == pl::SceneBuilder::SlideComposition::Source::SlideBlocks
            ? slide->blocks : comp.blocks;
    const bool framePriority = pl::SlideRules::NeedsDistributedFrame(drawn);
    const QVariantMap styleFacts{
        { QStringLiteral("styleColor"), comp.clearedByTake
            ? QStringLiteral("transparent")
            : QString::fromStdString(style.backgroundColor) },
        { QStringLiteral("styleImage"), styleImage },
        { QStringLiteral("stylePaints"), stylePaints && !comp.clearedByTake },
        { QStringLiteral("stylePaintless"), stylePaintless },
        { QStringLiteral("styleImagePainted"), styleImagePainted },
        { QStringLiteral("framePriority"), framePriority },
        { QStringLiteral("explicitTransparent"), comp.explicitTransparent },
        { QStringLiteral("frameTransparent"), comp.frameTransparent },
        { QStringLiteral("clearedByTake"), comp.clearedByTake },
        { QStringLiteral("mediaFit"), QString::fromStdString(
              g_mediaFitOnAir.empty() ? std::string("contain") : g_mediaFitOnAir) },
    };

    const QString background = comp.explicitTransparent
        ? QStringLiteral("transparent")
        : QString::fromStdString(comp.background);

    if (comp.source == pl::SceneBuilder::SlideComposition::Source::Cleared) {
        QVariantMap cleared = styleFacts;
        cleared.insert(QStringLiteral("styleColor"), QStringLiteral("transparent"));
        cleared.insert(QStringLiteral("stylePaints"), false);
        cleared.insert(QStringLiteral("stylePaintless"), true);
        cleared.insert(QStringLiteral("styleImagePainted"), false);
        cleared.insert(QStringLiteral("frameTransparent"), true);
        cleared.insert(QStringLiteral("valid"), true);
        cleared.insert(QStringLiteral("title"), QString());
        cleared.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
        cleared.insert(QStringLiteral("blocks"), QVariantList{});
        cleared.insert(QStringLiteral("background"), QStringLiteral("transparent"));
        cleared.insert(QStringLiteral("suppressStyleImage"), true);
        cleared.insert(QStringLiteral("cleared"), true);
        cleared.insert(QStringLiteral("clearStage"), true);
        return cleared;
    }

    if (comp.source == pl::SceneBuilder::SlideComposition::Source::Refused) {
        QVariantMap refused = styleFacts;
        refused.insert(QStringLiteral("valid"), true);
        refused.insert(QStringLiteral("title"), QString());
        refused.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
        refused.insert(QStringLiteral("blocks"), QVariantList{});
        refused.insert(QStringLiteral("background"), background);
        refused.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
        refused.insert(QStringLiteral("refusedByStyle"), true);
        return refused;
    }

    QVariantList blocks;
    using Source = pl::SceneBuilder::SlideComposition::Source;
    if (comp.source == Source::FamilyTemplate || comp.source == Source::StyleTemplate) {
        blocks = ShowConverter::blocksToVariant(comp.blocks);
    } else if (comp.source == Source::FamilyPreset || comp.source == Source::StylePreset) {
        const double kCanvasW = 754.0, kCanvasH = 428.0, kEngineStageH = 1080.0;
        auto presetRegion = [&](const pl::StyleBuilder::LayoutRect &r, const QString &text) {
            QVariantMap region{
                { QStringLiteral("key"), QString() },
                { QStringLiteral("kind"), QStringLiteral("text") },
                { QStringLiteral("text"), text },
                { QStringLiteral("x"), r.x * kCanvasW },
                { QStringLiteral("y"), r.y * kCanvasH },
                { QStringLiteral("width"), r.w * kCanvasW },
                { QStringLiteral("height"), r.h * kCanvasH },
                { QStringLiteral("bind"), QString() },
                { QStringLiteral("meta"), QVariantMap{
                    { QStringLiteral("fontSize"), 24.0 * kCanvasH / kEngineStageH },
                    { QStringLiteral("color"), QStringLiteral("#ffffff") },
                    { QStringLiteral("align"), QStringLiteral("left") },
                    { QStringLiteral("verticalAlign"), QStringLiteral("top") },
                } },
                { QStringLiteral("style"), QVariantMap{
                    { QStringLiteral("backgroundColor"), QStringLiteral("transparent") },
                } },
            };
            blocks.append(region);
        };
        if (!slide->title.empty() && comp.preset.showTitle)
            presetRegion(comp.preset.title, QString::fromStdString(slide->title));
        if (!slide->text.empty())
            presetRegion(comp.preset.body, QString::fromStdString(slide->text));
    } else {
        blocks = ShowConverter::blocksToVariant(slide->blocks);
    }

    QVariantMap composed = styleFacts;
    composed.insert(QStringLiteral("valid"), true);
    composed.insert(QStringLiteral("title"), QString::fromStdString(slide->title));
    composed.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
    composed.insert(QStringLiteral("blocks"), blocks);
    composed.insert(QStringLiteral("background"), background);
    composed.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
    composed.insert(QStringLiteral("body"), QString::fromStdString(slide->text));
    composed.insert(QStringLiteral("fill"),
                    ShowConverter::fillFromMeta(ShowConverter::metaOf(*slide)));
    composed.insert(QStringLiteral("clearStage"), slide->clearStage);
    // WHAT A LATER COMPOSITION NEEDS TO PICK THE SAME DESIGN. This map is the on-air slide the monitor
    // tiles compose AGAIN, each with its own style (previewWithStyle). Without the category the
    // slide belongs to, a tile cannot find the style's override for it and falls back to the style's
    // default Shows template: a Quiet Time slide looked right on the monitor, and the moment GO LIVE
    // put it on air the tiles redrew it as "Big Bold" / "Lower Third White". The staged slide kept
    // these fields; the on-air copy lost them. (The engine's own outputs read them from the slide
    // itself and were never wrong.)
    {
        const QVariantMap meta = ShowConverter::metaOf(*slide);
        composed.insert(QStringLiteral("showCategory"), meta.value(QStringLiteral("showCategory")).toString());
        composed.insert(QStringLiteral("templateOverride"), meta.value(QStringLiteral("templateOverride")).toBool());
        // The reference label's source: a style's label block binds to the slide's meta "ref" (and only falls back to the
        // TITLE when there is none). The reading walk moves that value verse by verse (LiveOutputService::setWalkReference);
        // a later composition of this map must see it, or it would draw the pick's own title again.
        if (const QString ref = meta.value(QStringLiteral("ref")).toString(); !ref.isEmpty())
            composed.insert(QStringLiteral("ref"), ref);
        if (!slide->tags.empty())
            composed.insert(QStringLiteral("tag"), QString::fromStdString(slide->tags.front()));
    }
    return composed;
}

QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    return slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle());
}

void setSlideReadingFlow(QVariantMap &slide, bool enabled, const QString &flowText, qreal flowChar)
{
    if (enabled && !flowText.isEmpty()) {
        slide.insert(QStringLiteral("flowText"), flowText);
        slide.insert(QStringLiteral("flowChar"), flowChar);
    } else {
        slide.remove(QStringLiteral("flowText"));
        slide.remove(QStringLiteral("flowChar"));
    }
}
} // namespace services::preview