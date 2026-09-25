// bps_cli — boots the engine core and demonstrates the systems working together
// (Kernel boot/shutdown, EventBus fan-out, ThreadPool, TaskScheduler, MemoryManager,
// ResourceManager, ModuleManager + a demo module). See docs/specs/.

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/ipc/IpcServer.hpp"
#include "core/kernel/Kernel.hpp"
#include "core/logging/Logger.hpp"
#include "core/memory/MemoryManager.hpp"
#include "core/modules/ModuleManager.hpp"
#include "core/resources/ResourceManager.hpp"
#include "core/services/ServiceManager.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"
#include "modules/automation/FlowEngine.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "modules/bible/BibleEngine.hpp"
#include "modules/media/MediaModule.hpp"
#include "modules/presentation/PresentationModule.hpp"
#include "modules/songs/SongEngine.hpp"
#include "modules/songs/SongsModule.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
// GCC 14+ ships <print>, but the msys2 GCC 15.2 (Rev8) MinGW package's
// libstdc++ DLL lacks the std::__open_terminal/__write_to_terminal exports
// its std::println needs (the vprint_unicode terminal path) — the link dies
// with undefined references. The <format>+cstdio shim below is the same
// surface; the real std::print comes back when the package is fixed.
#if defined(__cpp_lib_print) && !defined(BPS_PRINT_TERMINAL_BROKEN)
#include <print>
#else
#include <string_view>
// MinGW 13's libstdc++ predates <print> (GCC 14 ships it); same surface via
// <format> + cstdio until the toolchain moves up. The real std::print /
// std::println are used unchanged once __cpp_lib_print is defined.
namespace std {
template <typename... Args>
void print(string_view fmt, Args&&... args) {
    fputs(vformat(fmt, make_format_args(args...)).c_str(), stdout);
}
template <typename... Args>
void print(FILE* stream, string_view fmt, Args&&... args) {
    fputs(vformat(fmt, make_format_args(args...)).c_str(), stream);
}
template <typename... Args>
void println(string_view fmt, Args&&... args) {
    fputs((vformat(fmt, make_format_args(args...)) + '\n').c_str(), stdout);
}
template <typename... Args>
void println(FILE* stream, string_view fmt, Args&&... args) {
    fputs((vformat(fmt, make_format_args(args...)) + '\n').c_str(), stream);
}
}  // namespace std
#endif
#include <string>
#include <thread>

using namespace bps;

