#pragma once

// ---- ScenePaint: the block → draw-command translators ------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "modules/presentation/PresentationTypes.hpp"  // ContentBlock
#include "modules/rendering/IGraphicsBackend.hpp"      // RgbaImage
#include "modules/rendering/RenderEngine.hpp"          // RenderEngine
#include "modules/rendering/RenderTypes.hpp"           // Rect, Size, BlendMode
#include "modules/rendering/TextEngine.hpp"            // TextStyle, FontManager

namespace bps::presentation {
// ---- geometry + measurement ------------------------------------------------
// Largest font size ≤ `style.size` whose wrapped layout still fits `box` — the
// ONE fit solver both auto-size modes ride (the canvas's own shape too).
float BisectFittedSize(const std::string& text, const rendering::TextStyle& style,
                       const rendering::Rect& box, const rendering::FontManager& fonts,
                       float minPx, float maxPx);

// The two block shapes that can BE a backdrop.
bool IsBoxKind(const ContentBlock& b);
bool HasOwnBox(const ContentBlock& b);

rendering::Rect BlockRect(const ContentBlock& block, const rendering::Size& size);

// ---- the stage-block pass --------------------------------------------------
int AddStageBlocks(rendering::RenderEngine& engine, const std::string& sceneId,
                   const std::vector<ContentBlock>& blocks,
                   const rendering::Size& size, std::string_view tag,
                   std::string_view layer = "text",
                   const std::string& flowText = {},
                   float readingFontPx = 0.0f);

// ---- compositor video layers (taken input, media on air) --------------------
// Raw RGBA8 bytes (row-major, top-down) → RgbaImage. A straight copy is correct:
// RgbaImage::pixels is the same packed layout.
rendering::RgbaImage ToRgbaImage(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);

// Draw a live layer ONLY when its picture moved (see the definition's note: the
// seq counters make "is this the frame I already drew?" an integer compare).
void UpsertVideoLayer(rendering::RenderEngine& engine, const std::string& sceneId,
                      const std::string& objId, const rendering::RgbaImage& img,
                      const rendering::Rect& bounds, uint64_t sourceSeq = 0,
                      rendering::BlendMode blend = rendering::BlendMode::Alpha);
// MEASUREMENT HOOK: with BPS_LAYER_REFRESH_ALWAYS set every layer is re-copied on
// every build, exactly as before the skip existed (run_videoperf_test.sh).
bool LayerRefreshAlways();
bool LayerFrameCurrent(rendering::RenderEngine& engine, const std::string& sceneId,
                       const std::string& objId, uint64_t seq);

// ---- bus video windows (a "bus" block on the slide) ------------------------
int BusIdOf(const ContentBlock& block);
// Geometry is decided ONCE per build; the PIXELS are refreshed every build.
void RecordBusPlanes(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                     const rendering::Size& size, const OutputStyleSpec* gate = nullptr);
void RecordTemplateWindows(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                           const rendering::Size& size, int busNumber);
rendering::RgbaImage BusPlaceholder(const rendering::Rect& rect);
rendering::Rect BusFitRect(const rendering::RgbaImage& img, const rendering::Rect& box,
                           const std::string& fit);
void RefreshBusPlanes(rendering::RenderEngine& engine, const std::string& sceneId);

// ---- diagnostics -----------------------------------------------------------
// Writes `label layer <state> [sceneId]` once per scene per STATE CHANGE (not 60
// times a second). Both compositor layers and the block pass report through here.
void ReportLayerState(const std::string& sceneId, const std::string& label, std::string state);

// ---- the compositor + overlay pass -----------------------------------------
void AddCompositorLayers(rendering::RenderEngine& engine, const std::string& sceneId,
                         const rendering::Size& size, bool showOverlay = true);
} // namespace bps::presentation