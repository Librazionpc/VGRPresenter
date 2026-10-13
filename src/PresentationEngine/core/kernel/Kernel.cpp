#include "core/kernel/Kernel.hpp"

#include <format>

#include "core/config/ConfigurationManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/lifecycle/LifecycleManager.hpp"
#include "core/logging/Logger.hpp"
#include "core/memory/MemoryManager.hpp"
#include "core/modules/ModuleManager.hpp"
#include "core/plugins/PluginManager.hpp"
#include "core/resources/ResourceManager.hpp"
#include "core/services/ServiceManager.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"
#include "core/assets/AssetManager.hpp"
#include "core/database/DatabaseManager.hpp"
#include "core/display/DisplayManager.hpp"
#include "core/drivers/DriverManager.hpp"
#include "core/ipc/IpcServer.hpp"
#include "core/rendering/RendererManager.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/automation/FlowEngine.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/settings/Telemetry.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "modules/bible/BibleEngine.hpp"
#include "modules/content/ContentManager.hpp"
#include "modules/display/DisplayEngine.hpp"
#include "modules/media/MediaEngine.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/scene/SceneCompositionEngine.hpp"
#include "modules/search/SearchEngine.hpp"
#include "modules/songs/SongEngine.hpp"
#include "modules/vgr/VgrFormat.hpp"
#include "modules/notification/NotificationService.hpp"
#include "modules/project/DataManager.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include "platform/OsTag.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <thread>

namespace bps {

namespace {

// Engine-wide defaults/schema seeded into the ConfigurationManager (03 §3).
json::Value EngineDefaults() {
    json::Value::Object o;
    o["engine.log.level"] = json::Value::String("info");
    o["engine.threads.count"] = json::Value::Number(0);   // 0 = hardware_concurrency
    o["engine.resource.mode"] = json::Value::String("balanced");
    o["engine.eventbus.history"] = json::Value::Number(10000);
    return json::Value(std::move(o));
}

// v4-style UUID: /dev/urandom when available, else a time+address hash. No
// external dependency; uniqueness is per-process (01 §3 Engine UUID).
std::string GenerateUuid() {
    unsigned char b[16] = {0};
    std::ifstream ur("/dev/urandom", std::ios::binary);
    if (ur) {
        ur.read(reinterpret_cast<char*>(b), 16);
        if (ur.gcount() != 16) std::memset(b, 0, sizeof b);
    }
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) {
        auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        uint64_t v = static_cast<uint64_t>(t);
        v ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 1;
        // Extra entropy without any OS API (PAL DoD §1 — no OS calls here).
        v ^= std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (int i = 0; i < 16; ++i)
            b[i] = static_cast<unsigned char>((v >> ((i % 8) * 8)) & 0xFF);
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // variant 10
    return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
                       "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                       b[10], b[11], b[12], b[13], b[14], b[15]);
}

} // namespace

Kernel::Kernel() : uuid_(GenerateUuid()) {}

Kernel& Kernel::Instance() {
    static Kernel instance;
    return instance;
}

Result<void> Kernel::TransitionState(KernelState next) {
    KernelState prev = state_.exchange(next);
    // The bus exists for the whole process; publishing before init is a no-op
    // (zero subscribers), which keeps the boot path safe.
    (void)EventBus::Instance().Publish(events::KernelStateChanged{prev, next});
    return Ok();
}

Result<void> Kernel::Boot(const BootOptions& options) {
    KernelState st = state_.load();
    if (st != KernelState::Stopped && st != KernelState::CrashRecovery)
        return Error::Make(Err::Kernel_InvalidStateTransition, "Kernel",
                           "kernel already booted (state=" + std::string(ToString(st)) + ")");

    options_ = options;
    bootTime_ = EngineClock::now();
    sessionId_ = GenerateUuid();
    Logger::Instance().SetSessionId(sessionId_);   // 02 §Session logging
    auto bootBegin = EngineClock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bootLog_.clear();
        degraded_.clear();   // per-boot state: a previous session's holes are not this one's
    }

    (void)TransitionState(KernelState::Booting);

    // ---- 1. Logger (02) — first system, everything logs through it ----
    auto& logger = Logger::Instance();
    logger.SetGlobalLevel(options.logLevel);
    if (BootStep("Logger", [&] { return logger.Initialize(); })) {
        logger.Info("Kernel v" + kEngineVersion.ToString() + " booting...", "Kernel");
        AppendBootLog("Logger");
    }
    // (Logger::Log falls back to stderr when its pump never started, so a boot
    // that had to degrade the logger is still observable.)

    // ---- 2. ConfigurationManager (03) ----
    auto& config = ConfigurationManager::Instance();
    if (BootStep("ConfigurationManager",
                 [&] { return config.Initialize(EngineDefaults(), options.configPath); })) {
        if (!options.profileDir.empty()) {
            (void)config.SetProfileDir(options.profileDir);
            if (!options.profile.empty()) (void)config.ApplyProfile(options.profile);
        }
        logger.Info("ConfigurationManager ready", "Kernel");
        AppendBootLog("ConfigurationManager");
    }

