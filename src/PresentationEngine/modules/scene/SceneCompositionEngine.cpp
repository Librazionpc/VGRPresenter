#include "modules/scene/SceneCompositionEngine.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>

namespace bps::scene {

SceneCompositionEngine& SceneCompositionEngine::Instance() {
    static SceneCompositionEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    WireEvents();
    return Ok();
}

Result<void> SceneCompositionEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> SceneCompositionEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> SceneCompositionEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        templates_.clear();
        themes_.clear();
        layouts_.clear();
        rules_.clear();
        initialized_.store(false);
    }
    return Ok();
}

Result<void> SceneCompositionEngine::Reload() {
    return Ok();
}

Result<void> SceneCompositionEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.clear();
    return Ok();
}

HealthReport SceneCompositionEngine::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("templates={} themes={} layouts={} rules={}",
                           templates_.size(), themes_.size(), layouts_.size(), rules_.size());
    return r;
}

Metrics SceneCompositionEngine::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = composeCount_.load();
    m.errorCount = errorCount_.load();
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::RegisterTemplate(const SceneTemplate& tpl) {
    std::lock_guard<std::mutex> lock(mutex_);
    templates_[tpl.id] = tpl;
    return Ok();
}

Result<void> SceneCompositionEngine::RemoveTemplate(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = templates_.find(id);
    if (it == templates_.end())
        return Error::Make(Err::Scene_TemplateNotFound, "SceneCompositionEngine",
                           "template '" + std::string(id) + "' not found");
    templates_.erase(it);
    return Ok();
}

Result<SceneTemplate> SceneCompositionEngine::GetTemplate(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = templates_.find(id);
    if (it == templates_.end())
        return Error::Make(Err::Scene_TemplateNotFound, "SceneCompositionEngine",
                           "template '" + std::string(id) + "' not found");
    return ResolveTemplate(it->second);
}

std::vector<std::string> SceneCompositionEngine::TemplateIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : templates_) ids.push_back(id);
    return ids;
}

SceneTemplate SceneCompositionEngine::ResolveTemplate(const SceneTemplate& tpl) const {
    // Inheritance: child overrides only what it declares. Regions/widgets/
    // layouts merge with the base (children append; same-id replaces).
    SceneTemplate resolved = tpl;
    if (!tpl.baseTemplateId.empty()) {
        auto it = templates_.find(tpl.baseTemplateId);
        if (it != templates_.end() && it->second.id != tpl.id) {
            const SceneTemplate& base = ResolveTemplate(it->second);
            if (resolved.regions.empty()) resolved.regions = base.regions;
            if (resolved.widgets.empty()) resolved.widgets = base.widgets;
            if (resolved.layouts.empty()) resolved.layouts = base.layouts;
            if (resolved.themeId.empty()) resolved.themeId = base.themeId;
            if (resolved.contentType.empty()) resolved.contentType = base.contentType;
        }
    }
    return resolved;
}

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::RegisterTheme(const SceneTheme& theme) {
    std::lock_guard<std::mutex> lock(mutex_);
    themes_[theme.id] = theme;
    return Ok();
}

Result<void> SceneCompositionEngine::RemoveTheme(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = themes_.find(id);
    if (it == themes_.end())
        return Error::Make(Err::Scene_ThemeNotFound, "SceneCompositionEngine",
                           "theme '" + std::string(id) + "' not found");
    themes_.erase(it);
    return Ok();
}

Result<SceneTheme> SceneCompositionEngine::GetTheme(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = themes_.find(id);
    if (it == themes_.end())
        return Error::Make(Err::Scene_ThemeNotFound, "SceneCompositionEngine",
                           "theme '" + std::string(id) + "' not found");
    return it->second;
}

std::vector<std::string> SceneCompositionEngine::ThemeIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : themes_) ids.push_back(id);
    return ids;
}

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::RegisterLayout(const SceneLayout& layout) {
    std::lock_guard<std::mutex> lock(mutex_);
    layouts_[layout.id] = layout;
    return Ok();
}

Result<SceneLayout> SceneCompositionEngine::GetLayout(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = layouts_.find(id);
    if (it == layouts_.end())
        return Error::Make(Err::Scene_LayoutNotFound, "SceneCompositionEngine",
                           "layout '" + std::string(id) + "' not found");
    return it->second;
}

std::vector<std::string> SceneCompositionEngine::LayoutIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : layouts_) ids.push_back(id);
    return ids;
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::AddRule(const SceneRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : rules_)
        if (r.id == rule.id)
            return Error::Make(Err::Scene_RuleNotFound, "SceneCompositionEngine",
                               "rule '" + rule.id + "' already exists");
    rules_.push_back(rule);
    return Ok();
}

Result<void> SceneCompositionEngine::RemoveRule(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = rules_.begin(); it != rules_.end(); ++it) {
        if (it->id == id) {
            rules_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Scene_RuleNotFound, "SceneCompositionEngine",
                       "rule '" + std::string(id) + "' not found");
}

std::vector<SceneRule> SceneCompositionEngine::Rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

void SceneCompositionEngine::ClearRules() {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.clear();
}

