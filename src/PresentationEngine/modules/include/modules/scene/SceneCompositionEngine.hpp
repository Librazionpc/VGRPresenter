#pragma once

// SceneCompositionEngine (docs/specs/23): the Phase 11 facade. Owns the widget
// registry, template library, theme library, layout library, and the Rule
// Engine. Resolves content → rules → template → theme → widgets → layouts →
// ComposedScene, which the Presentation Engine hands to the Rendering Engine.
// Content, style, and layout are fully separated.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/scene/SceneTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::scene {

class SceneCompositionEngine final : public IService {
public:
    static SceneCompositionEngine& Instance();

    // --- Lifecycle ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "SceneCompositionEngine"; }

    // --- Templates (scene blueprints + inheritance) ---
    Result<void> RegisterTemplate(const SceneTemplate& tpl);
    Result<void> RemoveTemplate(std::string_view id);
    Result<SceneTemplate> GetTemplate(std::string_view id) const;
    std::vector<std::string> TemplateIds() const;

    // --- Themes ---
    Result<void> RegisterTheme(const SceneTheme& theme);
    Result<void> RemoveTheme(std::string_view id);
    Result<SceneTheme> GetTheme(std::string_view id) const;
    std::vector<std::string> ThemeIds() const;

    // --- Layouts (standalone, reusable) ---
    Result<void> RegisterLayout(const SceneLayout& layout);
    Result<SceneLayout> GetLayout(std::string_view id) const;
    std::vector<std::string> LayoutIds() const;

    // --- Rules ---
    Result<void> AddRule(const SceneRule& rule);
    Result<void> RemoveRule(std::string_view id);
    std::vector<SceneRule> Rules() const;
    void ClearRules();

    // --- Resolution (docs/specs/23 §Dynamic layout resolution) ---
    // content → apply rules → select template → apply theme → resolve widgets
    // → per-output layouts. Emits scene.* events.
    Result<ComposedScene> Compose(std::string_view contentId, std::string_view contentType);

    // --- Live editing (docs/specs/23 §Live editing) ---
    // Moving/updating a widget updates the composed scene without restart.
    Result<void> UpdateWidgetVisibility(std::string_view widgetId, bool visible);
    Result<void> UpdateLayout(std::string_view layoutId, const SceneLayout& layout);

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);

    size_t RuleCount() const;

private:
    SceneCompositionEngine() = default;

    // Resolves a template's inherited fields (base + overrides).
    SceneTemplate ResolveTemplate(const SceneTemplate& tpl) const;

    mutable std::mutex mutex_;
    std::map<std::string, SceneTemplate, std::less<>> templates_;
    std::map<std::string, SceneTheme, std::less<>> themes_;
    std::map<std::string, SceneLayout, std::less<>> layouts_;
    std::vector<SceneRule> rules_;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> composeCount_{0};
};

} // namespace bps::scene