    // ---- 3. ServiceManager (04) — DI registration of the core systems ----
    auto& services = ServiceManager::Instance();
    (void)services.Register<Logger>(&logger);
    (void)services.Register<ConfigurationManager>(&config);
    AppendBootLog("ServiceManager");

    // ---- 4. EventBus (05) ----
    auto& bus = EventBus::Instance();
    bus.SetHistoryLimit(options.eventHistoryLimit);
    AppendBootLog("EventBus");

    // ---- 5. ThreadPool (06) ----
    auto& pool = ThreadPool::Instance();
    if (BootStep("ThreadPool", [&] { return pool.Initialize(options.threadCount); }))
        AppendBootLog("ThreadPool");

    // ---- 6. TaskScheduler (07) ----
    auto& sched = TaskScheduler::Instance();
    if (BootStep("TaskScheduler", [&] { return sched.Initialize(); }))
        AppendBootLog("TaskScheduler");

    // ---- 7. ResourceManager (10) ----
    auto& res = ResourceManager::Instance();
    if (BootStep("ResourceManager", [&] { return res.Initialize(); })) {
        (void)BootStep("ResourceManager mode", [&] { return res.SetMode(options.resourceMode); });
        AppendBootLog("ResourceManager");
    }

    // ---- 8. MemoryManager (11) ----
    auto& mem = MemoryManager::Instance();
    if (BootStep("MemoryManager", [&] { return mem.Initialize(); }))
        AppendBootLog("MemoryManager");

    // ---- 9. PluginManager (09) ----
    auto& plugins = PluginManager::Instance();
    for (const auto& dir : options.pluginDirs) {
        if (auto r = plugins.Discover(dir); !r.ok())
            logger.Warning("Plugin discovery failed for " + dir + ": " + r.error().message, "Kernel");
    }
    AppendBootLog("PluginManager");

    // ---- 10. ModuleManager (08) ----
    auto& modules = ModuleManager::Instance();
    (void)modules;
    AppendBootLog("ModuleManager");

    // ---- 11. LifecycleManager (12) ----
    auto& lc = LifecycleManager::Instance();
    (void)lc.RegisterEntity("core:kernel", kEngineVersion);
    (void)lc.RegisterEntity("core:logger", kEngineVersion);
    (void)lc.RegisterEntity("core:config", kEngineVersion);
    (void)lc.RegisterEntity("core:eventbus", kEngineVersion);
    (void)lc.RegisterEntity("core:threadpool", kEngineVersion);
    (void)lc.RegisterEntity("core:scheduler", kEngineVersion);
    AppendBootLog("LifecycleManager");

    // ---- 12. Platform (PAL) + engine managers (Assets/Drivers/Database/Display/Renderer) ----
    {
        auto platform = std::shared_ptr<platform::IPlatform>(platform::CreatePlatform());
        if (platform) {
            // Global install: every engine system routes OS access through the
            // PAL (PluginManager, ModuleManager, DatabaseManager, ThreadPool...).
            platform::PlatformAccessor::Install(platform);
            ResourceManager::Instance().SetPlatform(platform);
            auto info = platform->Info();
            logger.Info(std::format("Platform: {} {} / {} / {} cores / {} MiB RAM / host={}",
                                    info.osName, info.osVersion, info.arch, info.coreCount,
                                    info.totalRamBytes / (1024ull * 1024ull), info.hostname),
                        "Kernel");

            // OS event watcher (Phase 2 §Operating System Events): drain
            // PollChanges() into the Event Bus on a 1s scheduler tick.
            auto watcher = [platform, &bus, &logger]() {
                for (const auto& ev : platform->PollChanges()) {
                    switch (ev.type) {
                        case platform::OsEventType::MonitorConnected:
                            (void)bus.Publish(events::MonitorConnected{ev.detail});
                            break;
                        case platform::OsEventType::MonitorDisconnected:
                            (void)bus.Publish(events::MonitorDisconnected{ev.detail});
                            break;
                        case platform::OsEventType::ResolutionChanged:
                            (void)bus.Publish(events::MonitorResolutionChanged{ev.detail});
                            break;
                        case platform::OsEventType::PowerChanged:
                            (void)bus.Publish(events::PowerChanged{ev.detail});
                            break;
                        case platform::OsEventType::BatteryLow:
                            (void)bus.Publish(events::BatteryLow{ev.detail});
                            break;
                        case platform::OsEventType::Sleep:
                            (void)bus.Publish(events::SystemSleep{ev.detail});
                            break;
                        case platform::OsEventType::Wake:
                            (void)bus.Publish(events::SystemWake{ev.detail});
                            break;
                        case platform::OsEventType::NetworkChanged:
                            (void)bus.Publish(events::NetworkChanged{ev.detail});
                            break;
                        case platform::OsEventType::DeviceConnected:
                            (void)bus.Publish(events::DeviceConnected{ev.detail});
                            break;
                        case platform::OsEventType::DeviceRemoved:
                            (void)bus.Publish(events::DeviceRemoved{ev.detail});
                            break;
                        case platform::OsEventType::LocaleChanged:
                            (void)bus.Publish(events::LocaleChanged{ev.detail});
                            break;
                        case platform::OsEventType::ClipboardChanged:
                            (void)bus.Publish(events::ClipboardChanged{ev.detail});
                            break;
                    }
                }
            };
            if (auto r = sched.ScheduleEvery(std::move(watcher), std::chrono::seconds(1));
                !r.ok())
                logger.Warning("Platform event watcher not scheduled: " + r.error().message,
                               "Kernel");
        } else {
            logger.Warning("Platform: no backend for this OS — telemetry limited", "Kernel");
        }
    }
    AppendBootLog("Platform");

