// Unit tests: the app's settings (Settings > General and Smart Config), the hardware report, and backups / recovery.
//   ./bps_unit_tests settings
#include "TestHarness.hpp"

#include <algorithm>
#include <set>

#include "modules/settings/AppSettings.hpp"
#include "modules/settings/DataProtection.hpp"
#include "modules/settings/SmartConfig.hpp"

namespace {

namespace st = bps::settings;
using J = bps::json::Value;

const std::string kRoot = "/tmp/bps_settings_test";

bps::platform::IFilesystem& Fs() { return bps::platform::PlatformAccessor::Get().Filesystem(); }

} // namespace

void TestAppSettings() {
    (void)Fs().RemoveAll(kRoot);
    const std::string file = kRoot + "/settings.json";
    st::AppSettings s(file);
    CHECK(s.Load().ok());   // no file yet: just the defaults

    // ---- the defaults the engine ships ----
    CHECK(!s.GetBool("appearance.lockInMode"));
    CHECK(s.GetString("appearance.accent") == "purple" && s.GetString("appearance.theme") == "dark");
    CHECK(!s.GetBool("startup.openLastProject") && !s.GetBool("startup.launchAtLogin") && !s.GetBool("startup.autosave"));
    CHECK(s.GetBool("preferences.restoreLastSession") && !s.GetBool("preferences.closeToTray") && !s.GetBool("preferences.startMinimized"));
    CHECK(s.GetBool("backups.automatic") && s.GetInt("backups.intervalMinutes") == 30 && s.GetInt("backups.keepLast") == 10);
    CHECK(s.GetBool("backups.crashRecovery") && s.GetBool("notifications.show") && s.GetString("notifications.logLevel") == "info");
    CHECK(s.Profile() == "performance" && s.Mode() == "smart");
    CHECK(s.GetInt("smart.gpuBudgetPct") == 80 && s.GetInt("smart.cpuBudgetPct") == 60);

    // Every definition is sound: a unique key under its group, and a default that passes its own rules.
    std::set<std::string> keys;
    for (const st::SettingDef& d : st::AppSettings::Definitions()) {
        CHECK(keys.insert(d.key).second);
        CHECK(d.key.rfind(d.group + ".", 0) == 0);
        CHECK(!d.label.empty());
        CHECK(st::AppSettings::Find(d.key) == &d);
        CHECK(s.Set(d.key, d.dflt).ok());
        CHECK(s.Get(d.key).ToString() == d.dflt.ToString());
        if (d.kind == st::SettingKind::Choice) CHECK(!d.choices.empty());
    }
    CHECK(st::AppSettings::Find("nope.nothing") == nullptr);

    // ---- every rule, one at a time; a refused write changes nothing ----
    CHECK(!s.Set("nope.nothing", J::Bool(true)).ok());
    CHECK(s.Get("nope.nothing").isNull() && !s.GetBool("nope.nothing") && s.GetInt("nope.nothing") == 0);
    CHECK(!s.Set("appearance.lockInMode", J::String("yes")).ok());          // wrong type
    CHECK(!s.Set("appearance.accent", J::String("pink")).ok());             // not a choice
    CHECK(!s.Set("appearance.accent", J::Bool(true)).ok());
    CHECK(!s.Set("smart.gpuBudgetPct", J::Number(5)).ok());                 // below the range
    CHECK(!s.Set("smart.gpuBudgetPct", J::Number(101)).ok());               // above it
    CHECK(!s.Set("smart.gpuBudgetPct", J::Number(55.5)).ok());              // not whole
    CHECK(!s.Set("smart.gpuBudgetPct", J::String("55")).ok());
    CHECK(!s.Set("backups.keepLast", J::Number(7)).ok());                   // only the listed values
    CHECK(!s.Set("session.lastShowPath", J::String(std::string(5000, 'x'))).ok());
    CHECK(s.GetString("appearance.accent") == "purple" && s.GetInt("smart.gpuBudgetPct") == 80 && s.GetInt("backups.keepLast") == 10);

    // ---- listeners hear a real change, and only that ----
    std::vector<std::string> heard;
    const size_t id = s.Subscribe([&](const std::string& key) { heard.push_back(key); });
    CHECK(s.Set("appearance.accent", J::String("green")).ok());
    CHECK(s.Set("appearance.accent", J::String("green")).ok());             // the same value: nobody is told
    CHECK(!s.Set("appearance.accent", J::String("pink")).ok());             // a refused value: nobody is told
    CHECK(heard.size() == 1 && heard[0] == "appearance.accent");
    s.Unsubscribe(id);
    CHECK(s.Set("appearance.accent", J::String("blue")).ok() && heard.size() == 1);

    // ---- what changed is kept, and only that ----
    CHECK(s.Set("startup.autosave", J::Bool(true)).ok());
    CHECK(s.Set("backups.keepLast", J::Number(20)).ok());
    CHECK(s.Set("session.lastShowPath", J::String("C:/shows/Sunday.vgr")).ok());
    {
        st::AppSettings again(file);
        CHECK(again.Load().ok());
        CHECK(again.GetString("appearance.accent") == "blue" && again.GetBool("startup.autosave"));
        CHECK(again.GetInt("backups.keepLast") == 20 && again.GetString("session.lastShowPath") == "C:/shows/Sunday.vgr");
        CHECK(!again.GetBool("appearance.lockInMode"));
        auto text = Fs().ReadText(file);
        CHECK(text.ok() && text.value().find("lockInMode") == std::string::npos && text.value().find("appearance.accent") != std::string::npos);
    }

    // ---- a damaged file is reported and the defaults are used ----
    CHECK(Fs().Write(file, "{ not json").ok());
    { st::AppSettings broken(file); CHECK(!broken.Load().ok()); CHECK(broken.GetString("appearance.accent") == "purple"); }
    // ...and a value the rules refuse is dropped while the good ones stay
    CHECK(Fs().Write(file, R"({"version":1,"settings":{"appearance.accent":"pink","smart.gpuBudgetPct":55,"unknown.key":1}})").ok());
    { st::AppSettings partial(file); CHECK(partial.Load().ok());
      CHECK(partial.GetString("appearance.accent") == "purple" && partial.GetInt("smart.gpuBudgetPct") == 55); }

    // ---- the video board's roster rides the SAME registered-key contract as the audio one ----
    // (an unregistered key refuses every write — the old undeclared session.videoRoster toasted
    // "there is no setting 'session.videoRoster'" from Settings · Audio & Video)
    CHECK(s.Set("session.videoRoster", J::String(R"([{\"label\":\"Cam\",\"kind\":\"ndi\"}])")).ok());
    CHECK(!s.Set("session.notASetting", J::String("x")).ok());
    {
        st::AppSettings again(file);
        CHECK(again.Load().ok());
        CHECK(again.GetString("session.videoRoster") == R"([{\"label\":\"Cam\",\"kind\":\"ndi\"}])");
    }

    // ---- ResetAll: back to the defaults, remembering where the user was ----
    CHECK(s.Set("smart.mode", J::String("manual")).ok());
    const size_t changed = s.ResetAll();
    CHECK(changed >= 4);
    CHECK(s.Mode() == "smart" && s.GetString("appearance.accent") == "purple" && !s.GetBool("startup.autosave"));
    CHECK(s.GetString("session.lastShowPath") == "C:/shows/Sunday.vgr");
    CHECK(s.ResetAll() == 0);

    // ---- the resource caps: the profile's own, until the user takes over in Manual ----
    CHECK(s.EffectiveCaps().gpuPct == 80 && s.EffectiveCaps().cpuPct == 60);
    CHECK(s.Set("resources.profile", J::String("balanced")).ok());
    CHECK(s.EffectiveCaps().gpuPct == 65 && s.EffectiveCaps().cpuPct == 50);
    CHECK(s.Set("resources.profile", J::String("powerSaver")).ok());
    CHECK(s.EffectiveCaps().gpuPct == 40 && s.EffectiveCaps().cpuPct == 35);
    CHECK(s.Set("smart.mode", J::String("manual")).ok() && s.Set("smart.gpuBudgetPct", J::Number(90)).ok() && s.Set("smart.cpuBudgetPct", J::Number(70)).ok());
    CHECK(s.EffectiveCaps().gpuPct == 90 && s.EffectiveCaps().cpuPct == 70);
    const auto perf = st::AppSettings::AllocationFor("performance");
    CHECK(perf.renderingPct == 68 && perf.encodingPct == 42 && perf.outputPct == 55);
    for (const char* p : { "performance", "balanced", "powerSaver" }) {
        const auto a = st::AppSettings::AllocationFor(p);
        const auto c = st::AppSettings::CapsFor(p);
        CHECK(a.renderingPct > 0 && a.renderingPct <= 100 && a.encodingPct > 0 && a.outputPct > 0 && c.gpuPct > 0 && c.cpuPct > 0);
    }
    (void)Fs().RemoveAll(kRoot);
}

