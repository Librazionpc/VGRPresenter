// Unit tests: Scene Composition Engine (docs/specs/23).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests scene
#include "TestHarness.hpp"

void TestSceneComposition() {
    auto& eng = sc::SceneCompositionEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    // Theme.
    sc::SceneTheme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.primaryText = {1, 1, 1, 1};
    CHECK(eng.RegisterTheme(dark).ok());
    CHECK(eng.GetTheme("dark").ok());
    // Layout per output.
    sc::SceneLayout audience;
    audience.id = "aud-layout";
    audience.name = "Audience";
    audience.outputId = "audience";
    sc::LayoutItem lyrics;
    lyrics.regionId = "main";
    lyrics.anchor = sc::Anchor::Center;
    audience.items.push_back(lyrics);
    CHECK(eng.RegisterLayout(audience).ok());
    // Template (scene blueprint).
    sc::SceneTemplate songTpl;
    songTpl.id = "song-tpl";
    songTpl.name = "Song";
    songTpl.contentType = "song";
    songTpl.themeId = "dark";
    songTpl.regions.push_back(sc::Region{"main", "Main", 0, true});
    sc::WidgetDef lyricsWidget;
    lyricsWidget.id = "lyrics";
    lyricsWidget.type = "lyrics";
    lyricsWidget.regionId = "main";
    lyricsWidget.visibilityOutputs = {"audience", "stream"};
    songTpl.widgets.push_back(lyricsWidget);
    sc::WidgetDef clockWidget;
    clockWidget.id = "clock";
    clockWidget.type = "clock";
    clockWidget.regionId = "main";
    clockWidget.visibilityOutputs = {"stage"};
    songTpl.widgets.push_back(clockWidget);
    songTpl.layouts.push_back(audience);
    CHECK(eng.RegisterTemplate(songTpl).ok());
    // Template inheritance.
    sc::SceneTemplate youthTpl;
    youthTpl.id = "youth-song";
    youthTpl.name = "Youth Song";
    youthTpl.contentType = "song";
    youthTpl.baseTemplateId = "song-tpl";
    sc::WidgetDef ticker;
    ticker.id = "ticker";
    ticker.type = "ticker";
    youthTpl.widgets.push_back(ticker);
    CHECK(eng.RegisterTemplate(youthTpl).ok());
    // Rules.
    sc::SceneRule rule;
    rule.id = "song-on-audience";
    rule.contentType = "song";
    rule.outputId = "";
    rule.templateId = "song-tpl";
    rule.layoutId = "aud-layout";
    CHECK(eng.AddRule(rule).ok());
    CHECK(eng.RuleCount() == 1);
    // Compose: content -> rules -> template -> theme -> widgets -> layouts.
    auto composed = eng.Compose("content-1", "song");
    CHECK(composed.ok());
    CHECK(composed.value().templateId == "song-tpl");
    CHECK(composed.value().themeId == "dark");
    CHECK(composed.value().appliedRuleId == "song-on-audience");
    CHECK(composed.value().widgets.size() == 2);
    bool hasLyrics = false, hasClock = false;
    for (const auto& w : composed.value().widgets) {
        if (w.type == "lyrics") hasLyrics = true;
        if (w.type == "clock") hasClock = true;
    }
    CHECK(hasLyrics && hasClock);
    bool hasAudLayout = false;
    for (const auto& l : composed.value().layouts)
        if (l.id == "aud-layout") hasAudLayout = true;
    CHECK(hasAudLayout);
    // Inherited template resolves base widgets.
    auto inherited = eng.Compose("content-2", "song");
    CHECK(inherited.ok());
    // Live editing: hide a widget.
    CHECK(eng.UpdateWidgetVisibility("lyrics", false).ok());
    CHECK(!eng.UpdateWidgetVisibility("nope", false).ok());
    // Update layout live.
    sc::SceneLayout newAud = audience;
    newAud.items[0].xPct = 0.5f;
    CHECK(eng.UpdateLayout("aud-layout", newAud).ok());
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

// ===========================================================================
// Phase 12 — Bible Engine (docs/specs/24)
// ===========================================================================