    auto& assets = AssetManager::Instance();
    if (BootStep("AssetManager", [&] { return assets.Initialize(); }))
        (void)services.Register<AssetManager>(&assets);

    auto& drivers = DriverManager::Instance();
    if (BootStep("DriverManager", [&] { return drivers.Initialize(); }))
        (void)services.Register<DriverManager>(&drivers);

    auto& db = DatabaseManager::Instance();
    // Open() takes a FILE path; dataDir is a directory. Name the database
    // inside it (an empty dataDir stays in-memory). Passing the bare dir
    // used to collide with the UI's own "enginedata" directory of the same
    // path, making every persist fail silently.
    //
    // THIS is the step whose failure started the whole story: an unreadable
    // kernel.json answered Config_ParseFailed, Boot aborted, the UI never saw a
    // Running kernel, and every write for the rest of the session answered "the
    // engine is not running yet". Open() quarantines a damaged file now, and a
    // failure here degrades a session that is missing its persistence — loudly,
    // in the log and in EngineContext — instead of stopping the engine.
    const std::string dbFile =
        options.dataDir.empty() ? std::string{} : options.dataDir + "/kernel.json";
    if (BootStep("Database", [&] { return db.Open(dbFile); }))
        (void)services.Register<DatabaseManager>(&db);

    // ---- 13. Content & Asset Management (Phase 3, docs/specs/13) ----
    // CAMS boots after the Database (metadata persistence) and the PAL
    // (all file I/O through the VFS). It is the single public API for content.
    auto& content = content::ContentManager::Instance();
    if (BootStep("Content", [&] { return content.Initialize(); }))
        (void)services.Register<content::ContentManager>(&content);
    // Scan any configured library root for unindexed files, then start the
    // watcher heartbeat.
    {
        auto roots = config.GetString("cams.roots", "");
        if (!roots.empty()) {
            if (auto r = content.MountDisk("lib", roots); !r.ok())
                Degrade("ContentManager mount", r.error());
        }
        if (auto s = content.Start(); !s.ok())
            Degrade("ContentManager start", s.error());
    }
    AppendBootLog("Content");

    auto& displays = DisplayManager::Instance();
    if (BootStep("DisplayManager", [&] { return displays.Initialize(); }))
        (void)services.Register<DisplayManager>(&displays);

    auto& renderer = RendererManager::Instance();
    if (BootStep("RendererManager", [&] { return renderer.Initialize(); }))
        (void)services.Register<RendererManager>(&renderer);

    AppendBootLog("Assets");
    AppendBootLog("Drivers");
    AppendBootLog("Database");
    AppendBootLog("Display");
    AppendBootLog("Renderer");

    // ---- 14. NotificationService (Phase 4, docs/specs/14) ----
    // The ONLY system that decides if/when/where/how the user is informed.
    // Subscribes to engine events (content, resource, platform, project) and
    // routes through providers; never touches the UI. Boots after CAMS (it
    // consumes content events) and before DataManager (whose project events it
    // also consumes).
    {
        auto& notify = notification::NotificationService::Instance();
        if (auto r = notify.Initialize(); !r.ok()) {
            Degrade("NotificationService", r.error());
        } else {
            (void)services.Register<notification::NotificationService>(&notify);
            if (auto s = notify.Start(); !s.ok())
                Degrade("NotificationService start", s.error());
            AppendBootLog("Notifications");
        }
    }

    // ---- 15. DataManager — Project & Data System (Phase 4, docs/specs/15) ----
    // Projects, documents, undo/redo, recovery, backups, packages, templates,
    // profiles. Depends on CAMS (assets) and the DatabaseManager (persistence).
    {
        auto& data = project::DataManager::Instance();
        if (auto r = data.Initialize(); !r.ok()) {
            Degrade("DataManager", r.error());
        } else {
            (void)services.Register<project::DataManager>(&data);
            if (auto s = data.Start(); !s.ok())
                Degrade("DataManager start", s.error());
            AppendBootLog("Projects");
        }
    }