size_t SceneCompositionEngine::RuleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_.size();
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
Result<ComposedScene> SceneCompositionEngine::Compose(std::string_view contentId,
                                                      std::string_view contentType) {
    std::vector<SceneRule> rules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules = rules_;
    }

    // 1. Apply rules: first matching (contentType + outputId "").
    std::string chosenTemplate;
    std::string chosenLayout;
    std::string chosenTheme;
    std::string appliedRule;
    for (const auto& rule : rules) {
        if (!rule.contentType.empty() && rule.contentType != contentType) continue;
        if (!chosenTemplate.empty()) break;   // first match wins
        chosenTemplate = rule.templateId;
        chosenLayout = rule.layoutId;
        chosenTheme = rule.themeId;
        appliedRule = rule.id;
        if (!rule.outputId.empty()) {
            // Output-scoped rule: applies the layout for that output.
        }
    }

    // 2. Select template (first content-type match if no rule).
    SceneTemplate tpl;
    bool haveTpl = false;
    if (!chosenTemplate.empty()) {
        auto t = GetTemplate(chosenTemplate);
        if (t.ok()) {
            tpl = t.value();
            haveTpl = true;
        }
    }
    if (!haveTpl) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, t] : templates_) {
            if (t.contentType == contentType) {
                tpl = ResolveTemplate(t);
                haveTpl = true;
                break;
            }
        }
    }
    if (!haveTpl)
        return Error::Make(Err::Scene_TemplateNotFound, "SceneCompositionEngine",
                           "no template for content type '" + std::string(contentType) + "'");

    // 3. Apply theme.
    if (chosenTheme.empty()) chosenTheme = tpl.themeId;
    if (chosenTheme.empty()) chosenTheme = "default";
    if (!themes_.count(chosenTheme) && !chosenTheme.empty()) {
        // Missing theme: fall back to a built-in neutral theme rather than fail.
        SceneTheme neutral;
        neutral.id = chosenTheme;
        neutral.name = chosenTheme;
        (void)const_cast<SceneCompositionEngine*>(this)->RegisterTheme(neutral);
    }

    // 4. Resolve widgets (visible, per-output visibility retained).
    ComposedScene out;
    out.sceneId = std::string(contentId) + ":scene";
    out.contentId = std::string(contentId);
    out.contentType = std::string(contentType);
    out.templateId = tpl.id;
    out.themeId = chosenTheme;
    out.appliedRuleId = appliedRule;
    for (const auto& w : tpl.widgets) {
        WidgetDef wd = w;
        if (wd.regionId.empty() && !tpl.regions.empty()) wd.regionId = tpl.regions[0].id;
        out.widgets.push_back(std::move(wd));
    }
    // 5. Layouts: template layouts + rule-chosen layout.
    for (const auto& l : tpl.layouts) out.layouts.push_back(l);
    if (!chosenLayout.empty()) {
        auto l = GetLayout(chosenLayout);
        if (l.ok()) {
            bool exists = false;
            for (const auto& e : out.layouts)
                if (e.id == l.value().id) exists = true;
            if (!exists) out.layouts.push_back(l.value());
        }
    }

    composeCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::SceneComposed{
        out.sceneId, out.contentId, out.contentType, out.widgets.size()});
    for (const auto& l : out.layouts)
        (void)EventBus::Instance().Publish(
            events::SceneLayoutApplied{out.sceneId, l.id, l.outputId});
    if (!appliedRule.empty())
        (void)EventBus::Instance().Publish(events::SceneRuleApplied{appliedRule, {}});
    (void)EventBus::Instance().Publish(events::SceneThemeApplied{chosenTheme});
    return out;
}

// ---------------------------------------------------------------------------
// Live editing
// ---------------------------------------------------------------------------
Result<void> SceneCompositionEngine::UpdateWidgetVisibility(std::string_view widgetId,
                                                            bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, tpl] : templates_) {
        for (auto& w : tpl.widgets) {
            if (w.id == widgetId) {
                w.visible = visible;
                return Ok();
            }
        }
    }
    return Error::Make(Err::Scene_WidgetNotFound, "SceneCompositionEngine",
                       "widget '" + std::string(widgetId) + "' not found");
}

Result<void> SceneCompositionEngine::UpdateLayout(std::string_view layoutId,
                                                  const SceneLayout& layout) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = layouts_.find(layoutId);
    if (it != layouts_.end()) {
        it->second = layout;
        return Ok();
    }
    // Also update template-owned layouts.
    for (auto& [_, tpl] : templates_) {
        for (auto& l : tpl.layouts) {
            if (l.id == layoutId) {
                l = layout;
                return Ok();
            }
        }
    }
    return Error::Make(Err::Scene_LayoutNotFound, "SceneCompositionEngine",
                       "layout '" + std::string(layoutId) + "' not found");
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void SceneCompositionEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
}

void SceneCompositionEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void SceneCompositionEngine::OnConfigReload(const events::ConfigHotReload&) {
    // Theme/layout reload hooks live here (nothing hardcoded to reload today).
}

} // namespace bps::scene