void TestSmartConfig() {
    namespace ad = bps::adaptive;

    // ---- what the report says about a strong machine, a modest one, and a laptop on battery ----
    ad::HardwareInfo strong;
    strong.coreCount = 12; strong.totalRamBytes = 32ull << 30;
    strong.gpuDetected = true; strong.gpuName = "NVIDIA GeForce RTX 4060"; strong.gpuVramTotalBytes = 8ull << 30;
    strong.displayCount = 3;
    const auto report = st::BuildHardwareReport(strong, true, { 4, 2 });
    CHECK(report.rows.size() == 4);
    CHECK(report.rows[0].key == "gpu" && report.rows[0].ok && report.rows[0].value == "NVIDIA GeForce RTX 4060 · 8 GB");
    CHECK(report.rows[1].key == "encoder" && report.rows[1].ok && report.rows[1].value == "NVENC available");
    CHECK(report.rows[2].key == "audio" && report.rows[2].ok && report.rows[2].value == "4 outputs, 2 inputs");
    CHECK(report.rows[3].key == "displays" && report.rows[3].ok && report.rows[3].value == "3 connected");
    CHECK(report.recommendedProfile == "performance" && !report.headline.empty() && !report.detail.empty());

    ad::HardwareInfo modest;
    modest.coreCount = 2; modest.totalRamBytes = 4ull << 30; modest.displayCount = 1;
    const auto lean = st::BuildHardwareReport(modest, false, { 1, 0 });
    CHECK(!lean.rows[0].ok && lean.rows[0].value.find("Not detected") != std::string::npos);
    CHECK(!lean.rows[1].ok && lean.rows[1].value == "Software encoding only");
    CHECK(lean.rows[2].value == "1 output, 0 inputs" && lean.rows[3].value == "1 connected");
    CHECK(lean.recommendedProfile == "balanced");
    CHECK(!st::BuildHardwareReport(modest, false, { 0, 0 }).rows[2].ok);
    { ad::HardwareInfo none; none.displayCount = 0; CHECK(st::BuildHardwareReport(none, false, {}).rows[3].value == "None found"); }

    ad::HardwareInfo laptop = strong;
    laptop.hasBattery = true; laptop.onBattery = true;
    CHECK(st::RecommendProfile(laptop) == "powerSaver");
    laptop.onBattery = false;
    CHECK(st::RecommendProfile(laptop) == "performance");
    ad::HardwareInfo fewCores = strong; fewCores.coreCount = 2;
    CHECK(st::RecommendProfile(fewCores) == "balanced");
    ad::HardwareInfo amd = strong; amd.gpuName = "AMD Radeon RX 7800"; CHECK(st::BuildHardwareReport(amd, true, {}).rows[1].value == "AMF available");
    ad::HardwareInfo intel = strong; intel.gpuName = "Intel Arc"; CHECK(st::BuildHardwareReport(intel, true, {}).rows[1].value == "Quick Sync available");

    // The report for this machine always has its four rows and a valid recommendation.
    const auto here = st::CurrentHardwareReport();
    CHECK(here.rows.size() == 4);
    CHECK(here.recommendedProfile == "performance" || here.recommendedProfile == "balanced" || here.recommendedProfile == "powerSaver");

    // ---- the settings, put to work in the adaptive runtime ----
    auto& rt = ad::AdaptiveRuntime::Instance();
    CHECK(rt.Initialize().ok());
    CHECK(rt.Start().ok());
    CHECK(!rt.SetResourceCaps(5, 50).ok() && !rt.SetResourceCaps(50, 101).ok());
    CHECK(rt.GpuCapPct() == 100 && rt.CpuCapPct() == 100);   // refused: nothing changed

    (void)Fs().RemoveAll(kRoot);
    st::AppSettings s(kRoot + "/settings.json");
    CHECK(s.Load().ok());
    CHECK(st::ApplyToEngine(s, rt).ok());                      // the defaults: Smart, Performance
    CHECK(rt.GetConfigLayer() == ad::ConfigLayer::Automatic && rt.GetUserMode() == ad::UserMode::Performance);
    CHECK(rt.GpuCapPct() == 80 && rt.CpuCapPct() == 60);
    CHECK(rt.Snapshot().gpuCapPct == 80 && rt.Snapshot().cpuCapPct == 60);
    {   // the CPU cap bounds the worker count
        const unsigned cores = std::max(1u, rt.Hardware().coreCount);
        CHECK(rt.GetRecommendedThreadCount() >= 1 && rt.GetRecommendedThreadCount() <= std::max(1u, (cores * 60 + 99) / 100));
    }

    CHECK(s.Set("resources.profile", J::String("powerSaver")).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetUserMode() == ad::UserMode::Battery && rt.GpuCapPct() == 40 && rt.CpuCapPct() == 35);
    CHECK(s.Set("resources.profile", J::String("balanced")).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetUserMode() == ad::UserMode::Balanced);

    CHECK(s.Set("smart.mode", J::String("strict")).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetConfigLayer() == ad::ConfigLayer::Assisted && rt.GetPreference() == ad::Preference::PreferLowMemory);

    CHECK(s.Set("smart.mode", J::String("manual")).ok() && s.Set("smart.gpuBudgetPct", J::Number(90)).ok() && s.Set("smart.cpuBudgetPct", J::Number(100)).ok());
    CHECK(st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetConfigLayer() == ad::ConfigLayer::Expert && rt.GetPreference() == ad::Preference::None);
    CHECK(rt.GpuCapPct() == 90 && rt.CpuCapPct() == 100);

    CHECK(s.Set("appearance.lockInMode", J::Bool(true)).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetUserMode() == ad::UserMode::Presentation);   // Lock In Mode: background work paused
    CHECK(!rt.BackgroundWorkAllowed());
    CHECK(s.Set("appearance.lockInMode", J::Bool(false)).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(rt.GetUserMode() == ad::UserMode::Balanced && rt.BackgroundWorkAllowed());

    const auto before = bps::Logger::Instance().GlobalLevel();
    CHECK(s.Set("notifications.logLevel", J::String("error")).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(bps::Logger::Instance().GlobalLevel() == bps::LogLevel::Error);
    CHECK(s.Set("notifications.logLevel", J::String("debug")).ok() && st::ApplyToEngine(s, rt).ok());
    CHECK(bps::Logger::Instance().GlobalLevel() == bps::LogLevel::Debug);
    bps::Logger::Instance().SetGlobalLevel(before);

    CHECK(rt.Stop().ok());
    CHECK(rt.Shutdown().ok());
    CHECK(rt.Reset().ok());
    (void)Fs().RemoveAll(kRoot);
}

void TestDataProtection() {
    (void)Fs().RemoveAll(kRoot);
    const std::string shows = kRoot + "/shows";
    CHECK(Fs().CreateDirectories(shows).ok());
    const std::string sunday = shows + "/Sunday Service.vgr";
    const std::string youth = shows + "/Youth.vgr";
    CHECK(Fs().Write(sunday, "sunday v1").ok() && Fs().Write(youth, "youth v1").ok());

    st::BackupStore backups(kRoot + "/backups");
    CHECK(backups.List().empty());                                   // no folder yet: nothing, not an error
    CHECK(!backups.Create(shows + "/missing.vgr", "20260101-000000").ok());

    // ---- dated copies, named after the show ----
    auto first = backups.Create(sunday, "20260921-100000");
    CHECK(first.ok() && first.value().find("Sunday Service__20260921-100000.vgr") != std::string::npos);
    CHECK(first.ok() && Fs().ReadText(first.value()).value() == "sunday v1");
    CHECK(Fs().Write(sunday, "sunday v2").ok());
    auto second = backups.Create(sunday, "20260921-103000");
    auto third = backups.Create(sunday, "20260921-110000");
    auto sameSecond = backups.Create(sunday, "20260921-110000");     // never overwrites
    CHECK(second.ok() && third.ok() && sameSecond.ok() && sameSecond.value() != third.value());
    CHECK(Fs().ReadText(third.value()).value() == "sunday v2");
    CHECK(backups.Create(youth, "20260921-090000").ok());
    CHECK(!backups.Create(youth).value().empty());                   // stamped with the time now
    CHECK(!st::NowStamp().empty() && st::NowStamp().size() == 15);

    // ---- listing: newest first, per show ----
    const auto sundays = backups.List("Sunday Service");
    CHECK(sundays.size() == 4);
    CHECK(sundays[0].stamp == "20260921-110000-2" && sundays[1].stamp == "20260921-110000" && sundays[3].stamp == "20260921-100000");
    CHECK(sundays[0].show == "Sunday Service" && sundays[0].sizeBytes == 9);
    CHECK(backups.List("Youth").size() == 2 && backups.List().size() == 6);
    CHECK(backups.List("Nobody").empty());
    // a stray file in the folder is not a backup
    CHECK(Fs().Write(kRoot + "/backups/notes.txt", "hi").ok() && Fs().Write(kRoot + "/backups/odd__name.vgr", "x").ok());
    CHECK(backups.List().size() == 6);

    // ---- CreateIfChanged: a timer can ask as often as it likes ----
    CHECK(Fs().Write(youth, "youth v2").ok());
    auto changed = backups.CreateIfChanged(youth, "20990101-120000");
    CHECK(changed.ok() && !changed.value().empty() && backups.List("Youth").size() == 3);
    auto unchanged = backups.CreateIfChanged(youth, "20990101-123000");
    CHECK(unchanged.ok() && unchanged.value().empty() && backups.List("Youth").size() == 3);   // identical to the newest: no new copy
    CHECK(Fs().Write(youth, "youth v3").ok() && !backups.CreateIfChanged(youth, "20990101-130000").value().empty());
    // Four Youth backups exist now (090000, the now-stamped one, 120000,
    // 130000 — "123000" was checked to NOT copy): keeping 2 removes 2.
    CHECK(backups.Prune("Youth", 2) == 2);   // back to two

    // ---- pruning keeps the newest N of THAT show ----
    CHECK(backups.Prune("Sunday Service", 2) == 2);
    const auto kept = backups.List("Sunday Service");
    CHECK(kept.size() == 2 && kept[0].stamp == "20260921-110000-2" && kept[1].stamp == "20260921-110000");
    CHECK(backups.List("Youth").size() == 2);                        // another show's backups are left alone
    CHECK(backups.Prune("Sunday Service", 10) == 0);
    CHECK(backups.Prune("Sunday Service", 0) == 2 && backups.List("Sunday Service").empty());

    // ---- recovery ----
    st::RecoveryStore recovery(kRoot + "/recovery");
    CHECK(!recovery.Info().has_value());
    CHECK(recovery.Clear().ok());                                    // nothing to clear is fine
    CHECK(Fs().CreateDirectories(kRoot + "/recovery").ok());
    CHECK(Fs().Write(recovery.Path(), "unsaved work").ok());
    CHECK(recovery.Note("Sunday Service", "C:/shows/Sunday Service.vgr").ok());
    auto found = recovery.Info();
    CHECK(found.has_value() && found->path == recovery.Path());
    CHECK(found && found->showName == "Sunday Service" && found->originalPath == "C:/shows/Sunday Service.vgr");
    CHECK(recovery.Clear().ok() && !recovery.Info().has_value());
    // a copy with no note (the app died between the two writes) is still recoverable
    CHECK(Fs().Write(recovery.Path(), "unsaved work").ok());
    CHECK(recovery.Info().has_value() && recovery.Info()->showName == "Untitled show" && recovery.Info()->originalPath.empty());
    CHECK(recovery.Clear().ok() && !recovery.Info().has_value());

    // restoring writes a new file, never over an existing one, and clears what was waiting
    CHECK(!recovery.RestoreTo(shows + "/Recovered.vgr").ok());                     // nothing to recover
    CHECK(Fs().Write(recovery.Path(), "unsaved work").ok() && recovery.Note("Sunday Service", "").ok());
    CHECK(!recovery.RestoreTo(sunday).ok() && recovery.Info().has_value());         // would overwrite: refused, still waiting
    CHECK(recovery.RestoreTo(shows + "/Recovered.vgr").ok());
    CHECK(Fs().ReadText(shows + "/Recovered.vgr").value() == "unsaved work" && !recovery.Info().has_value());
    (void)Fs().RemoveAll(kRoot);
}