    // ---- 16. AdaptiveRuntime (Phase 5, docs/specs/16) ----
    // The self-optimizing brain: hardware/capability detection, feature
    // registry, quality modes, resource budgets, pressure reflexes, learning.
    // Boots last (it watches everything above it) and shuts down first.
    {
        auto& runtime = adaptive::AdaptiveRuntime::Instance();
        if (auto r = runtime.Initialize(); !r.ok()) {
            Degrade("AdaptiveRuntime", r.error());
        } else {
            (void)services.Register<adaptive::AdaptiveRuntime>(&runtime);
            if (auto s = runtime.Start(); !s.ok())
                Degrade("AdaptiveRuntime start", s.error());
            AppendBootLog("Adaptive");
        }
    }

    // ---- 16b. Telemetry (live utilization feed for the Settings meters) ----
    // Subscribes to the render frame events; boots before the RenderEngine (17)
    // so the very first frame is counted. Stops with the engine (it holds only
    // event subscriptions — nothing to stop out of order).
    {
        auto& tel = settings::Telemetry::Instance();
        if (auto r = tel.Initialize(); !r.ok())
            Degrade("Telemetry", r.error());
        else
            (void)services.Register<settings::Telemetry>(&tel);
    }

    // ---- 17. RenderEngine (Phase 6, docs/specs/17) ----
    // The generic rendering engine: scenes → layers → objects → pixels.
    // Backend-independent (IGraphicsBackend) and frontend-agnostic; renders
    // frames and distributes them to Render Outputs. Boots last — it asks the
    // AdaptiveRuntime (16) for quality/budget guidance — and stops first.
    {
        auto& render = rendering::RenderEngine::Instance();
        if (auto r = render.Initialize(); !r.ok()) {
            Degrade("RenderEngine", r.error());
        } else {
            (void)services.Register<rendering::RenderEngine>(&render);
            if (auto s = render.Start(); !s.ok())
                Degrade("RenderEngine start", s.error());
            AppendBootLog("RenderEngine");
        }
    }

    // ---- 18. DisplayEngine (Phase 7, docs/specs/18) ----
    // Routes one rendered frame to any number of outputs through a provider
    // architecture; hot-plug detection, profiles, recovery. Boots after the
    // RenderEngine (17) whose frames it routes, and stops before it.
    {
        auto& disp = display::DisplayEngine::Instance();
        if (auto r = disp.Initialize(); !r.ok()) {
            Degrade("DisplayEngine", r.error());
        } else {
            (void)services.Register<display::DisplayEngine>(&disp);
            if (auto s = disp.Start(); !s.ok())
                Degrade("DisplayEngine start", s.error());
            AppendBootLog("DisplayEngine");
        }
    }

    // ---- 19. PresentationEngine (Phase 8, docs/specs/19) ----
    // The conductor: orchestrates shows, slides, timelines, cues, playback.
    // Never renders pixels or routes outputs itself. Boots last (it consumes
    // events from every system above) and stops first.
    {
        auto& pres = presentation::PresentationEngine::Instance();
        if (auto r = pres.Initialize(); !r.ok()) {
            Degrade("PresentationEngine", r.error());
        } else {
            (void)services.Register<presentation::PresentationEngine>(&pres);
            if (auto s = pres.Start(); !s.ok())
                Degrade("PresentationEngine start", s.error());
            AppendBootLog("PresentationEngine");
        }
    }

    // ---- 19b. LiveOutputController (Phase 8 live loop) ----
    // Drives the real pipeline continuously while a show is on air: renders
    // the PresentationEngine's current slide through the RenderEngine (17) at
    // the 60Hz target with distribution to every enabled output. Boots after
    // the PresentationEngine (19) it drives; stops first (its worker must not
    // outlive the engines it renders through).
    {
        auto& live = live::LiveOutputController::Instance();
        if (auto r = live.Initialize(); !r.ok()) {
            Degrade("LiveOutputController", r.error());
        } else {
            (void)services.Register<live::LiveOutputController>(&live);
            AppendBootLog("LiveOutputController");
        }
    }

    // ---- 20. SearchEngine (Phase 9, docs/specs/20) ----
    // The central knowledge layer: every module asks it "find me the
    // information". Boots after CAMS (13) whose assets it auto-indexes, and
    // before Media (21) which registers documents with it.
    {
        auto& search = search::SearchEngine::Instance();
        if (auto r = search.Initialize(); !r.ok()) {
            Degrade("SearchEngine", r.error());
        } else {
            (void)services.Register<search::SearchEngine>(&search);
            if (auto s = search.Start(); !s.ok())
                Degrade("SearchEngine start", s.error());
            AppendBootLog("SearchEngine");
        }
    }

