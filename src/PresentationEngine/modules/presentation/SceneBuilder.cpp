#include "modules/presentation/SceneBuilder.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"

namespace bps::presentation {

std::string SceneBuilder::SceneIdFor(const Presentation& p, const Slide& s) {
    return "pres:" + p.id + ":" + s.id;
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    const std::string sceneId = SceneIdFor(presentation, slide);

    // Already built (cache hit) — return it.
    if (engine.GetScene(sceneId).ok()) return sceneId;

    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();

    // Layer order: background -> video -> image -> text -> overlay.
    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("text", "Text",
                                                    rendering::LayerKind::Text, 2));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    // Background: dark neutral (theme-driven later — Phase 11).
    auto bg = std::make_shared<rendering::BackgroundObject>(
        "bg", "Background", rendering::Color(0.06f, 0.07f, 0.09f, 1.0f));
    bg->SetBounds(rendering::Rect(0, 0, size.width, size.height));
    bg->SetLayer("bg");
    (void)engine.AddObject(sceneId, bg, "bg");

    // Title + body text objects.
    if (!slide.title.empty()) {
        auto title = std::make_shared<rendering::TextObject>("title", "Title", slide.title);
        title->SetBounds(rendering::Rect(80, 48, size.width - 160, 96));
        title->SetLayer("text");
        (void)engine.AddObject(sceneId, title, "text");
    }
    if (!slide.text.empty()) {
        auto body = std::make_shared<rendering::TextObject>("body", "Body", slide.text);
        body->SetBounds(rendering::Rect(80, 160, size.width - 160, size.height - 240));
        body->SetLayer("text");
        (void)engine.AddObject(sceneId, body, "text");
    }

    Logger::Instance().Debug("Scene built: " + sceneId, "SceneBuilder");
    return sceneId;
}

} // namespace bps::presentation
