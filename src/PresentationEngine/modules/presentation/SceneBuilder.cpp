#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/BlockMeta.hpp"
#include "modules/presentation/MediaPixels.hpp"
#include "modules/presentation/ScenePaint.hpp"
#include "modules/presentation/SlideBinding.hpp"
#include "modules/presentation/TextFormat.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"

#include "core/logging/Logger.hpp"
#include "core/config/Json.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"
#include "platform/IVideo.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <format>
#include <map>
#include <mutex>
#include <QFont>
#include <QTextBoundaryFinder>
#include <QString>
#include <set>
#include <string_view>

#include "modules/media/FrameSource.hpp"
#include "modules/rendering/PngCodec.hpp"

namespace bps::presentation {
// ---------------------------------------------------------------------------

SceneBuilder::BlockLayerPlan SceneBuilder::PlanBlockLayers(const std::vector<ContentBlock>& blocks) {
    constexpr double kBlockStageW = bps::library::kStageWidth, kBlockStageH = bps::library::kStageHeight;
    BlockLayerPlan plan;
    plan.underVideo.assign(blocks.size(), false);
    double accX0 = 0.0, accY0 = 0.0, accX1 = 0.0, accY1 = 0.0;
    bool any = false, covered = false;
    for (size_t i = 0; i < blocks.size() && !covered; ++i) {
        const ContentBlock& b = blocks[i];
        if (b.kind == "text" && !HasOwnBox(b))
            continue;                 // the design's text: no backdrop, run goes on
        if (!IsBoxKind(b) && !HasOwnBox(b))
            break;                    // clock/timer/image/media: the run ends here
        if (!MetaBoolToken(b.metaJson, "underVideo").empty())
            continue;                 // the author decided this one — not the rule
        const double bx0 = b.x, by0 = b.y;
        const double bx1 = b.x + b.width, by1 = b.y + b.height;
        if (!any) {
            accX0 = bx0; accY0 = by0; accX1 = bx1; accY1 = by1;
            any = true;
        } else {
            accX0 = std::min(accX0, bx0);
            accY0 = std::min(accY0, by0);
            accX1 = std::max(accX1, bx1);
            accY1 = std::max(accY1, by1);
        }
        constexpr double kStageTolX = 0.02 * kBlockStageW;
        constexpr double kStageTolY = 0.02 * kBlockStageH;
        covered = accX0 <= kStageTolX && accY0 <= kStageTolY
            && accX1 >= kBlockStageW - kStageTolX
            && accY1 >= kBlockStageH - kStageTolY;
        if (covered) {
            // The whole leading run IS the backdrop — every block that got it
            // there, not just the last one.
            for (size_t j = 0; j <= i; ++j)
                if (IsBoxKind(blocks[j]) || HasOwnBox(blocks[j]))
                    plan.underVideo[j] = true;
        }
    }
    plan.stageCovered = covered;
    return plan;
}

// ---------------------------------------------------------------------------
rendering::Color StyleBuilder::ParseColor(std::string_view css) {
    if (!IsHexColor(css))
        return rendering::Color(0, 0, 0, 0);

    const auto channel = [&css](size_t offset) {
        uint8_t v = 255;
        // Two hex digits starting at `offset` (after '#').
        std::from_chars(css.data() + offset, css.data() + offset + 2, v, 16);
        return static_cast<float>(v) / 255.0f;
    };
    if (css.size() == 7)
        return rendering::Color(channel(1), channel(3), channel(5), 1.0f);
    return rendering::Color(channel(3), channel(5), channel(7), channel(1));
}

// ---------------------------------------------------------------------------
// SceneBuilder
// ---------------------------------------------------------------------------
std::string SceneBuilder::SceneIdFor(const Presentation& p, const Slide& s) {
    return "pres:" + p.id + ":" + s.id;
}

std::string SceneBuilder::StyledSceneIdFor(const Presentation& p, const Slide& s,
                                           const OutputStyleSpec& style,
                                           rendering::Size sceneSize) {
    size_t familyIdx = SlideRules::FamilyTemplateSlot(SlideRules::ContentType(s));
    const std::string &famKey = familyIdx < 4 ? style.familyTemplateKeys[familyIdx]
                                              : std::string();
    const uint64_t famHash = familyIdx < 4 && !style.familyTemplateBlocks[familyIdx].empty()
                                 ? HashBlocks(style.familyTemplateBlocks[familyIdx])
                                 : 0;
    std::string id = SceneIdFor(p, s) + std::format("@s:{}_{}_{}_{}_{:016x}_{}_{:016x}",
                                                   style.backgroundColor,
                                                   style.templateKey,
                                                   style.clearBackgroundOnText ? 1 : 0,
                                                   style.backgroundImage,
                                                   HashBlocks(style.templateBlocks),
                                                   famKey,
                                                   famHash);
    const int sw = static_cast<int>(sceneSize.width);
    const int sh = static_cast<int>(sceneSize.height);
    if (sw > 0 && sh > 0 && (sw != 1920 || sh != 1080))
        id += std::format("@size:{}x{}", sw, sh);
    if (style.readingFontPx > 0.0f)
        id += std::format("@rf:{:.2f}", static_cast<double>(style.readingFontPx));
    if (!style.readingLineStarts.empty())
        id += std::format("@lt:{:016x}_{}", style.readingLineTextHash, style.readingLineStarts.size());
    if (presentation::CompositorState::Instance().HasMediaFrame())
        id += "@m:on";
    if (style.gateVideoBuses) {
        std::vector<int> buses = style.allowedVideoBuses;
        std::sort(buses.begin(), buses.end());
        buses.erase(std::unique(buses.begin(), buses.end()), buses.end());
        std::string token = "@gb:";
        for (size_t i = 0; i < buses.size(); ++i) {
            if (i > 0)
                token += ",";
            token += std::to_string(buses[i]);
        }
        if (buses.empty())
            token += "none";
        id += token;
    }
    return id;
}
} // namespace bps::presentation