    // ---- 21. MediaEngine (Phase 10, docs/specs/21) ----
    // The single system for managing every type of media. Depends on the
    // Search Engine (20) for auto-indexing and the Adaptive Runtime (16) for
    // budgets; boots after both.
    {
        auto& media = media::MediaEngine::Instance();
        if (auto r = media.Initialize(); !r.ok()) {
            Degrade("MediaEngine", r.error());
        } else {
            (void)services.Register<media::MediaEngine>(&media);
            if (auto s = media.Start(); !s.ok())
                Degrade("MediaEngine start", s.error());
            AppendBootLog("MediaEngine");
        }
    }

    // ---- 22. VGR native format (docs/specs/22) ----
    // The authoritative project format. Stateless serializer — registered as a
    // service so modules resolve it uniformly.
    {
        AppendBootLog("VgrFormat");
    }

    // ---- 23. SceneCompositionEngine (Phase 11, docs/specs/23) ----
    // Everything is a Scene: templates, themes, layouts, widget rules. Consumes
    // presentation events and produces composed scenes for the Render Engine.
    // Boots last (it ties Content + Presentation + Rendering together) and
    // stops first.
    {
        auto& sce = scene::SceneCompositionEngine::Instance();
        if (auto r = sce.Initialize(); !r.ok()) {
            Degrade("SceneCompositionEngine", r.error());
        } else {
            (void)services.Register<scene::SceneCompositionEngine>(&sce);
            if (auto s = sce.Start(); !s.ok())
                Degrade("SceneCompositionEngine start", s.error());
            AppendBootLog("SceneEngine");
        }
    }

    // ---- 24. BibleEngine (Phase 12, docs/specs/24) ----
    // The first-class Scripture knowledge system: imports, validates, indexes,
    // resolves references, queries, formats, and compares Bibles — completely
    // independent of the UI. Boots after the Search Engine (20) which it uses
    // for verse indexing. Imported bibles persist as a JSON store inside the
    // data dir: SetStorePath BEFORE Initialize() so boot restores them.
    {
        auto& bible = bible::BibleEngine::Instance();
        if (!options.dataDir.empty())
            bible.SetStorePath(options.dataDir + "/bibles.json");
        if (auto r = bible.Initialize(); !r.ok()) {
            Degrade("BibleEngine", r.error());
        } else {
            (void)services.Register<bible::BibleEngine>(&bible);
            if (auto s = bible.Start(); !s.ok())
                Degrade("BibleEngine start", s.error());
            AppendBootLog("BibleEngine");
        }
    }

    // ---- 25. SongEngine (Phase 13, docs/specs/25) ----
    // Structured musical content: imports, validates, transposes, arranges,
    // detects duplicates, versions, and indexes songs. Never renders lyrics —
    // the Scene Engine composes them. Boots after Search (20) and Media (21).
    {
        auto& songs = song::SongEngine::Instance();
        if (auto r = songs.Initialize(); !r.ok()) {
            Degrade("SongEngine", r.error());
        } else {
            (void)services.Register<song::SongEngine>(&songs);
            if (auto s = songs.Start(); !s.ok())
                Degrade("SongEngine start", s.error());
            AppendBootLog("SongEngine");
        }
    }

    // ---- 26. FlowEngine (Phase 14, docs/specs/26) ----
    // The orchestration layer: loads, validates, and executes Service Flows
    // (playlists/automation) through registered actions, conditions, and
    // triggers. It coordinates the other engines via actions only — it never
    // renders or plays content itself. Boots after Scene (23), Bible (24), and
    // Song (25) so its actions can target them.
    {
        auto& flow = automation::FlowEngine::Instance();
        if (auto r = flow.Initialize(); !r.ok()) {
            Degrade("FlowEngine", r.error());
        } else {
            (void)services.Register<automation::FlowEngine>(&flow);
            if (auto s = flow.Start(); !s.ok())
                Degrade("FlowEngine start", s.error());
            AppendBootLog("FlowEngine");
        }
    }

    // ---- 27. ProductionEngine (Phase 15, docs/specs/27) ----
    // The production backbone: a universal signal graph (sources → processing
    // → virtual sources → buses → outputs) for audio and video independently,
    // with routing, mixing, planning, meters, clocks, and health. It routes
    // and coordinates — rendering/media/display engines own pixels/audio/screens.
    {
        auto& prod = production::ProductionEngine::Instance();
        if (auto r = prod.Initialize(); !r.ok()) {
            Degrade("ProductionEngine", r.error());
        } else {
            (void)services.Register<production::ProductionEngine>(&prod);
            if (auto s = prod.Start(); !s.ok())
                Degrade("ProductionEngine start", s.error());
            AppendBootLog("ProductionEngine");
        }
    }

    // ---- 28. RecordingEngine (Phase 16, docs/specs/28) ----
    // Recording, replay & capture: recording is a first-class output consumer
    // of the Phase 15 production graph. Any valid signal (source, bus, virtual
    // source, scene, output) can be recorded with its own profile, tap point,
    // format and lifecycle. Owns profiles, segmentation, markers, metadata,
    // crash recovery (journal), replay buffers, capture devices, encoder
    // selection and disk-aware storage. It never renders — it consumes the graph.
    {
        auto& rec = recording::RecordingEngine::Instance();
        if (auto r = rec.Initialize(); !r.ok()) {
            Degrade("RecordingEngine", r.error());
        } else {
            (void)services.Register<recording::RecordingEngine>(&rec);
            if (auto s = rec.Start(); !s.ok())
                Degrade("RecordingEngine start", s.error());
            AppendBootLog("RecordingEngine");
        }
    }

