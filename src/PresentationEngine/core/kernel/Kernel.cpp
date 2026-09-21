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
    }

    (void)TransitionState(KernelState::Booting);

    // ---- 1. Logger (02) — first system, everything logs through it ----
    auto& logger = Logger::Instance();
    logger.SetGlobalLevel(options.logLevel);
    if (auto r = logger.Initialize(); !r.ok()) {
        state_.store(KernelState::Stopped);
        return r;
    }
    logger.Info("Kernel v" + kEngineVersion.ToString() + " booting...", "Kernel");
    AppendBootLog("Logger");

    // ---- 2. ConfigurationManager (03) ----
    auto& config = ConfigurationManager::Instance();
    if (auto r = config.Initialize(EngineDefaults(), options.configPath); !r.ok())
        return FailBoot(r.error());
    if (!options.profileDir.empty()) {
        (void)config.SetProfileDir(options.profileDir);
        if (!options.profile.empty()) (void)config.ApplyProfile(options.profile);
    }
    logger.Info("ConfigurationManager ready", "Kernel");
    AppendBootLog("ConfigurationManager");

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
    if (auto r = pool.Initialize(options.threadCount); !r.ok()) return FailBoot(r.error());
    AppendBootLog("ThreadPool");

    // ---- 6. TaskScheduler (07) ----
    auto& sched = TaskScheduler::Instance();
    if (auto r = sched.Initialize(); !r.ok()) return FailBoot(r.error());
    AppendBootLog("TaskScheduler");

    // ---- 7. ResourceManager (10) ----
    auto& res = ResourceManager::Instance();
    if (auto r = res.Initialize(); !r.ok()) return FailBoot(r.error());
    if (auto r = res.SetMode(options.resourceMode); !r.ok()) return FailBoot(r.error());
    AppendBootLog("ResourceManager");

    // ---- 8. MemoryManager (11) ----
    auto& mem = MemoryManager::Instance();
    if (auto r = mem.Initialize(); !r.ok()) return FailBoot(r.error());
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
    if (auto r = assets.Initialize(); !r.ok()) return FailBoot(r.error());
    (void)services.Register<AssetManager>(&assets);

    auto& drivers = DriverManager::Instance();
    if (auto r = drivers.Initialize(); !r.ok()) return FailBoot(r.error());
    (void)services.Register<DriverManager>(&drivers);

    auto& db = DatabaseManager::Instance();
    // Open() takes a FILE path; dataDir is a directory. Name the database
    // inside it (an empty dataDir stays in-memory). Passing the bare dir
    // used to collide with the UI's own "enginedata" directory of the same
    // path, making every persist fail silently.
    const std::string dbFile =
        options.dataDir.empty() ? std::string{} : options.dataDir + "/kernel.json";
    if (auto r = db.Open(dbFile); !r.ok()) return FailBoot(r.error());
    (void)services.Register<DatabaseManager>(&db);

    // ---- 13. Content & Asset Management (Phase 3, docs/specs/13) ----
    // CAMS boots after the Database (metadata persistence) and the PAL
    // (all file I/O through the VFS). It is the single public API for content.
    auto& content = content::ContentManager::Instance();
    if (auto r = content.Initialize(); !r.ok()) return FailBoot(r.error());
    (void)services.Register<content::ContentManager>(&content);
    // Scan any configured library root for unindexed files, then start the
    // watcher heartbeat.
    {
        auto roots = config.GetString("cams.roots", "");
        if (!roots.empty()) {
            if (auto r = content.MountDisk("lib", roots); !r.ok())
                logger.Warning("ContentManager mount '" + roots + "': " + r.error().message,
                               "Kernel");
        }
        if (auto s = content.Start(); !s.ok())
            logger.Warning("ContentManager start: " + s.error().message, "Kernel");
    }
    AppendBootLog("Content");

    auto& displays = DisplayManager::Instance();
    if (auto r = displays.Initialize(); !r.ok()) return FailBoot(r.error());
    (void)services.Register<DisplayManager>(&displays);

    auto& renderer = RendererManager::Instance();
    if (auto r = renderer.Initialize(); !r.ok()) return FailBoot(r.error());
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
            logger.Warning("NotificationService init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<notification::NotificationService>(&notify);
            if (auto s = notify.Start(); !s.ok())
                logger.Warning("NotificationService start: " + s.error().message, "Kernel");
            AppendBootLog("Notifications");
        }
    }

    // ---- 15. DataManager — Project & Data System (Phase 4, docs/specs/15) ----
    // Projects, documents, undo/redo, recovery, backups, packages, templates,
    // profiles. Depends on CAMS (assets) and the DatabaseManager (persistence).
    {
        auto& data = project::DataManager::Instance();
        if (auto r = data.Initialize(); !r.ok()) {
            logger.Warning("DataManager init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<project::DataManager>(&data);
            if (auto s = data.Start(); !s.ok())
                logger.Warning("DataManager start: " + s.error().message, "Kernel");
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
            logger.Warning("AdaptiveRuntime init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<adaptive::AdaptiveRuntime>(&runtime);
            if (auto s = runtime.Start(); !s.ok())
                logger.Warning("AdaptiveRuntime start: " + s.error().message, "Kernel");
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
            logger.Warning("Telemetry init: " + r.error().message, "Kernel");
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
            logger.Warning("RenderEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<rendering::RenderEngine>(&render);
            if (auto s = render.Start(); !s.ok())
                logger.Warning("RenderEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("DisplayEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<display::DisplayEngine>(&disp);
            if (auto s = disp.Start(); !s.ok())
                logger.Warning("DisplayEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("PresentationEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<presentation::PresentationEngine>(&pres);
            if (auto s = pres.Start(); !s.ok())
                logger.Warning("PresentationEngine start: " + s.error().message, "Kernel");
            AppendBootLog("PresentationEngine");
        }
    }

    // ---- 20. SearchEngine (Phase 9, docs/specs/20) ----
    // The central knowledge layer: every module asks it "find me the
    // information". Boots after CAMS (13) whose assets it auto-indexes, and
    // before Media (21) which registers documents with it.
    {
        auto& search = search::SearchEngine::Instance();
        if (auto r = search.Initialize(); !r.ok()) {
            logger.Warning("SearchEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<search::SearchEngine>(&search);
            if (auto s = search.Start(); !s.ok())
                logger.Warning("SearchEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("MediaEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<media::MediaEngine>(&media);
            if (auto s = media.Start(); !s.ok())
                logger.Warning("MediaEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("SceneCompositionEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<scene::SceneCompositionEngine>(&sce);
            if (auto s = sce.Start(); !s.ok())
                logger.Warning("SceneCompositionEngine start: " + s.error().message, "Kernel");
            AppendBootLog("SceneEngine");
        }
    }

    // ---- 24. BibleEngine (Phase 12, docs/specs/24) ----
    // The first-class Scripture knowledge system: imports, validates, indexes,
    // resolves references, queries, formats, and compares Bibles — completely
    // independent of the UI. Boots after the Search Engine (20) which it uses
    // for verse indexing.
    {
        auto& bible = bible::BibleEngine::Instance();
        if (auto r = bible.Initialize(); !r.ok()) {
            logger.Warning("BibleEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<bible::BibleEngine>(&bible);
            if (auto s = bible.Start(); !s.ok())
                logger.Warning("BibleEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("SongEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<song::SongEngine>(&songs);
            if (auto s = songs.Start(); !s.ok())
                logger.Warning("SongEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("FlowEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<automation::FlowEngine>(&flow);
            if (auto s = flow.Start(); !s.ok())
                logger.Warning("FlowEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("ProductionEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<production::ProductionEngine>(&prod);
            if (auto s = prod.Start(); !s.ok())
                logger.Warning("ProductionEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("RecordingEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<recording::RecordingEngine>(&rec);
            if (auto s = rec.Start(); !s.ok())
                logger.Warning("RecordingEngine start: " + s.error().message, "Kernel");
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
            logger.Warning("BroadcastEngine init: " + r.error().message, "Kernel");
        } else {
            (void)services.Register<broadcast::BroadcastEngine>(&bc);
            if (auto s = bc.Start(); !s.ok())
                logger.Warning("BroadcastEngine start: " + s.error().message, "Kernel");
            AppendBootLog("BroadcastEngine");
        }
    }

    // ---- 13. IPC server + remote log streaming (optional, SystemArchitecture §2) ----
    if (options.ipcPort > 0) {
        auto& ipc = IpcServer::Instance();
        if (auto r = ipc.Start(options.ipcPort); !r.ok()) {
            logger.Warning("IPC server failed to start: " + r.error().message, "Kernel");
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
    logger.Info(std::format("Kernel ready in {} ms — state=Running",
                             static_cast<double>(bootTime.count()) / 1000.0),
                "Kernel");
    return Ok();
}

Result<void> Kernel::FailBoot(const Error& cause) {
    auto& logger = Logger::Instance();
    logger.Fatal(std::format("Boot failed: [{}] {}", cause.code, cause.message), "Kernel");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = cause.message;
    }
    (void)ShutdownSystems();
    state_.store(KernelState::Stopped);
    return cause;
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
    // Phase 14 (26) stops first: it was booted last, so it tears down first.
    (void)automation::FlowEngine::Instance().Shutdown();
    // Phase 13 (25) and Phase 12 (24) stop before Scene (23): they were booted
    // after it, so they tear down in reverse order.
    (void)song::SongEngine::Instance().Shutdown();
    (void)bible::BibleEngine::Instance().Shutdown();
    (void)scene::SceneCompositionEngine::Instance().Shutdown();
    (void)media::MediaEngine::Instance().Shutdown();
    (void)search::SearchEngine::Instance().Shutdown();
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
    c.health = GetHealth();
    return c;
}

HealthReport Kernel::GetHealth() const {
    HealthReport r;
    r.state = state_.load() == KernelState::Running ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("state={} uptime={}ms systems={}", ToString(state_.load()),
                           Uptime().count() / 1000, BootLog().size());
    std::lock_guard<std::mutex> lock(mutex_);
    r.lastError = lastError_;
    return r;
}

} // namespace bps