int main(int argc, char** argv) {
    BootOptions opts;
    opts.logLevel = LogLevel::Info;
    int holdSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--trace") opts.logLevel = LogLevel::Trace;
        else if (a == "--debug") opts.logLevel = LogLevel::Debug;
        else if (a == "--quiet") opts.logLevel = LogLevel::Warning;
        else if (a == "--performance") opts.resourceMode = ResourceMode::Performance;
        else if (a == "--battery") opts.resourceMode = ResourceMode::Battery;
        else if (a == "--ipc" && i + 1 < argc)
            opts.ipcPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--data" && i + 1 < argc)
            opts.dataDir = argv[++i];   // persistent DB (kernel.json) under this dir
        else if (a == "--hold" && i + 1 < argc)
            holdSeconds = std::atoi(argv[++i]);
    }

    auto boot = Kernel::Instance().Boot(opts);
    if (!boot.ok()) {
        std::println(stderr, "Boot failed: {}", boot.error().message);
        return 1;
    }

    // With --ipc, expose a presentation command for remote clients (bps_remote).
    if (opts.ipcPort > 0) {
        (void)IpcServer::Instance().RegisterHandler(
            "presentation.slide", [](const json::Value& params) {
                int idx = params.Find("index")
                              ? static_cast<int>(params.Find("index")->asInt(0)) : 0;
                (void)EventBus::Instance().Publish(
                    events::SlideChanged{idx, 12, std::format("Slide {}", idx + 1)});
                json::Value::Object o;
                o["index"] = json::Value::Number(idx);
                return json::Value(std::move(o));
            });
        std::println("ipc server     listening on 127.0.0.1:{} (try: bps_remote {} health)",
                     IpcServer::Instance().Port(), IpcServer::Instance().Port());
    }

    // --- EventBus: the canonical fan-out example (05 §7) ---
    EventBus::Instance().Subscribe<events::SlideChanged>(
        [](const events::SlideChanged& e) {
            Logger::Instance().Info(
                std::format("[cli] subscriber received slide {} of {}", e.index + 1, e.total),
                "CLI");
        },
        0);

    // --- ModuleManager: register and start the feature modules ---
    auto& modules = ModuleManager::Instance();
    auto reg = modules.RegisterModule(modules::PresentationModule::Create());
    if (!reg.ok()) {
        std::println(stderr, "Register module failed: {}", reg.error().message);
    }
    (void)modules.Start("presentation");
    (void)EventBus::Instance().Publish(events::SlideChanged{2, 12, "The Good Shepherd"});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Songs module: lyric library + verse navigation (songs.selected / verse_changed).
    (void)modules.RegisterModule(modules::SongsModule::Create());
    (void)modules.Start("songs");
    auto songsMod = std::static_pointer_cast<modules::SongsModule>(modules.GetModule("songs"));
    if (songsMod) {
        (void)songsMod->RegisterSong(modules::Song{"h100", "Amazing Grace", "John Newton",
                                                   {"Amazing grace, how sweet the sound...",
                                                    "'Twas grace that taught my heart to fear..."}});
        (void)songsMod->Select("h100");
        (void)songsMod->NextVerse();   // -> verse 2 published
    }

    // Media module: playlist playback with a scheduler-driven position clock.
    (void)modules.RegisterModule(modules::MediaModule::Create());
    (void)modules.Start("media");
    auto mediaMod = std::static_pointer_cast<modules::MediaModule>(modules.GetModule("media"));
    if (mediaMod) {
        (void)mediaMod->Enqueue(modules::MediaItem{"intro", "Welcome Video",
                                                   modules::MediaType::Video, 2.0});
        (void)mediaMod->Play("intro");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        (void)mediaMod->Pause();
        std::println("media: \"{}\" paused at {:.2f}s / {:.1f}s",
                     mediaMod->CurrentTitle(), mediaMod->PositionSec(), 2.0);
        (void)mediaMod->Stop();
    }

    // --- Bible Engine (Phase 12): import, resolve, search, format, compare ---
    auto& bible = bible::BibleEngine::Instance();
    (void)bible.Initialize();
    (void)bible.Start();
    const char* kjvXml =
        "<bible abbrev=\"TEST\" name=\"Test Bible\">"
        "<book num=\"JHN\" name=\"John\">"
        "<chapter num=\"3\"><verse num=\"16\">For God so loved the world that he gave his one "
        "and only Son, that whoever believes in him shall not perish but have eternal life.</verse>"
        "</chapter></book></bible>";
    auto bibleId = bible.Import(kjvXml, "xml", bible::ImportOptions{/*id*/ "TEST", "", true, false});
    if (bibleId.ok()) {
        auto ref = bible.ResolveReference("John 3:16", "TEST");
        if (ref.ok()) {
            auto passage = bible.GetPassage("TEST", ref.value());
            if (passage.ok())
                std::println("bible: John 3:16 -> {}", passage.value().front().text);
            auto fmt = bible.Format("TEST", ref.value(),
                                    bible::FormatOptions{bible::FormatOptions::Mode::VersePerLine,
                                                         true, true, false, false});
            if (fmt.ok()) std::println("bible formatted:\n{}", fmt.value());
        }
        auto hits = bible.Search("loved the world", "TEST");
        std::println("bible search \"loved the world\": {} hit(s)",
                     hits.ok() ? hits.value().size() : 0);
    }

    // --- Song Engine (Phase 13): import, transpose, arrange ---
    auto& songs = song::SongEngine::Instance();
    (void)songs.Initialize();
    (void)songs.Start();
    const char* chordPro =
        "{title: Amazing Grace}\n{key: C}\n{author: John Newton}\n"
        "[Verse 1]\nC          G\nAmazing grace, how sweet the sound\n"
        "[Chorus]\nF          C\nThat saved a wretch like me\n";
    auto songId = songs.Import(chordPro, "chordpro");
    if (songId.ok()) {
        auto transposed = songs.Transposed(songId.value(), 2);   // C -> D
        if (transposed.ok()) {
            std::println("song: \"{}\" in {} (transposed +2)",
                         transposed.value().metadata.title,
                         transposed.value().metadata.performanceKey.empty()
                             ? transposed.value().metadata.originalKey
                             : transposed.value().metadata.performanceKey);
            for (const auto& sec : transposed.value().sections)
                for (const auto& line : sec.lines)
                    for (const auto& cr : line.chords)
                        std::println("song chord: {}", cr.chord.Display());
        }
        song::Arrangement sunday;
        sunday.id = "sunday";
        sunday.name = "Sunday";
        for (const auto& sec : transposed.ok() ? transposed.value().sections
                                               : songs.GetSong(songId.value()).value().sections)
            sunday.sectionIds.push_back(sec.id);
        (void)songs.AddArrangement(songId.value(), sunday);
        std::println("song arrangements: {}", songs.Arrangements(songId.value()).value().size());
    }

    // --- Automation Engine (Phase 14): load, start, wait-for-event, complete ---
    auto& flow = automation::FlowEngine::Instance();
    (void)flow.Initialize();
    (void)flow.Start();
    // Plugin action: a consumer registers a domain action without touching the
    // engine core (docs/specs/26 §Plugin Support) — implement, register, done.
    (void)flow.RegisterAction("play_song", []() -> std::shared_ptr<automation::IAction> {
        struct PlaySong final : automation::IAction {
            const char* Type() const noexcept override { return "play_song"; }
            Result<automation::ActionOutcome> Execute(automation::AutomationContext&) override {
                return automation::ActionOutcome{automation::ActionOutcome::Kind::Completed,
                                                 {}, {}};
            }
        };
        return std::make_shared<PlaySong>();
    });
    const char* serviceVgr =
        "{\"type\":\"flow\",\"flow\":{"
        "\"id\":\"sunday\",\"name\":\"Sunday Service\","
        "\"variables\":{\"SERVICE_NAME\":\"Morning Service\"},"
        "\"nodes\":["
        "{\"id\":\"welcome\",\"label\":\"Welcome\",\"actionType\":\"log\","
        "\"actionPayload\":\"Welcome to {SERVICE_NAME}\"},"
        "{\"id\":\"song\",\"label\":\"Worship Song\",\"actionType\":\"play_song\"},"
        "{\"id\":\"video\",\"label\":\"Video\",\"kind\":\"wait\","
        "\"waitFor\":\"media.state_changed\"},"
        "{\"id\":\"end\",\"label\":\"End\",\"kind\":\"end\"}"
        "]}}";
    auto flowId = flow.Load(serviceVgr, "vgr");
    if (flowId.ok()) {
        auto execId = flow.Start("sunday");
        if (execId.ok()) {
            // Video "finishes": the WAIT node resolves and the flow completes.
            (void)flow.Notify("media.state_changed");
            auto fstate = flow.ExecutionState(execId.value());
            std::println("flow: \"{}\" state={} nodes={}",
                         flowId.ok() ? flowId.value() : "?",
                         fstate.ok() && fstate.value().state == automation::FlowState::Completed
                             ? "completed"
                             : "running",
                         fstate.ok() ? fstate.value().history.size() : 0);
        }
    }

    // --- ProductionEngine (Phase 15): route a graph, plan, and check health ---
    {
        auto& prod = production::ProductionEngine::Instance();
        namespace pr = bps::production;
        auto& g = prod.Graph();
        // Audio: mics + music -> worship bus -> program bus.
        auto mic1 = g.AddSource("mic1", "Mic 1", pr::SignalType::Audio, {5, 3, 0, 0});
        auto mic2 = g.AddSource("mic2", "Mic 2", pr::SignalType::Audio, {5, 3, 0, 0});
        auto music = g.AddSource("music", "Music", pr::SignalType::Audio, {10, 5, 0, 64});
        auto worship = prod.CreateBus("worship", "Worship Bus", pr::BusRole::Group,
                                      pr::SignalType::Audio);
        auto program = prod.CreateBus("program", "Program Audio", pr::BusRole::Program,
                                      pr::SignalType::Audio);
        if (mic1.ok()) (void)g.Connect(mic1.value(), "worship", pr::SignalType::Audio);
        if (mic2.ok()) (void)g.Connect(mic2.value(), "worship", pr::SignalType::Audio);
        if (music.ok()) (void)g.Connect(music.value(), "worship", pr::SignalType::Audio);
        if (worship.ok()) (void)g.Connect(worship.value(), "program", pr::SignalType::Audio);
        // Video: camera -> program video bus.
        auto cam = g.AddSource("cam1", "Camera 1", pr::SignalType::Video, {60, 15, 2048, 512});
        auto pvb = prod.CreateBus("pvb", "Program Video", pr::BusRole::Program,
                                  pr::SignalType::Video);
        if (cam.ok()) (void)g.Connect(cam.value(), "pvb", pr::SignalType::Video);
        // Outputs: TV and RTMP each carry video + audio buses.
        pr::OutputConfig tvCfg, rtmpCfg;
        tvCfg.priority = pr::OutputPriority::Critical;
        rtmpCfg.priority = pr::OutputPriority::High;
        rtmpCfg.networkTarget = "rtmp://ingest.example/live";
        auto tv = g.AddOutput("tv", "TV", pr::SignalType::Video, tvCfg);
        auto rtmp = g.AddOutput("rtmp", "RTMP", pr::SignalType::Video, rtmpCfg);
        if (tv.ok()) (void)prod.AssignOutputBuses("tv", "pvb", "program");
        if (rtmp.ok()) (void)prod.AssignOutputBuses("rtmp", "pvb", "program");
        // Circular route must be rejected.
        bool rejected = !g.Connect("program", "worship", pr::SignalType::Audio).ok();
        // Duck music under the speaker, then plan and check.
        (void)prod.Duck("program", "mic1", 6.0);
        auto plan = prod.PlanProduction({100, 100, 8192, 16384});
        auto health = prod.CheckProduction();
        std::println("production: nodes={} edges={} cycleRejected={} duck={}dB "
                     "feasible={} health={}%",
                     g.NodeCount(), g.EdgeCount(), rejected ? "yes" : "no",
                     static_cast<int>(prod.DuckDepth("program")), plan.feasible ? "yes" : "no",
                     health.score);
        (void)prod.SendControl("midi", "note-on C3");
    }

    // --- RecordingEngine (Phase 16): record a bus + ISO, markers, replay ---
    {
        auto& rec = recording::RecordingEngine::Instance();
        namespace rk = bps::recording;
        // Master profile for the program bus (docs/specs/28 §2).
        rk::RecordingProfile master;
        master.id = "master";
        master.name = "Sunday Service Master";
        master.container = rk::ContainerKind::MKV;
        master.videoCodec = rk::VideoCodec::H264;
        master.audioCodec = rk::AudioCodec::AAC;
        master.quality = rk::QualityPreset::Broadcast;
        master.segmentDurationMs = 60000;  // 1-minute segments
        auto pid = rec.CreateProfile(master);
        // ISO profile: isolated camera, post-processing tap.
        rk::RecordingProfile iso;
        iso.id = "iso";
        iso.name = "Camera ISO";
        iso.priority = rk::RecordingPriority::Low;
        auto isop = rec.CreateProfile(iso);
        // Record the program video bus and the camera independently. Fall
        // back to the engine-default "master" profile if creation failed.
        auto masterRec = rec.StartRecording(pid.ok() ? pid.value() : "master", "pvb",
                                            rk::TapPoint::PostProcessing);
        auto isoRec = rec.StartRecording(isop.ok() ? isop.value() : "master", "cam1",
                                         rk::TapPoint::PreProcessing);
        // Markers + semantic metadata (docs/specs/28 §24-§25).
        if (masterRec.ok()) {
            (void)rec.AddMarker(masterRec.value(), "Sermon Started");
            (void)rec.SetMetadata(masterRec.value(), "event", "Sunday Service");
            (void)rec.SetMetadata(masterRec.value(), "speaker", "John");
            (void)rec.SetMetadata(masterRec.value(), "scripture", "John 3:16");
        }
        // Instant replay: rolling 30s buffer on the program video bus.
        auto replay = rec.CreateReplay("pvb", rk::ReplayMode::Normal, 30000);
        (void)rec.Tick(125000);  // drives segments + schedules
        auto st = rec.GetStatus(masterRec.ok() ? masterRec.value() : "");
        auto seg = st.ok() ? st.value().segmentIndex : 0u;
        std::println("recording: master={} iso={} replay={} segments={} disk={}% clock={}ms",
                     masterRec.ok() ? "recording" : "failed",
                     isoRec.ok() ? "recording" : "failed",
                     replay.ok() ? "ready" : "failed", seg,
                     rec.GetStorageInfo().ok() ? rec.GetStorageInfo().value().freePercent : -1,
                     rec.ClockMs());
        if (masterRec.ok()) (void)rec.StopRecording(masterRec.value(), "service complete");
        if (isoRec.ok()) (void)rec.StopRecording(isoRec.value(), "service complete");
        (void)rec.RemoveReplay(replay.ok() ? replay.value() : "");
    }

    // --- BroadcastEngine (Phase 17): NDI + SDI providers + software loopback ---
    {
        auto& bc = broadcast::BroadcastEngine::Instance();
        namespace br = bps::broadcast;
        // NDI/SDI availability (runtime SDK probing; Unsupported when absent).
        auto ndiProbe = bc.Probe("ndi");
        auto sdiProbe = bc.Probe("sdi");
        // Software loopback round-trip: sender -> ring -> receiver.
        auto sender = bc.CreateNdiSender("Worship Cam");
        std::vector<uint8_t> frame(1920 * 1080 * 2, 0x10);   // UYVY-ish
        br::VideoFrameInfo vf;
        vf.width = 1920; vf.height = 1080;
        auto sent = sender.ok() ? bc.SendVideoFrame(sender.value(), vf, frame.data(), frame.size())
                                : Result<void>(Error::Make(Err::Broadcast_Unsupported, "cli",
                                                           "no sender"));
        auto discovered = bc.DiscoverNdiSources();
        auto sources = discovered.ok() ? discovered.value() : std::vector<br::NdiSourceInfo>{};
        auto recv = sources.empty()
                        ? bc.CreateNdiReceiver("Worship Cam")
                        : bc.CreateNdiReceiver(sources.front().name);
        br::VideoFrameInfo got;
        std::vector<uint8_t> payload;
        auto received = recv.ok() ? bc.ReceiveFrame(recv.value(), got, payload) : Result<bool>(false);
        std::println("broadcast: ndi={} sdi={} send={} recv={} sources={}",
                     ndiProbe.ok() && ndiProbe.value() == br::ProviderState::Available
                         ? "available"
                         : "unavailable",
                     sdiProbe.ok() && sdiProbe.value() == br::ProviderState::Available
                         ? "available"
                         : "unavailable",
                     sent.ok() ? "ok" : sent.error().message,
                     received.ok() && received.value() ? "ok" : "none", sources.size());
        if (sender.ok()) (void)bc.StopSender(sender.value());
        if (recv.ok()) (void)bc.DisconnectReceiver(recv.value());
        auto stats = bc.Stats();
        std::println("broadcast stats: sent={} received={} errors={}",
                     stats.framesSent, stats.framesReceived, stats.errors);
    }

    // --- ThreadPool: a future computed on a worker ---
    auto fut = ThreadPool::Instance().SubmitFuture(
        TaskClass::Foreground, 0, [](int x) { return x * x; }, 7);
    if (fut.ok())
        std::println("thread pool: 7^2 = {}", fut.value().get());
    else
        std::println("thread pool: submit failed ({})", fut.error().message);

    // --- TaskScheduler: delayed + recurring ---
    (void)TaskScheduler::Instance().ScheduleOnce(
        [] { Logger::Instance().Info("delayed task fired", "CLI"); },
        std::chrono::milliseconds(80));
    static std::atomic<int> ticks{0};
    auto tickTask = TaskScheduler::Instance().ScheduleEvery(
        [] { ticks.fetch_add(1); }, std::chrono::milliseconds(25));

    // --- MemoryManager: tagged allocation + leak check ---
    if (auto mem = MemoryManager::Instance().Allocate("cli", 4096); mem.ok())
        (void)MemoryManager::Instance().Release("cli", mem.value());
    auto leaks = MemoryManager::Instance().CheckLeaks();
    std::println("memory: live={} bytes, leaks={}",
                 MemoryManager::Instance().TotalLiveBytes(),
                 leaks.ok() ? "none" : leaks.error().message);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // --- Health of every core system ---
    std::println("\n--- core health ---");
    auto hr = [](const HealthReport& h) {
        return std::format("{} ({})", ToString(h.state), h.detail);
    };
    std::println("Kernel          {}", hr(Kernel::Instance().GetHealth()));
    std::println("Logger          {}", hr(Logger::Instance().GetHealth()));
    std::println("EventBus        {}", hr(EventBus::Instance().GetHealth()));
    std::println("ThreadPool      {}", hr(ThreadPool::Instance().GetHealth()));
    std::println("TaskScheduler   {}", hr(TaskScheduler::Instance().GetHealth()));
    std::println("ResourceManager {}", hr(ResourceManager::Instance().GetHealth()));
    std::println("ModuleManager   {}", hr(ModuleManager::Instance().GetHealth()));
    auto tel = ResourceManager::Instance().Telemetry();
    std::println("telemetry       mem={}MB/{}MB cpu={:.1f}% battery={}%",
                 tel.usedRamBytes / (1024 * 1024), tel.totalRamBytes / (1024 * 1024),
                 tel.cpuUsagePct, tel.batteryPercent);
    auto build = Kernel::Instance().GetBuildInfo();
    auto rt = Kernel::Instance().GetRuntimeInfo();
    std::println("engine uuid     {}", rt.engineUuid);
    std::println("session id      {}", rt.sessionId);
    std::println("build           v{} {} {}/{} ({})", build.version.ToString(),
                 build.buildType, build.platform, build.arch, build.buildDate);
    std::println("event bus       published={} deadletters={} history={}",
                 EventBus::Instance().PublishedCount(), EventBus::Instance().DeadLetterCount(),
                 EventBus::Instance().History().size());
    std::println("scheduler       tick count={}", ticks.load());
    std::print("boot order      ");
    for (const auto& s : Kernel::Instance().BootLog()) std::print("{} -> ", s);
    std::println("ready");

    (void)TaskScheduler::Instance().Cancel(tickTask.value());

    // --hold <seconds>: keep the engine (and its IPC server) alive so a remote
    // control app can attach: bps_remote <port> health | logs | slide | shutdown.
    if (holdSeconds > 0) {
        std::println("\nengine running for {} s on 127.0.0.1:{} — try: bps_remote {} health",
                     holdSeconds, IpcServer::Instance().Port(), IpcServer::Instance().Port());
        // Ends early if a remote client shuts the engine down (bps_remote shutdown).
        for (int s = 0; s < holdSeconds && Kernel::Instance().State() == KernelState::Running; ++s)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::println("\nshutting down...");
    if (Kernel::Instance().State() != KernelState::Running) {
        std::println("engine already shut down (e.g. by bps_remote ... shutdown)");
        return 0;
    }
    auto shutdown = Kernel::Instance().Shutdown();
    std::println("{}", shutdown.ok() ? "shutdown OK" : shutdown.error().message);
    return shutdown.ok() ? 0 : 2;
}