    // ---- 29. BroadcastEngine (Phase 17, docs/specs/29) ----
    // NDI + SDI via runtime-loaded SDKs (libndi / DeckLink). Providers degrade
    // to Unsupported when the SDK or hardware is absent, so boot never fails
    // on a machine without broadcast hardware. The software loopback provider
    // is always available for rehearsal and CI.
    {
        auto& bc = broadcast::BroadcastEngine::Instance();
        if (auto r = bc.Initialize(); !r.ok()) {
            Degrade("BroadcastEngine", r.error());
        } else {
            (void)services.Register<broadcast::BroadcastEngine>(&bc);
            if (auto s = bc.Start(); !s.ok())
                Degrade("BroadcastEngine start", s.error());
            AppendBootLog("BroadcastEngine");
        }
    }

    // ---- 13. IPC server + remote log streaming (optional, SystemArchitecture §2) ----
    if (options.ipcPort > 0) {
        auto& ipc = IpcServer::Instance();
        if (auto r = ipc.Start(options.ipcPort); !r.ok()) {
            Degrade("IPC server", r.error());
        } else {
            auto reg = [&](std::string method, IpcServer::Handler h) {
                if (auto r = ipc.RegisterHandler(std::move(method), std::move(h)); !r.ok())
                    logger.Warning("IPC handler " + method + " not registered: " +
                                       r.error().message, "Kernel");
            };
            reg("engine.ping", [](const json::Value& params) { return params; });
            reg("engine.health", [](const json::Value&) {
                json::Value::Object o;
                HealthReport h = Kernel::Instance().GetHealth();
                o["state"] = json::Value::String(ToString(h.state));
                o["detail"] = json::Value::String(h.detail);
                o["uptimeMs"] = json::Value::Number(
                    static_cast<double>(Kernel::Instance().Uptime().count()) / 1000.0);
                return json::Value(std::move(o));
            });
            // `engine.shutdown` must not run on the connection thread (Stop()
            // joins connection threads) nor on a pool worker (Shutdown tears
            // down the ThreadPool, and a worker joining itself terminates).
            // Contract: Shutdown() only returns after full teardown (state
            // flips to Stopped at the end), so a caller that waits for
            // State() != Running is safe to exit afterwards.
            reg("engine.shutdown", [](const json::Value&) {
                std::thread([]() { (void)Kernel::Instance().Shutdown(); }).detach();
                json::Value::Object o;
                o["accepted"] = json::Value::Number(1);
                return json::Value(std::move(o));
            });
            // Remote log streaming (02 §3): every record -> Broadcast to
            // `log.subscribe` clients as a JSON line.
            auto remoteSink = std::make_shared<RemoteSink>();
            remoteSink->SetPublisher([](const std::string& line) {
                (void)IpcServer::Instance().Broadcast(line);
            });
            if (auto r = logger.AddSink(remoteSink); !r.ok())
                logger.Warning("Remote log sink not installed: " + r.error().message, "Kernel");
            logger.Info(std::format("IPC server listening on 127.0.0.1:{} (remote log "
                                    "streaming on)",
                                    ipc.Port()),
                        "Kernel");
            AppendBootLog("Ipc");
        }
    }

    (void)TransitionState(KernelState::Running);

    auto bootTime = std::chrono::duration_cast<std::chrono::microseconds>(EngineClock::now() - bootBegin);
    (void)bus.Publish(events::EngineBooted{bootTime});
    const auto degraded = Degraded();
    logger.Info(std::format("Kernel ready in {} ms — state=Running{}",
                             static_cast<double>(bootTime.count()) / 1000.0,
                             degraded.empty()
                                 ? std::string()
                                 : std::format(" with {} degraded subsystem(s)", degraded.size())),
                "Kernel");
    // One summary line so a degraded boot is obvious without reading every
    // step, and reachable from EngineContext.degraded for the UI to surface.
    for (const auto& d : degraded)
        logger.Warning(std::format("degraded: {} [{}] {}", d.system, d.code, d.message), "Kernel");
    return Ok();
}

bool Kernel::BootStep(std::string_view system, const std::function<Result<void>()>& step) {
    Result<void> r = Ok();
    try {
        r = step();
    } catch (const std::exception& e) {
        Degrade(system, Error::Make(Err::Kernel_BootFailed, std::string(system),
                                    std::string("threw: ") + e.what()));
        return false;
    } catch (...) {
        Degrade(system, Error::Make(Err::Kernel_BootFailed, std::string(system),
                                    "threw a non-std exception"));
        return false;
    }
    if (!r.ok()) {
        Degrade(system, r.error());
        return false;
    }
    return true;
}

void Kernel::Degrade(std::string_view system, const Error& cause) {
    DegradedSubsystem d;
    d.system = std::string(system);
    d.code = cause.code;
    d.message = cause.message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        degraded_.push_back(d);
        lastError_ = d.system + ": " + d.message;
    }
    // Warning, not Fatal: the engine comes up without this system. The message
    // names the hole so a log reader can tell a degraded boot from a clean one,
    // and the operator's own data is what the UI should point at.
    Logger::Instance().Warning(
        std::format("{} unavailable this session: [{}] {} (engine continues)", d.system,
                    d.code, d.message),
        "Kernel");
    (void)EventBus::Instance().Publish(events::SubsystemDegraded(d.system, d.code, d.message));
}

std::vector<DegradedSubsystem> Kernel::Degraded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return degraded_;
}

std::vector<std::string> Kernel::BootLog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bootLog_;
}

void Kernel::AppendBootLog(std::string_view system) {
    std::lock_guard<std::mutex> lock(mutex_);
    bootLog_.push_back(std::string(system));
}

Result<void> Kernel::ShutdownSystems() {
    auto& logger = Logger::Instance();
    logger.Info(std::format("Kernel shutdown: tearing down {} systems", BootLog().size()),
                "Kernel");

    // Modules (08) and plugins (09) first, in dependency-reverse order.
    auto& modules = ModuleManager::Instance();
    for (const auto& info : modules.Snapshot()) {
        if (info.state == ModuleState::Running || info.state == ModuleState::Paused ||
            info.state == ModuleState::Suspended)
            (void)modules.Stop(info.id);
    }
    auto& plugins = PluginManager::Instance();
    for (const auto& info : plugins.Snapshot())
        if (info.state != 0) (void)plugins.Unload(info.id);

    // Engine managers (boot steps 12-23, reverse order). The SceneComposition
    // Engine (23) stops first, then Media (21) whose workers must not run, then
    // Search (20) whose event subscriptions must be torn down, then the
    // PresentationEngine (19) whose live tick must not run during teardown,
    // then the DisplayEngine (18) whose output routing must stop, then the
    // RenderEngine (17) whose render thread must not run during teardown, then
    // the AdaptiveRuntime (16) whose heartbeat must not run during teardown.
    // The Project & Data system and the Notification service shut down before
    // CAMS (their events and metadata consumers must stop first); CAMS before
    // the Database (persisted metadata must flush first).
    // The PRODUCTION backbone (27) stops before everything it feeds, and
    // critically before DatabaseManager::Shutdown() at the bottom of this
    // method — ProductionEngine::Shutdown() takes the final graph save, which
    // is only durable if the database flushes AFTER it. It never used to be
    // called at all (its own comment claimed the kernel tears production down
    // before the database, and this list simply did not include it), so the
    // last edits of a session lived in the in-memory store and were written
    // only because DatabaseManager::Shutdown happens to flush everything —
    // with nothing guaranteeing production got its say first. See
    // ProductionEngine::RequestPersist for the in-session counterpart.
    (void)production::ProductionEngine::Instance().Shutdown();
    // Phase 14 (26) stops next: it was booted after the rest of the engines, so
    // it tears down before them.
    (void)automation::FlowEngine::Instance().Shutdown();
    // Phase 13 (25) and Phase 12 (24) stop before Scene (23): they were booted
    // after it, so they tear down in reverse order.
    (void)song::SongEngine::Instance().Shutdown();
    (void)bible::BibleEngine::Instance().Shutdown();
    (void)scene::SceneCompositionEngine::Instance().Shutdown();
    (void)media::MediaEngine::Instance().Shutdown();
    (void)search::SearchEngine::Instance().Shutdown();
    // The live render loop (19b) stops before the engines it renders through.
    (void)live::LiveOutputController::Instance().Shutdown();
    (void)presentation::PresentationEngine::Instance().Shutdown();
    (void)display::DisplayEngine::Instance().Shutdown();
    (void)rendering::RenderEngine::Instance().Shutdown();
    // Telemetry (16b) unsubscribes after the RenderEngine (17) stopped — no
    // more frames can arrive mid-unsubscribe.
    (void)settings::Telemetry::Instance().Shutdown();
    (void)adaptive::AdaptiveRuntime::Instance().Shutdown();
    (void)project::DataManager::Instance().Shutdown();
    (void)notification::NotificationService::Instance().Shutdown();
    (void)RendererManager::Instance().Shutdown();
    (void)DisplayManager::Instance().Shutdown();
    (void)content::ContentManager::Instance().Shutdown();
    (void)DatabaseManager::Instance().Shutdown();
    (void)DriverManager::Instance().Shutdown();
    (void)AssetManager::Instance().Shutdown();

    // Stop remote log streaming first (no broadcasts after the server is down),
    // then the IPC server itself (joins accept + connection threads).
    (void)Logger::Instance().RemoveSink("Remote");
    (void)IpcServer::Instance().Shutdown();

    // MemoryManager: verify no leaks attributed to core subsystems (11 §10).
    auto& mem = MemoryManager::Instance();
    if (auto leaks = mem.CheckLeaks(); !leaks.ok())
        logger.Warning("MemoryManager: " + leaks.error().message, "Kernel");

    // Resource -> Scheduler -> Threads -> Bus -> Services -> Config -> Logs.
    (void)ResourceManager::Instance().Shutdown();
    (void)TaskScheduler::Instance().Shutdown();
    (void)ThreadPool::Instance().Shutdown();
    (void)ServiceManager::Instance().DestroyAll();
    logger.Info("Kernel shutdown complete", "Kernel");
    (void)Logger::Instance().Shutdown();
    return Ok();
}

Result<void> Kernel::Shutdown() {
    KernelState st = state_.load();
    if (st != KernelState::Running && st != KernelState::Paused)
        return Error::Make(Err::Kernel_InvalidStateTransition, "Kernel", "kernel not running");
    (void)TransitionState(KernelState::ShuttingDown);
    (void)EventBus::Instance().Publish(events::ShutdownStarted{});
    auto r = ShutdownSystems();
    state_.store(KernelState::Stopped);
    return r;
}

Result<void> Kernel::Pause() {
    if (state_.load() != KernelState::Running)
        return Error::Make(Err::Kernel_InvalidStateTransition, "Kernel", "kernel not running");
    return TransitionState(KernelState::Paused);
}

Result<void> Kernel::Resume() {
    if (state_.load() != KernelState::Paused)
        return Error::Make(Err::Kernel_InvalidStateTransition, "Kernel", "kernel not paused");
    return TransitionState(KernelState::Running);
}

Result<void> Kernel::Panic(const Error& reason) {
    auto& logger = Logger::Instance();
    // Crash logging: fatal record + synchronous flush so it survives the teardown.
    logger.CrashLog("Kernel", std::format("KERNEL PANIC: [{}] {}", reason.code,
                                           reason.message));
    (void)EventBus::Instance().Publish(events::KernelPanic{reason});   // best effort
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = reason.message;
    }
    (void)ShutdownSystems();
    state_.store(KernelState::CrashRecovery);   // recovery mode: Recover() may re-boot
    return Ok();
}

Result<void> Kernel::Recover(const BootOptions& options) {
    if (state_.load() != KernelState::CrashRecovery)
        return Error::Make(Err::Kernel_InvalidStateTransition, "Kernel",
                           "no crash to recover from (state=" +
                               std::string(ToString(state_.load())) + ")");
    Logger::Instance().Info("Kernel: crash recovery — re-booting", "Kernel");
    return Boot(options);
}

std::chrono::microseconds Kernel::Uptime() const {
    if (state_.load() == KernelState::Stopped) return std::chrono::microseconds(0);
    return std::chrono::duration_cast<std::chrono::microseconds>(EngineClock::now() - bootTime_);
}

BuildInfo Kernel::GetBuildInfo() const {
    BuildInfo b;
    b.version = kEngineVersion;
#ifdef NDEBUG
    b.buildType = "Release";
#else
    b.buildType = "Debug";
#endif
#if defined(__clang__)
    b.compiler = "Clang " + std::string(__clang_version__);
#elif defined(__GNUC__)
    b.compiler = "GCC " + std::string(__VERSION__);
#else
    b.compiler = "Unknown";
#endif
    b.buildDate = std::string(__DATE__) + " " + __TIME__;
    // Compile-time tags come from the PAL (platform/OsTag.hpp) — the core never
    // branches on OS macros itself (PAL DoD §1).
    b.platform = platform::kCompileOs;
    b.arch = platform::kCompileArch;
    return b;
}

RuntimeInfo Kernel::GetRuntimeInfo() const {
    RuntimeInfo r;
    r.engineUuid = uuid_;
    r.sessionId = sessionId_;
    r.startedAt = bootTime_;
    r.bootedAt = bootTime_;
    r.state = state_.load();
    return r;
}

EngineContext Kernel::Context() const {
    EngineContext c;
    c.build = GetBuildInfo();
    c.runtime = GetRuntimeInfo();
    c.options = options_;
    c.bootLog = BootLog();
    c.degraded = Degraded();
    c.health = GetHealth();
    return c;
}

HealthReport Kernel::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool running = state_.load() == KernelState::Running;
    // A running kernel with a hole in it is Degraded, not Healthy: the state
    // string used to be the only signal, and "Running" on a session that lost
    // its database is exactly the kind of half-truth this reports against.
    r.state = (running && degraded_.empty()) ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("state={} uptime={}ms systems={} degraded={}", ToString(state_.load()),
                           Uptime().count() / 1000, bootLog_.size(), degraded_.size());
    r.lastError = lastError_;
    return r;
}

} // namespace bps
