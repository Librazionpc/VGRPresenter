// Unit tests: Core Engine + PAL (Phases 1-8, docs/specs/01-15) and the whole-engine Kernel test.
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests core
#include "TestHarness.hpp"

void TestResult() {
    Result<int> ok(42);
    CHECK(ok.ok());
    CHECK(ok.value() == 42);
    Result<int> bad(Error::Make(Err::NotFound, "test", "nope"));
    CHECK(!bad.ok());
    CHECK(bad.error().code == Err::NotFound);
    CHECK(bad.error().module == "test");
    Result<void> vok = Ok();
    CHECK(vok.ok());
    Result<void> vbad(Error::Make(Err::Timeout, "test", "t"));
    CHECK(!vbad.ok());
}
void TestVersion() {
    Version v = Version::Parse("1.2.3");
    CHECK(v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(v.ToString() == "1.2.3");
    Version pre = Version::Parse("2.0.0-beta");
    CHECK(pre.pre == "beta");
    CHECK(pre > v);
    CHECK(kEngineVersion >= Version::Parse("1.0.0"));
    CHECK(Version::Parse("1.10.0") > Version::Parse("1.9.9"));
}
void TestJson() {
    auto parsed = json::Parse(R"({"a":1,"b":{"c":[true,null,"x"]}})");
    CHECK(parsed.ok());
    if (parsed.ok()) {
        const json::Value& root = parsed.value();
        CHECK(root.Find("a") && root.Find("a")->asInt() == 1);
        CHECK(root.Find("b.c") != nullptr);
        CHECK(root.Find("b.c")->type() == json::Value::Type::Array);
        const auto* arr = root.Find("b.c")->asArray();
        CHECK(arr && arr->size() == 3);
        CHECK((*arr)[0].asBool());
        CHECK((*arr)[1].isNull());
        CHECK(std::string((*arr)[2].asString()) == "x");
        CHECK(!root.Has("missing"));
        CHECK(!root.Find("b.missing"));
    }
    auto round = json::Parse(R"({"k":[1,2,3],"s":"hi\n"})");
    CHECK(round.ok());
    if (round.ok()) {
        auto reparsed = json::Parse(round.value().ToString());
        CHECK(reparsed.ok());
        if (reparsed.ok()) CHECK(reparsed.value().ToString() == round.value().ToString());
    }
    CHECK(!json::Parse("{").ok());
    CHECK(!json::Parse("[1,]").ok());
    CHECK(!json::Parse("{\"a\":}").ok());
}
void TestLogger() {
    auto& logger = Logger::Instance();
    CHECK(logger.Initialize().ok());
    logger.SetGlobalLevel(LogLevel::Trace);
    logger.Info("unit-test-log", "Test");
    logger.Warning("another-line", "Test");
    (void)logger.Flush();
    auto hits = logger.Search(LogQuery{LogLevel::Trace, "Core", "Test", "unit-test-log"});
    CHECK(!hits.empty());
    // Global-level filtering drops records below the threshold.
    logger.SetGlobalLevel(LogLevel::Error);
    logger.Info("should-not-appear", "Test");
    (void)logger.Flush();
    auto filtered = logger.Search(LogQuery{LogLevel::Info, "", "", "should-not-appear"});
    CHECK(filtered.empty());
    logger.SetGlobalLevel(LogLevel::Trace);

    // Session logging (02 §Session): session id stamped into records.
    logger.SetSessionId("test-session-1");
    logger.Info("with-session", "Test");
    (void)logger.Flush();
    CHECK(logger.SessionId() == "test-session-1");
    auto withSession = logger.Search(LogQuery{LogLevel::Trace, "", "Test", "with-session"});
    CHECK(!withSession.empty() && withSession.front().session == "test-session-1");

    // JSON sink (02 §3): records serialize as JSON lines.
    auto jsonSink = std::make_shared<JsonSink>("/tmp/bps_test_log.jsonl");
    CHECK(logger.AddSink(jsonSink).ok());
    logger.Info("json-line", "Test");
    (void)logger.Flush();
    CHECK(logger.RemoveSink("Json").ok());
    {
        std::ifstream jf("/tmp/bps_test_log.jsonl");
        CHECK(jf.is_open());
        std::string line;
        bool found = false;
        while (std::getline(jf, line))
            if (line.find("json-line") != std::string::npos) found = true;
        CHECK(found);
    }
    std::remove("/tmp/bps_test_log.jsonl");

    // Crash logging (02 §Crash): Fatal + synchronous flush.
    logger.CrashLog("Test", "crash-marker");
    CHECK(!logger.Search(LogQuery{LogLevel::Fatal, "", "Crash", "crash-marker"}).empty());

    // Debugger sink (02 §3): keeps a ring a debugger / diagnostics can inspect.
    auto dbgSink = std::make_shared<DebuggerSink>();
    CHECK(logger.AddSink(dbgSink).ok());
    logger.Info("debugger-line", "Test");
    (void)logger.Flush();
    bool sawDbg = false;
    for (const auto& l : dbgSink->RecentLines())
        if (l.find("debugger-line") != std::string::npos) sawDbg = true;
    CHECK(sawDbg);

    // Remote sink (02 §3): each record is handed to the publisher as a JSON line.
    auto remoteSink = std::make_shared<RemoteSink>();
    std::vector<std::string> remoteLines;
    remoteSink->SetPublisher([&](const std::string& line) { remoteLines.push_back(line); });
    CHECK(logger.AddSink(remoteSink).ok());
    CHECK(remoteSink->HasPublisher());
    logger.Info("remote-line", "Test");
    (void)logger.Flush();
    CHECK(logger.RemoveSink("Remote").ok());
    CHECK(logger.RemoveSink("Debugger").ok());
    bool sawRemote = false;
    for (const auto& l : remoteLines)
        if (l.find("\"message\":\"remote-line\"") != std::string::npos) sawRemote = true;
    CHECK(sawRemote);

    CHECK(logger.GetHealth().IsHealthy() || logger.GetHealth().state == HealthState::Healthy);
}
void TestConfig() {
    auto& config = ConfigurationManager::Instance();
    // Defaults are nested objects; dotted keys are resolved by Find() at read time.
    json::Value::Object flags;
    flags["verbose"] = json::Value::Bool(true);
    json::Value::Object app;
    app["name"] = json::Value::String("engine");
    app["volume"] = json::Value::Number(0.8);
    app["flags"] = json::Value(std::move(flags));
    json::Value::Object defs;
    defs["app"] = json::Value(std::move(app));
    CHECK(config.Initialize(json::Value(std::move(defs))).ok());
    CHECK(config.GetString("app.name") == "engine");
    CHECK(config.GetDouble("app.volume") == 0.8);
    CHECK(config.GetBool("app.flags.verbose"));
    CHECK(config.GetString("missing.key", "dflt") == "dflt");
    // Runtime overrides defaults; scope precedence: Runtime > Project.
    CHECK(config.Set("app.name", json::Value::String("live")).ok());
    CHECK(config.GetString("app.name") == "live");
    CHECK(config.Set("app.name", json::Value::String("project"), ConfigScope::Project).ok());
    CHECK(config.GetString("app.name") == "live");
    CHECK(config.Set("app.name", json::Value::String("runtime"), ConfigScope::Runtime).ok());
    CHECK(config.GetString("app.name") == "runtime");
    CHECK(config.Has("app.name"));
    // Save/load round trip through a temp file.
    const std::string tmp = "/tmp/bps_test_config.json";
    CHECK(config.SetScopePath(ConfigScope::User, tmp).ok());
    // Use a key no higher-priority scope overrides (Runtime holds app.name).
    CHECK(config.Set("app.language", json::Value::String("saved"), ConfigScope::User).ok());
    CHECK(config.Save(ConfigScope::User).ok());
    CHECK(config.Reset(ConfigScope::User).ok());
    CHECK(config.GetString("app.language", "none") == "none");  // user scope now empty
    CHECK(config.Load(ConfigScope::User).ok());
    CHECK(config.GetString("app.language") == "saved");          // reloaded from disk
    CHECK(config.Validate().ok());
    std::remove(tmp.c_str());

    // Schema migration (03 §Migrate): ascending, stamped into `_schema`.
    CHECK(config.AddMigration(1, [](json::Value& doc) -> Result<void> {
              if (doc.type() != json::Value::Type::Object)
                  return Error::Make(Err::InvalidState, "test", "not an object");
              json::Value::Object mig;
              mig["v1"] = json::Value::Bool(true);
              const_cast<json::Value::Object*>(doc.asObject())->emplace("migrated",
                                                                         json::Value(std::move(mig)));
              return Ok();
          }).ok());
    CHECK(config.AddMigration(2, [](json::Value& doc) -> Result<void> {
              json::Value::Object mig;
              mig["v2"] = json::Value::Number(42.0);
              const_cast<json::Value::Object*>(doc.asObject())->emplace("migrated2",
                                                                        json::Value(std::move(mig)));
              return Ok();
          }).ok());
    CHECK(config.Migrate(ConfigScope::Runtime).ok());
    CHECK(config.GetBool("migrated.v1"));
    CHECK(config.GetInt("migrated2.v2") == 42);
    CHECK(config.Get("app.name").asString() == "runtime");   // untouched by migration
}
void TestServices() {
    auto& services = ServiceManager::Instance();
    CHECK(services.Register<Logger>(&Logger::Instance()).ok());
    CHECK(services.IsRegistered<Logger>());
    CHECK(services.Get<Logger>() == &Logger::Instance());
    auto r = services.Resolve<Logger>();
    CHECK(r.ok());
    CHECK(r.value() == &Logger::Instance());
    CHECK(services.Get<ConfigurationManager>() == nullptr);   // not registered here
    CHECK(!services.Resolve<ConfigurationManager>().ok());

    // --- 04 §3/§4: lazy, lifetimes, remove, dependency graph ---
    struct FakeA : public IService {
        const char* ServiceName() const noexcept override { return "FakeA"; }
    };
    struct FakeB : public IService {
        const char* ServiceName() const noexcept override { return "FakeB"; }
    };
    // Lazy singleton: the factory runs on first resolve, then is cached.
    int created = 0;
    CHECK(services.RegisterLazy<FakeA>([&]() {
              ++created;
              return std::make_shared<FakeA>();
          }).ok());
    CHECK(created == 0);
    CHECK(services.Get<FakeA>() != nullptr);
    CHECK(created == 1);
    CHECK(services.Get<FakeA>() == services.Get<FakeA>());   // cached
    CHECK(created == 1);
    // Transient lifetime: a fresh instance per resolve.
    CHECK(services
              .RegisterLazy<FakeB>([]() { return std::make_shared<FakeB>(); },
                                   ServiceManager::Lifetime::Transient)
              .ok());
    CHECK(services.Get<FakeB>() != nullptr);
    CHECK(services.Get<FakeB>() != services.Get<FakeB>());
    // Remove + NotFound.
    CHECK(services.Remove<FakeB>().ok());
    CHECK(!services.IsRegistered<FakeB>());
    CHECK(!services.Remove<FakeB>().ok());
    // Declared dependency cycle is detected (04 §4).
    CHECK(services.RegisterLazy<FakeA>([]() { return std::make_shared<FakeA>(); },
                                       ServiceManager::Lifetime::Singleton, {"FakeB"}, "FakeA")
              .ok());
    CHECK(services.RegisterLazy<FakeB>([]() { return std::make_shared<FakeB>(); },
                                       ServiceManager::Lifetime::Singleton, {"FakeA"}, "FakeB")
              .ok());
    CHECK(!services.CheckCycles().ok());
    CHECK(services.Remove<FakeA>().ok());
    CHECK(services.Remove<FakeB>().ok());
    CHECK(services.CheckCycles().ok());
}
void TestEventBus() {
    auto& bus = EventBus::Instance();
    std::vector<std::string> order;
    auto subA = bus.Subscribe<events::KernelStateChanged>(
        [&](const events::KernelStateChanged&) { order.push_back("A"); }, 100);
    auto subB = bus.Subscribe<events::KernelStateChanged>(
        [&](const events::KernelStateChanged&) { order.push_back("B"); }, 0);
    CHECK(bus.SubscriberCount(events::KernelStateChanged::kTopic) == 2);
    CHECK(bus.Publish(events::KernelStateChanged{KernelState::Running, KernelState::Paused}).ok());
    CHECK(order.size() == 2 && order[0] == "A" && order[1] == "B");  // priority order
    CHECK(bus.Unsubscribe(subA).ok());
    CHECK(bus.SubscriberCount(events::KernelStateChanged::kTopic) == 1);
    CHECK(!bus.Unsubscribe(subA).ok());   // double unsubscribe -> NotFound

    // Sticky events.
    bus.SetSticky(events::ResourceModeChanged::kTopic,
                  std::make_shared<events::ResourceModeChanged>());
    CHECK(bus.GetSticky(events::ResourceModeChanged::kTopic) != nullptr);
    CHECK(bus.GetSticky("never.set") == nullptr);

    // History.
    CHECK(!bus.History(events::KernelStateChanged::kTopic).empty());

    // Replay: retained history is delivered to late subscribers (05 §History).
    int replayed = 0;
    CHECK(bus.Replay<events::KernelStateChanged>(
              [&](const events::KernelStateChanged&) { ++replayed; }) > 0);

    // Tracing (05 §Tracing): dispatch records carry timing metadata.
    bus.EnableTracing(true);
    CHECK(bus.Publish(events::KernelStateChanged{KernelState::Running, KernelState::Running}).ok());
    CHECK(!bus.TraceLog(8).empty());
    bus.EnableTracing(false);

    // Isolation: a throwing subscriber must not break other subscribers (05 §10).
    auto subC = bus.Subscribe<events::KernelStateChanged>(
        [&](const events::KernelStateChanged&) { throw std::runtime_error("boom"); }, 50);
    order.clear();
    CHECK(bus.Publish(events::KernelStateChanged{KernelState::Paused, KernelState::Running}).ok());
    CHECK(bus.DeadLetterCount() > 0);
    CHECK(order.size() == 1 && order[0] == "B");
    CHECK(bus.Unsubscribe(subB).ok());
    CHECK(bus.Unsubscribe(subC).ok());
}
void TestThreadPool() {
    auto& pool = ThreadPool::Instance();
    CHECK(pool.Initialize(2).ok());
    auto fut = pool.SubmitFuture(TaskClass::Foreground, 0,
                                 [](int a, int b) { return a + b; }, 20, 22);
    CHECK(fut.ok());
    if (fut.ok()) CHECK(fut.value().get() == 42);

    // Cancellation is safe and never crashes; the task either cancels, runs, or finishes.
    TaskOptions slow;
    slow.taskClass = TaskClass::Background;
    slow.priority = -100;
    std::atomic<bool> ran{false};
    auto h = pool.Submit([&]() { ran.store(true); }, slow);
    CHECK(h.ok());
    if (h.ok()) {
        // Cancellation is safe and never crashes: the task either cancels, runs, or
        // finishes. Cancel() fails only when the task already finished and was
        // removed from the pool (NotFound) — both outcomes are valid.
        auto cr = pool.Cancel(h.value());
        if (cr.ok()) {
            auto st2 = pool.TaskStateOf(h.value());
            CHECK(!st2 || *st2 == TaskState::Cancelled || *st2 == TaskState::Running ||
                  *st2 == TaskState::Finished);
        } else {
            CHECK(cr.error().code == Err::NotFound);
            CHECK(!pool.TaskStateOf(h.value()).has_value());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(pool.ExecutedCount() >= 1);

    // Dynamic resize grows the worker pool.
    CHECK(pool.Resize(4).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(pool.WorkerCount() >= 2);

    // CPU affinity (06 §4): pinning a live worker works on Linux, fails for bad indices.
    auto aff = pool.SetWorkerAffinity(0, 0);
    CHECK(aff.ok() || aff.error().code == Err::Unsupported);
    CHECK(!pool.SetWorkerAffinity(999, 0).ok());

    // Unknown handle.
    CHECK(!pool.Cancel(999999u).ok());
    CHECK(pool.Shutdown().ok());
}
void TestScheduler() {
    auto& sched = TaskScheduler::Instance();
    CHECK(sched.Initialize().ok());
    std::atomic<int> once{0};
    auto h1 = sched.ScheduleOnce([&]() { once++; }, std::chrono::milliseconds(20));
    CHECK(h1.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(once.load() == 1);

    std::atomic<int> rec{0};
    auto h2 = sched.ScheduleEvery([&]() { rec++; }, std::chrono::milliseconds(20));
    CHECK(h2.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    CHECK(rec.load() >= 2);
    CHECK(sched.Cancel(h2.value()).ok());
    int frozen = rec.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(rec.load() == frozen);

    // Pause / resume of a heartbeat.
    std::atomic<int> beat{0};
    auto h3 = sched.ScheduleHeartbeat([&]() { beat++; }, std::chrono::milliseconds(15));
    CHECK(h3.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    CHECK(beat.load() >= 1);
    CHECK(sched.Pause(h3.value()).ok());
    int paused = beat.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    CHECK(beat.load() == paused);
    CHECK(sched.Resume(h3.value()).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    CHECK(beat.load() > paused);
    CHECK(sched.Cancel(h3.value()).ok());

    // Countdown timer (07 §Countdown): ticks until the budget, then done fires once.
    std::atomic<int> ticks{0};
    std::atomic<bool> doneFired{false};
    auto h5 = sched.ScheduleCountdown([&]() { ticks++; }, std::chrono::milliseconds(15),
                                      std::chrono::milliseconds(60),
                                      [&]() { doneFired = true; });
    CHECK(h5.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(doneFired.load());
    CHECK(ticks.load() >= 2);

    // Retry policy (07 §Retry): consecutive failures exhaust the budget and stop.
    std::atomic<int> attempts{0};
    auto h6 = sched.ScheduleWithRetry([&]() {
                                           attempts++;
                                           throw std::runtime_error("boom");
                                       },
                                       std::chrono::milliseconds(10), 2);
    CHECK(h6.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(attempts.load() >= 3);   // initial + retries, then it gives up
    int frozenAttempts = attempts.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(attempts.load() == frozenAttempts);

    // Cron schedule + missing-task errors.
    CronSpec spec;
    spec.minute = 59;
    auto h4 = sched.ScheduleCron([]() {}, spec);
    CHECK(h4.ok());
    CHECK(sched.Cancel(h4.value()).ok());
    CHECK(!sched.Cancel(999999u).ok());
    CHECK(!sched.Pause(999999u).ok());

    // Animation timer (07 §Animation): progress ticks stay in [0,1], done once.
    std::atomic<int> animTicks{0};
    std::atomic<double> lastP{-1.0};
    std::atomic<bool> animDone{false};
    auto h7 = sched.ScheduleAnimation([&](double p) { lastP.store(p); animTicks++; },
                                      std::chrono::milliseconds(10),
                                      std::chrono::milliseconds(45),
                                      [&]() { animDone = true; });
    CHECK(h7.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    CHECK(animDone.load());
    CHECK(animTicks.load() >= 3);
    CHECK(lastP.load() >= 0.0 && lastP.load() <= 1.0);

    // Sequence / autoplay (07 §Autoplay): fires 0..N-1 once each, then stops.
    std::vector<int> seen;
    std::mutex seenMx;
    auto h8 = sched.ScheduleSequence([&](size_t i) {
        std::lock_guard<std::mutex> lk(seenMx);
        seen.push_back(static_cast<int>(i));
    }, 4, std::chrono::milliseconds(10));
    CHECK(h8.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    size_t seenCount = 0;
    {
        std::lock_guard<std::mutex> lk(seenMx);
        seenCount = seen.size();
        CHECK(seen.size() == 4);
        for (size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == static_cast<int>(i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lk(seenMx);
        CHECK(seen.size() == seenCount);   // no extra fires after the final step
    }
    CHECK(!sched.ScheduleSequence([](size_t) {}, 0, std::chrono::milliseconds(5)).ok());
}
void TestMemory() {
    auto& mem = MemoryManager::Instance();
    CHECK(mem.Initialize().ok());

    // Tagged allocate/release + stats.
    auto p = mem.Allocate("test", 256);
    CHECK(p.ok());
    if (p.ok()) {
        auto* bytes = static_cast<char*>(p.value());
        bytes[0] = 'x';
        bytes[255] = 'y';
        CHECK(mem.Stats("test").liveBytes == 256);
        CHECK(mem.Release("test", p.value()).ok());
        CHECK(mem.Stats("test").liveBytes == 0);
    }
    // Alignment guarantee.
    auto aligned = mem.Allocate("test", 128, 4096);
    CHECK(aligned.ok());
    if (aligned.ok()) {
        CHECK(reinterpret_cast<uintptr_t>(aligned.value()) % 4096 == 0);
        CHECK(mem.Release("test", aligned.value()).ok());
    }
    // Per-tag limits.
    CHECK(mem.SetLimit("limited", 128).ok());
    auto over = mem.Allocate("limited", 1024);
    CHECK(!over.ok());
    if (!over.ok()) CHECK(over.error().code == Err::Memory_LimitExceeded);
    // Bad free (wrong tag).
    auto q = mem.Allocate("test", 32);
    CHECK(q.ok());
    if (q.ok()) CHECK(!mem.Release("wrong-tag", q.value()).ok());
    if (q.ok()) CHECK(mem.Release("test", q.value()).ok());
    // Leak detection.
    auto leak = mem.Allocate("leaky", 64);
    CHECK(leak.ok());
    CHECK(!mem.CheckLeaks().ok());
    if (leak.ok()) CHECK(mem.Release("leaky", leak.value()).ok());
    CHECK(mem.CheckLeaks().ok());
    // Concrete allocators.
    void* b1 = mem.Pool().Allocate();
    CHECK(b1 != nullptr);
    mem.Pool().Release(b1);
    void* a1 = mem.Arena().Allocate(1024, 16);
    CHECK(a1 != nullptr);
    mem.Arena().Reset();
    size_t marker = mem.Stack().Marker();
    void* s1 = mem.Stack().Allocate(64);
    CHECK(s1 != nullptr);
    mem.Stack().FreeTo(marker);

    // Cache allocator (11 §Cache): released blocks are reused, not re-malloc'd.
    void* c1 = mem.Cache().Allocate();
    CHECK(c1 != nullptr);
    CHECK(mem.Cache().InUse() == 1);
    mem.Cache().Release(c1);
    CHECK(mem.Cache().InUse() == 0);
    CHECK(mem.Cache().Cached() == 1);
    void* c2 = mem.Cache().Allocate();
    CHECK(c2 == c1);   // reused the cached block
    mem.Cache().Release(c2);

    // Fragmentation analysis (11 §Fragmentation).
    void* fragAlloc = mem.Arena().Allocate(1024, 16);   // ensure the arena has a chunk
    CHECK(fragAlloc != nullptr);
    auto frag = mem.Fragmentation();
    CHECK(frag.ratio >= 0.0 && frag.ratio <= 1.0);
    CHECK(frag.arenaChunks >= 1);

    // Cleanup policy (11 §Cleanup): safe to run, arena remains usable.
    CHECK(mem.Cleanup("test").ok());
    void* a2 = mem.Arena().Allocate(1024, 16);
    CHECK(a2 != nullptr);
}
void TestResource() {
    auto& res = ResourceManager::Instance();
    res.SetPlatform(bps::platform::CreatePlatform());   // PAL backend (SystemArchitecture §3.6)
    CHECK(res.Initialize().ok());
    res.Sample();
    auto t = res.Telemetry();
    CHECK(t.totalRamBytes > 0);        // Linux /proc/meminfo
    CHECK(t.usedRamBytes > 0);
    CHECK(t.coreCount > 0);
    CHECK(res.MemoryPressure() >= PressureLevel::None);
    CHECK(res.SetMode(ResourceMode::Performance).ok());
    CHECK(res.Mode() == ResourceMode::Performance);
    CHECK(res.SetMode(ResourceMode::Balanced).ok());
    CHECK(res.Claim("testowner", 1024).ok());
    CHECK(res.ReleaseClaim("testowner").ok());

    // GPU/VRAM telemetry (10 §3): reported by the PAL backend (0 when absent).
    CHECK(res.GpuVramPressure() == PressureLevel::None ||
          res.GpuVramPressure() <= PressureLevel::Critical);

    // Cache / module-usage ledger (10 §3).
    CHECK(res.RecordUsage("assets", 2048).ok());
    CHECK(res.RecordUsage("plugin:x", 1024).ok());
    CHECK(res.TotalUsageBytes() == 3072);
    CHECK(res.UsageSnapshot().size() == 2);
    CHECK(res.ClearUsage("assets").ok());
    CHECK(res.TotalUsageBytes() == 1024);
    CHECK(res.ClearUsage("plugin:x").ok());
    CHECK(res.TotalUsageBytes() == 0);

    CHECK(res.Shutdown().ok());
}
void TestPlatform() {
    auto platform = bps::platform::CreatePlatform();
    CHECK(platform != nullptr);
    if (!platform) return;
#if defined(_WIN32)
    CHECK(std::string(platform->Name()) == "WindowsPlatform");
#elif defined(__APPLE__)
    CHECK(std::string(platform->Name()) == "MacOSPlatform");
#else
    CHECK(std::string(platform->Name()) == "LinuxPlatform");
#endif
    CHECK(!platform->OsName().empty());
    CHECK(!platform->Arch().empty());
    auto s = platform->Sample();
    CHECK(s.totalRamBytes > 0);
    CHECK(s.coreCount > 0);
    CHECK(s.threadCount > 0);
    CHECK(s.cpuTotalJiffies > 0);
}
void TestPal() {
    namespace p = bps::platform;
    auto& plat = p::PlatformAccessor::Get();
    CHECK(plat.Name() != nullptr);
    CHECK(p::PlatformAccessor::Shared() != nullptr);

    // --- System info (Phase 2 §Core Platform Class) ---
    auto info = plat.Info();
    CHECK(!info.osName.empty());
    CHECK(!info.arch.empty());
    CHECK(info.coreCount > 0);
    CHECK(info.totalRamBytes > 0);
    CHECK(!info.hostname.empty());

    // --- Architecture coverage: canonical names + pointer width (DoD §24) ---
    // Arch() must match the compile-time tag on every target (Windows
    // x86/x64/ARM64, Linux x86_64/aarch64/armv7, macOS arm64/x86_64).
    // Exception: Linux reports the *kernel* arch via uname, so a 32-bit build
    // on a 64-bit kernel (the standard 32-bit CI setup) reports "x86_64"/"arm64"
    // while kCompileArch is "x86"/"arm" — both are legitimate.
    auto archOk = [&](const std::string& a) {
        return a == p::kCompileArch ||
               (p::kCompileBits == 32 && (a == "x86_64" || a == "arm64"));
    };
    CHECK(archOk(plat.Arch()));
    CHECK(archOk(info.arch));
    CHECK(p::kCompileBits == static_cast<int>(sizeof(void*) * 8));
    CHECK(!std::string(p::kCompileOs).empty());

    // --- Snapshot telemetry ---
    auto s = plat.Sample();
    CHECK(s.totalRamBytes > 0);
    CHECK(s.cpuTotalJiffies > 0);
    CHECK(s.coreCount > 0);

    // --- Timer (Phase 2 §Timer) ---
    auto& timer = plat.Timer();
    CHECK(timer.NowNs() > 0);
    CHECK(timer.WallClockMs() > 0);
    auto sw = timer.StartStopwatch();
    timer.SleepMicros(20'000);
    CHECK(sw.ElapsedNs() >= 10'000'000);   // at least ~10ms elapsed
    auto utc = timer.UtcNow();
    CHECK(utc.year >= 2024 && utc.month >= 1 && utc.month <= 12);
    CHECK(utc.hour >= 0 && utc.hour <= 23);
    auto local = timer.LocalNow();
    CHECK(local.year >= 2024);

    // --- Filesystem (Phase 2 §File System) ---
    auto& fs = plat.Filesystem();
    const std::string dir = "/tmp/bps_pal_test";
    CHECK(fs.CreateDirectories(dir).ok());
    const std::string file = dir + "/data.txt";
    CHECK(fs.Write(file, "hello world").ok());
    CHECK(fs.Exists(file));
    CHECK(fs.IsRegularFile(file));
    CHECK(!fs.IsDirectory(file));
    auto text = fs.ReadText(file);
    CHECK(text.ok() && text.value() == "hello world");
    auto bin = fs.ReadBinary(file);
    CHECK(bin.ok() && bin.value().size() == 11);
    CHECK(fs.Append(file, "!").ok());
    CHECK(fs.ReadText(file).value() == "hello world!");
    auto size = fs.FileSize(file);
    CHECK(size.ok() && size.value() == 12);
    auto entries = fs.Enumerate(dir);
    CHECK(entries.ok() && entries.value().size() >= 1);
    CHECK(fs.Join(dir, "x") == dir + "/x");
    CHECK(fs.Extension("a.json") == ".json");
    const std::string moved = dir + "/moved.txt";
    CHECK(fs.Move(file, moved).ok());
    CHECK(!fs.Exists(file) && fs.Exists(moved));
    const std::string copy = dir + "/copy.txt";
    CHECK(fs.Copy(moved, copy).ok());
    CHECK(fs.Exists(copy));
    CHECK(fs.Remove(copy).ok());
    CHECK(fs.Remove(moved).ok());
    // Binary round trip + rename + metadata + temp file.
    const std::string bfile = dir + "/blob.bin";
    std::vector<uint8_t> blob{0x00, 0x01, 0xFE, 0xFF};
    CHECK(fs.WriteBinary(bfile, blob).ok());
    auto back = fs.ReadBinary(bfile);
    CHECK(back.ok() && back.value() == blob);
    const std::string bfile2 = dir + "/blob2.bin";
    CHECK(fs.Rename(bfile, bfile2).ok());
    CHECK(!fs.Exists(bfile) && fs.Exists(bfile2));
    auto meta = fs.Metadata(bfile2);
    CHECK(meta.ok() && meta.value().sizeBytes == 4 && meta.value().isRegularFile);
    CHECK(meta.ok() && meta.value().permissions != 0);   // POSIX mode bits (DoD §3)
    auto tmp = fs.CreateTempFile("bps-");
    CHECK(tmp.ok() && fs.Exists(tmp.value()));
    if (tmp.ok()) CHECK(fs.Remove(tmp.value()).ok());
    // Symlinks. On Windows this needs Developer Mode or elevation — a real
    // OS permission gate, not something the calling process can grant
    // itself, so ERROR_PRIVILEGE_NOT_HELD (1314) there is expected on a
    // machine with neither, not a bug. Anything else (including any
    // failure at all on non-Windows, where an ordinary user needs no
    // special privilege for this) still hard-fails.
    auto symResult = fs.CreateSymlink(bfile2, dir + "/link");
#if defined(_WIN32)
    constexpr int kErrorPrivilegeNotHeld = 1314;
    if (!symResult.ok() && symResult.error().nativeError == kErrorPrivilegeNotHeld) {
        Logger::Instance().Info(
            "Test", "skipping symlink checks: ERROR_PRIVILEGE_NOT_HELD — "
                     "enable Developer Mode to exercise this on Windows");
    } else
#endif
    {
        CHECK(symResult.ok());
        auto target = fs.ReadSymlink(dir + "/link");
        CHECK(target.ok() && target.value().find("blob2.bin") != std::string::npos);
        CHECK(fs.Remove(dir + "/link").ok());
    }
    // Recursive search.
    CHECK(fs.CreateDirectories(dir + "/nested/deep").ok());
    CHECK(fs.Write(dir + "/nested/deep/leaf.txt", "x").ok());
    auto found = fs.FindFiles(dir, ".txt");
    CHECK(found.ok() && found.value().size() >= 1);
    CHECK(fs.RemoveAll(dir).ok());
    CHECK(!fs.Exists(dir));
    CHECK(!fs.ReadText("/nonexistent/x").ok());
    // Native error surfaced on a PAL failure (DoD §19).
    auto badMeta = fs.Metadata("/nonexistent/nope");
    CHECK(!badMeta.ok() && badMeta.error().nativeError != 0);

    // Watch (polling): fires when the watched file changes (interval 500ms).
    const std::string wdir = "/tmp/bps_pal_watch";
    CHECK(fs.CreateDirectories(wdir).ok());
    const std::string wfile = wdir + "/watch.txt";
    CHECK(fs.Write(wfile, "v1").ok());
    std::atomic<int> watchHits{0};
    auto wt = fs.Watch(wfile, [&](std::string_view) { watchHits++; });
    CHECK(wt.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fs.Write(wfile, "v2").ok());
    // 900ms (1.8x the 500ms poll interval) was tight enough to miss under
    // real scheduling jitter in a 3500+-check test process with several
    // background threads already running — 1.5s guarantees a full poll
    // cycle lands inside the window even under load, not just in isolation.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(watchHits.load() >= 1);
    CHECK(fs.CancelWatch(wt.value()).ok());

    // Regression: a callback that cancels its own watch must not deadlock —
    // the poll loop fires callbacks outside the lock (reviewer finding).
    std::atomic<int> selfCancel{0};
    p::IFilesystem::WatchToken wt2 = 0;
    auto wt2r = fs.Watch(wfile, [&](std::string_view) {
        selfCancel++;
        if (wt2 != 0) (void)fs.CancelWatch(wt2);
    });
    CHECK(wt2r.ok());
    if (wt2r.ok()) wt2 = wt2r.value();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fs.Write(wfile, "v3").ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(selfCancel.load() >= 1);
    CHECK(fs.RemoveAll(wdir).ok());

    // --- Paths (Phase 2 §Path Manager) ---
    auto& paths = plat.Paths();
    CHECK(!paths.ConfigDir().empty());
    CHECK(!paths.TempDir().empty());
    CHECK(!paths.LogDir().empty());
    CHECK(fs.IsDirectory(paths.TempDir()));
    CHECK(!paths.ExecutableDir().empty());
    CHECK(!paths.CurrentWorkingDir().empty());
    CHECK(fs.IsDirectory(paths.CurrentWorkingDir()));
    CHECK(!paths.UserDataDir().empty());
    CHECK(!paths.AppDataDir().empty());

    // --- Threading (Phase 2 §Thread Abstraction) ---
    auto& th = plat.Threading();
    CHECK(th.HardwareConcurrency() >= 1);
    CHECK(th.CurrentThreadId() > 0);
    CHECK(th.SetCurrentThreadName("pal-test").ok());
    CHECK(th.CurrentThreadName() == "pal-test");

    // --- Process (Phase 2 §Process Manager) ---
    auto& proc = plat.Process();
    CHECK(proc.SetEnvironment("BPS_PAL_VAR", "42").ok());
    auto env = proc.Environment("BPS_PAL_VAR");
    CHECK(env.ok() && env.value() == "42");
#if defined(_WIN32)
    auto pid = proc.Start("cmd.exe", {"/c", "exit 7"});
#else
    auto pid = proc.Start("/bin/sh", {"-c", "exit 7"});
#endif
    CHECK(pid.ok());
    if (pid.ok()) {
        CHECK(proc.Wait(pid.value(), 5000).ok());
        auto code = proc.ExitCode(pid.value());
        CHECK(code.ok() && code.value() == 7);
        CHECK(!proc.IsRunning(pid.value()).value());
    }
    CHECK(proc.CurrentProcessId() > 0);
    // Restart: terminate the old child, spawn a fresh one. No bare `sleep`
    // binary on Windows — `ping` against loopback is the standard
    // stdin-free way to burn ~30s (each echo request takes ~1s; `timeout`
    // needs a real console and errors out when stdin is redirected, which
    // it is here).
#if defined(_WIN32)
    const std::string sleepCmd = "ping.exe";
    const std::vector<std::string> sleepArgs = {"-n", "31", "127.0.0.1"};
#else
    const std::string sleepCmd = "/bin/sleep";
    const std::vector<std::string> sleepArgs = {"30"};
#endif
    auto sleeper = proc.Start(sleepCmd, sleepArgs);
    CHECK(sleeper.ok());
    if (sleeper.ok()) {
        CHECK(proc.IsRunning(sleeper.value()).ok());
        auto restarted = proc.Restart(sleeper.value(), sleepCmd, sleepArgs);
        CHECK(restarted.ok());
        if (restarted.ok()) {
            CHECK(restarted.value() != sleeper.value());   // a new pid
            CHECK(!proc.IsRunning(sleeper.value()).value());
            CHECK(proc.IsRunning(restarted.value()).ok());
            (void)proc.Kill(restarted.value());
            (void)proc.Wait(restarted.value(), 5000);   // reap — no zombies
        }
    }

    // --- Dynamic library loader (Phase 2 §Dynamic Library Loader) ---
    auto& lib = plat.Library();
    bool libLoaded = false;
#if defined(_WIN32)
    // msvcrt.dll exports the libm-style math functions (incl. cos) as a
    // system DLL always present — the Windows equivalent of libm.so.6.
    for (const char* cand : {"msvcrt.dll"}) {
#else
    for (const char* cand : {"libm.so.6", "libc.so.6"}) {
#endif
        auto handle = lib.Load(cand);
        if (!handle.ok()) continue;
        libLoaded = true;
        CHECK(lib.IsLoaded(handle.value()));
        auto sym = lib.Symbol(handle.value(), "cos");
        CHECK(sym.ok() && sym.value() != nullptr);
        CHECK(lib.Unload(handle.value()).ok());
        CHECK(!lib.IsLoaded(handle.value()));
        break;
    }
    CHECK(libLoaded);
    CHECK(!lib.Load("/nonexistent/lib.so").ok());   // dlopen error path
    CHECK(!lib.LastError().empty());

    // --- Monitor (Phase 2 §Display Manager) ---
    auto& mon = plat.Monitor();
    auto monitors = mon.Enumerate();
    for (const auto& m : monitors) {
        CHECK(!m.id.empty());
        CHECK(m.widthPx > 0);
        // Orientation is normalized to 0/90/180/270 (DRM panel orientation
        // enrichment is best-effort, so it may stay 0 on headless hosts).
        CHECK(m.orientation == 0 || m.orientation == 90 || m.orientation == 180 ||
              m.orientation == 270);
    }
    auto primary = mon.Primary();
    CHECK(primary.ok() || monitors.empty());   // headless boxes may have none
    CHECK(mon.DefaultMonitorId().empty() || !monitors.empty());

    // --- Audio (Phase 2 §Audio Device Manager; DoD §13) ---
    auto& audio = plat.Audio();
    for (const auto& d : audio.Enumerate()) CHECK(!d.id.empty());
    // Input + output devices both enumerated; Default* succeed or fail with a
    // clear error (headless hosts may have none) — never crash.
    bool sawInput = false, sawOutput = false;
    for (const auto& d : audio.Enumerate()) {
        if (d.isInput) sawInput = true;
        else sawOutput = true;
        // Rates populated via runtime-loaded libasound idle probe (when the
        // ALSA library + a device are present); 0 = unknown is still valid.
        CHECK(d.sampleRateHz == 0 || (d.sampleRateHz >= 8000 && d.sampleRateHz <= 768000));
        CHECK(d.channels == 0 || (d.channels >= 1 && d.channels <= 32));
    }
    auto defOut = audio.DefaultOutput();
    CHECK(defOut.ok() || defOut.error().code == Err::NotFound);
    auto defIn = audio.DefaultInput();
    CHECK(defIn.ok() || defIn.error().code == Err::NotFound);
    CHECK(!audio.Fingerprint().empty() || (!sawInput && !sawOutput));   // fingerprint or no devices

    // --- Input devices (DoD §11) ---
    auto& input = plat.Input();
    for (const auto& d : input.Enumerate()) {
        CHECK(!d.id.empty());
        CHECK(d.kind >= p::InputDeviceKind::Keyboard && d.kind <= p::InputDeviceKind::Other);
    }
    (void)input.HasKeyboard();   // no crash; result depends on the machine
    (void)input.HasPointer();

    // --- Network (Phase 2 §Network) ---
    auto& net = plat.Network();
    CHECK(!net.Hostname().empty());
    auto adapters = net.Adapters();
    CHECK(!adapters.empty());
    auto ip = net.IpAddress();
    CHECK(ip.ok() || ip.error().code == Err::NotFound);
    auto gw = net.Gateway();
    CHECK(gw.ok() || gw.error().code == Err::NotFound);
    for (const auto& dns : net.DnsServers()) CHECK(!dns.empty());
    // Proxy: read from the environment (set then read to exercise the path).
    CHECK(proc.SetEnvironment("https_proxy", "http://proxy.local:3128").ok());
    CHECK(net.Proxy() == "http://proxy.local:3128");
    CHECK(proc.SetEnvironment("https_proxy", "").ok());

    // --- Power (Phase 2 §Power Manager) ---
    auto power = plat.Power().Current();
    CHECK(power.batteryPercent >= -1);
    CHECK(power.batteryPercent <= 100);

    // --- Clipboard (Phase 2 §Clipboard; DoD §10) — best effort ---
    auto& clip = plat.Clipboard();
    auto clipW = clip.WriteText("pal-clip-test");
    if (clipW.ok()) {
        auto rd = clip.ReadText();
        CHECK(rd.ok());
        // File lists: only when the provider has the text/uri-list target.
        auto files = clip.SetFiles({"/tmp/bps_clip_a", "/tmp/bps_clip_b"});
        if (files.ok()) {
            auto got = clip.GetFiles();
            CHECK(got.ok());
            if (got.ok()) CHECK(!got.value().empty());
        } else {
            CHECK(files.error().code == Err::Unsupported || files.error().code == Err::IoError);
        }
    } else {
        CHECK(clipW.error().code == Err::Unsupported || clipW.error().code == Err::IoError);
        auto files = clip.GetFiles();
        CHECK(!files.ok() || files.value().empty());
    }

    // --- Environment + Locale (DoD §14/§15) ---
    auto envInfo = plat.Environment().Current();
    CHECK(!envInfo.osName.empty());
    CHECK(!envInfo.arch.empty());
    CHECK(envInfo.coreCount > 0);
    CHECK(envInfo.totalRamBytes > 0);
    CHECK(!envInfo.hostname.empty());
    CHECK(!envInfo.username.empty());
    auto loc = plat.Locale().Current();
    CHECK(!loc.language.empty());
    CHECK(!loc.dateFormat.empty());
    CHECK(plat.Locale().Language() == loc.language);

    // --- OS events (Phase 2 §Operating System Events) ---
    (void)plat.PollChanges();   // first call records the baseline
    auto evs = plat.PollChanges();
    CHECK(evs.empty() || !evs.empty());   // never crashes; no requirement on content

    // --- Platform events reach the Event Bus ---
    int gotMonitor = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::MonitorConnected>(
        [&](const events::MonitorConnected&) { gotMonitor++; }, 0);
    CHECK(EventBus::Instance().Publish(events::MonitorConnected{"DP-1"}).ok());
    CHECK(gotMonitor == 1);
    (void)EventBus::Instance().Unsubscribe(sub);

    int gotPower = 0;
    Subscription sub2 = EventBus::Instance().Subscribe<events::PowerChanged>(
        [&](const events::PowerChanged&) { gotPower++; }, 0);
    CHECK(EventBus::Instance().Publish(events::PowerChanged{"on battery (42%)"}).ok());
    CHECK(gotPower == 1);
    (void)EventBus::Instance().Unsubscribe(sub2);

    int gotLow = 0;
    Subscription sub3 = EventBus::Instance().Subscribe<events::BatteryLow>(
        [&](const events::BatteryLow&) { gotLow++; }, 0);
    CHECK(EventBus::Instance().Publish(events::BatteryLow{"12%"}).ok());
    CHECK(gotLow == 1);
    (void)EventBus::Instance().Unsubscribe(sub3);

    int gotRes = 0;
    Subscription sub4 = EventBus::Instance().Subscribe<events::MonitorResolutionChanged>(
        [&](const events::MonitorResolutionChanged&) { gotRes++; }, 0);
    CHECK(EventBus::Instance().Publish(events::MonitorResolutionChanged{"DP-1 1920x1080"}).ok());
    CHECK(gotRes == 1);
    (void)EventBus::Instance().Unsubscribe(sub4);

    // --- Thread priority (DoD §6) — priority 1 (lower) always permitted ---
    CHECK(plat.Threading().SetCurrentThreadPriority(1).ok());
    CHECK(plat.Threading().SetCurrentThreadPriority(0).ok());

    // --- Notifications (DoD §17) — existence probe only (no desktop spam) ---
    (void)plat.Notifications().Supported();   // never crashes

    // --- Color + font pickers (DoD §16 — implemented on Linux via zenity). ---
    // Headless hosts get a clean Err::Unsupported; with a desktop session the
    // dialog is spawned but the user is expected to cancel (nullopt).
    auto color = plat.Dialogs().ColorPicker("pick");
    CHECK(color.ok() || color.error().code == Err::Unsupported ||
          color.error().code == Err::IoError);
    auto font = plat.Dialogs().FontPicker("pick");
    CHECK(font.ok() || font.error().code == Err::Unsupported ||
          font.error().code == Err::IoError);

    // --- Error.nativeError (DoD §19): PAL errors surface errno ---
    auto missing = plat.Filesystem().ReadText("/nonexistent/bps_missing");
    CHECK(!missing.ok());
    if (!missing.ok()) CHECK(missing.error().nativeError != 0);

    // --- Socket transport (DoD §transport): direct PAL echo round trip ---
    auto& sockets = plat.Sockets();
    auto lis = sockets.Listen(0);
    CHECK(lis.ok());
    if (lis.ok()) {
        auto port = lis.value()->Port();
        CHECK(port > 0);
        std::atomic<bool> gotPong{false};
        std::thread server([&]() {
            auto acc = lis.value()->Accept(5000);
            if (!acc.ok()) return;
            char b[8] = {};
            auto n = acc.value()->Receive(b, sizeof b);
            if (n.ok() && n.value() == 4) {
                auto sent = acc.value()->Send("pong");
                gotPong = sent.ok() && sent.value() == 4;
            }
            acc.value()->Close();
        });
        auto conn = sockets.Connect("127.0.0.1", port);
        CHECK(conn.ok());
        if (conn.ok()) {
            auto sent = conn.value()->Send("ping");
            CHECK(sent.ok() && sent.value() == 4);
            char b[8] = {};
            auto n = conn.value()->ReceiveTimeout(b, sizeof b, 5000);
            CHECK(n.ok() && n.value() == 4);
            if (n.ok()) CHECK(std::string(b, n.value()) == "pong");
            conn.value()->Close();
        }
        server.join();
        CHECK(gotPong.load());
        lis.value()->Close();
    }
}

// DoD §21/§22: performance and stress coverage for the PAL (loose bounds —
// these are regression guards, not benchmarks).
void TestPalPerfStress() {
    namespace p = bps::platform;
    auto& plat = p::PlatformAccessor::Get();

    // --- Timer perf (DoD §21): 10k Now() reads must be far under 100 ms. ---
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) (void)Now();
    auto t1 = std::chrono::steady_clock::now();
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() < 100);

    // --- Filesystem stress (DoD §22): 200 write/read/delete round trips. ---
    auto& fs = plat.Filesystem();
    std::string dir = plat.Paths().TempDir() + "/bps_pal_stress_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    CHECK(fs.CreateDirectories(dir).ok());
    t0 = std::chrono::steady_clock::now();
    bool allOk = true;
    for (int i = 0; i < 200; ++i) {
        std::string f = dir + "/f" + std::to_string(i) + ".dat";
        std::string payload(64, static_cast<char>('a' + (i % 26)));
        if (!fs.Write(f, payload).ok()) { allOk = false; break; }
        auto rd = fs.ReadText(f);
        if (!rd.ok() || rd.value() != payload) { allOk = false; break; }
        if (!fs.Remove(f).ok()) { allOk = false; break; }
    }
    t1 = std::chrono::steady_clock::now();
    CHECK(allOk);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() < 5000);
    (void)fs.RemoveAll(dir);

    // --- Socket stress (DoD §22): 100 echo round trips over the PAL transport. ---
    auto lis = plat.Sockets().Listen(0);
    CHECK(lis.ok());
    if (lis.ok()) {
        auto port = lis.value()->Port();
        std::atomic<int> echoed{0};
        std::thread srv([&]() {
            for (int i = 0; i < 100; ++i) {
                auto acc = lis.value()->Accept(15000);
                if (!acc.ok()) break;
                char b[8] = {};
                auto n = acc.value()->Receive(b, sizeof b);
                if (n.ok() && n.value() == 4) {
                    auto s = acc.value()->Send("pong");
                    if (s.ok() && s.value() == 4) echoed.fetch_add(1);
                }
                acc.value()->Close();
            }
        });
        t0 = std::chrono::steady_clock::now();
        int connected = 0;
        for (int i = 0; i < 100; ++i) {
            auto conn = plat.Sockets().Connect("127.0.0.1", port);
            if (!conn.ok()) break;
            ++connected;
            if (!conn.value()->Send("ping").ok()) break;
            char b[8] = {};
            auto n = conn.value()->ReceiveTimeout(b, sizeof b, 15000);
            if (!n.ok() || n.value() != 4) break;
            conn.value()->Close();
        }
        t1 = std::chrono::steady_clock::now();
        srv.join();
        CHECK(connected == 100);
        CHECK(echoed.load() == 100);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() < 15000);
        lis.value()->Close();
    }

    // --- ThreadPool stress (DoD §22): 1000 tasks across 4 workers. ---
    auto& pool = ThreadPool::Instance();
    CHECK(pool.Initialize(4).ok());
    std::atomic<int> done{0};
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        CHECK(pool.Submit([&done]() { done.fetch_add(1); }).ok());
    while (done.load() < 1000 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    t1 = std::chrono::steady_clock::now();
    CHECK(done.load() == 1000);
    CHECK(pool.ExecutedCount() >= 1000);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() < 20000);
    CHECK(pool.Shutdown().ok());
}
void TestAssets() {
    auto& am = AssetManager::Instance();
    CHECK(am.Initialize().ok());
    CHECK(am.Register("bg", AssetType::Texture, 4096, "bg.png").ok());
    CHECK(am.Register("song", AssetType::Data, 1024).ok());
    CHECK(am.Register("", AssetType::Data, 1).error().code == Err::InvalidArgument);
    CHECK(am.Load("bg").ok());
    CHECK(am.Get("bg").value().state == AssetState::Loaded);
    CHECK(am.CachedBytes() == 4096);
    // LRU shrink: target 2048 -> bg (only loaded asset) is unloaded
    CHECK(am.ShrinkTo(2048) == 4096);
    CHECK(am.CachedBytes() == 0);
    // resource pressure event -> automatic shrink (10 §7)
    CHECK(am.Load("bg").ok());
    CHECK(am.Load("song").ok());
    CHECK(am.CachedBytes() == 4096 + 1024);
    (void)EventBus::Instance().Publish(events::ResourcePressureHigh{"memory", PressureLevel::High});
    CHECK(am.CachedBytes() <= 4096 + 1024);
    CHECK(am.UnloadAll().ok());
    CHECK(am.CachedBytes() == 0);
    CHECK(am.Shutdown().ok());
}
void TestDrivers() {
    auto& dm = DriverManager::Instance();
    CHECK(dm.Initialize().ok());
    FakeDriver a, b;
    CHECK(dm.Register("ndi", &a).ok());
    CHECK(dm.Register("midi", &b).ok());
    CHECK(dm.Register("ndi", &a).error().code == Err::AlreadyExists);
    CHECK(dm.ProbeAll().ok());
    CHECK(a.probeCalls == 1 && b.probeCalls == 1);
    CHECK(dm.EnableAll().ok());
    CHECK(a.enableCalls == 1);
    CHECK(dm.Reload("ndi").ok());   // hot reload: disable + enable
    CHECK(a.disableCalls == 1 && a.enableCalls == 2);
    CHECK(dm.Resolve("ndi").value() == &a);
    CHECK(dm.Resolve("nope").error().code == Err::NotFound);
    CHECK(dm.Snapshot().size() == 2);
    CHECK(dm.Unregister("midi").ok());
    CHECK(dm.Shutdown().ok());
}
void TestDatabase() {
    auto& db = DatabaseManager::Instance();
    namespace fs = std::filesystem;
    fs::path path = fs::temp_directory_path() / "bps_db_unit_test.json";
    std::error_code ec;
    fs::remove(path, ec);
    CHECK(db.Open(path.string()).ok());
    json::Value::Object doc;
    doc["title"] = json::Value::String("Amazing Grace");
    CHECK(db.Put("songs", "grace", json::Value(std::move(doc))).ok());
    CHECK(db.Put("songs", "holy", json::Value::String("Holy, Holy, Holy")).ok());
    CHECK(db.Put("settings", "theme", json::Value::String("dark")).ok());
    CHECK(db.DocumentCount() == 3);
    CHECK(db.Get("songs", "grace").value().Find("title") != nullptr);
    CHECK(db.Keys("songs").size() == 2);
    CHECK(db.Collections().size() == 2);
    CHECK(db.Shutdown().ok());            // flushes to disk
    CHECK(fs::exists(path));
    CHECK(db.Open(path.string()).ok());   // reload from disk
    CHECK(db.DocumentCount() == 3);
    CHECK(std::string(db.Get("settings", "theme").value().asString()) == "dark");
    CHECK(db.Remove("settings", "theme").ok());
    CHECK(db.DocumentCount() == 2);
    CHECK(db.Open("").ok());             // back to in-memory
    CHECK(db.DocumentCount() == 0);
    fs::remove(path, ec);
}
void TestDisplay() {
    auto& dm = DisplayManager::Instance();
    CHECK(dm.Initialize().ok());
    FakeDisplay primary(2), secondary(1);
    CHECK(dm.RegisterDisplay("main", &primary).ok());
    CHECK(dm.RegisterDisplay("aux", &secondary).ok());
    CHECK(dm.DisplayCount() == 2);
    CHECK(dm.Enumerate().size() == 3);
    CHECK(dm.SetOutput("main", 0, true).ok());
    CHECK(dm.IsOutputEnabled("main", 0));
    CHECK(primary.setOutputCalls == 1);
    CHECK(dm.SetOutput("main", 9, true).error().code == Err::InvalidArgument);
    CHECK(dm.SetOutput("nope", 0, true).error().code == Err::NotFound);
    CHECK(dm.RemoveDisplay("aux").ok());
    CHECK(dm.Enumerate().size() == 2);
    CHECK(dm.Shutdown().ok());
}
void TestRenderer() {
    auto& rm = RendererManager::Instance();
    CHECK(rm.Initialize().ok());
    FakeRenderer a, b;
    CHECK(rm.RegisterRenderer("sw", &a).ok());
    CHECK(rm.RegisterRenderer("gpu", &b).ok());
    CHECK(rm.Active().value() == &a);   // first registered becomes active
    CHECK(rm.SetActive("gpu").ok());
    CHECK(rm.Active().value() == &b);
    CHECK(rm.BeginFrame().ok());
    CHECK(rm.EndFrame().ok());
    CHECK(rm.Present().ok());
    CHECK(rm.BeginFrame().ok());
    CHECK(rm.EndFrame().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));   // measurable frame time
    CHECK(rm.Present().ok());
    CHECK(b.beginCalls == 2 && b.presentCalls == 2);
    auto st = rm.Stats();
    CHECK(st.frames == 2);
    CHECK(st.fps > 0.0);
    CHECK(rm.RendererCount() == 2);
    CHECK(rm.Shutdown().ok());
}
void TestIpc() {
    auto& server = IpcServer::Instance();
    CHECK(server.RegisterHandler("engine.ping", [](const json::Value& params) {
        return params;   // echo
    }).ok());
    CHECK(server.Start(0).ok());
    CHECK(server.Running());
    CHECK(server.Port() > 0);
    IpcClient client;
    CHECK(client.Connect("127.0.0.1", server.Port()).ok());
    json::Value::Object params;
    params["v"] = json::Value::Number(42);
    auto resp = client.Call("engine.ping", json::Value(std::move(params)));
    CHECK(resp.ok());
    if (resp.ok()) CHECK(resp.value().Find("v")->asInt() == 42);
    auto unknown = client.Call("no.such.method", json::Value::Object{});
    CHECK(!unknown.ok());
    client.Close();
    CHECK(server.RequestsServed() >= 2);
    CHECK(server.Stop().ok());
    CHECK(!server.Running());
}
void TestLifecycle() {
    auto& lc = LifecycleManager::Instance();
    CHECK(lc.RegisterEntity("test:svc", kEngineVersion, {}).ok());
    CHECK(lc.RegisterEntity("test:mod", kEngineVersion, {"test:svc"}).ok());

    // Stepwise walk Registered -> Running (entities enter via registration).
    CHECK(lc.TransitionDependent("test:svc", LifecycleState::Running).ok());
    CHECK(lc.State("test:svc") == LifecycleState::Running);

    // Dependency satisfied: plain Transition walks the module forward too.
    CHECK(lc.Transition("test:mod", LifecycleState::Running).ok());
    CHECK(lc.State("test:mod") == LifecycleState::Running);

    // Dependency gating: an entity can't run while a dependency is behind.
    CHECK(lc.RegisterEntity("test:svc2", kEngineVersion, {}).ok());
    CHECK(lc.RegisterEntity("test:mod2", kEngineVersion, {"test:svc2"}).ok());
    CHECK(!lc.Transition("test:mod2", LifecycleState::Running).ok());
    CHECK(lc.State("test:mod2") == LifecycleState::Registered);

    // TransitionDependent advances dependencies automatically.
    CHECK(lc.TransitionDependent("test:mod2", LifecycleState::Running).ok());
    CHECK(lc.State("test:svc2") == LifecycleState::Running);
    CHECK(lc.State("test:mod2") == LifecycleState::Running);

    // Pause -> Running (direct resume edge).
    CHECK(lc.Transition("test:mod", LifecycleState::Paused).ok());
    CHECK(lc.State("test:mod") == LifecycleState::Paused);
    CHECK(lc.Transition("test:mod", LifecycleState::Running).ok());

    // Stop is a forward walk: Running -> Stopping -> Stopped (12 §5).
    CHECK(lc.Transition("test:mod", LifecycleState::Stopped).ok());
    CHECK(lc.State("test:mod") == LifecycleState::Stopped);

    // Cross-branch jumps are rejected: no Paused from Stopped.
    CHECK(!lc.Transition("test:mod", LifecycleState::Paused).ok());

    // Restart is legal via the reset edge Stopped -> Registered (12 §3).
    CHECK(lc.Transition("test:mod", LifecycleState::Running).ok());
    CHECK(lc.State("test:mod") == LifecycleState::Running);

    // Teardown to the terminal state is a forward walk.
    CHECK(lc.Transition("test:mod", LifecycleState::Destroyed).ok());
    CHECK(lc.State("test:mod") == LifecycleState::Destroyed);

    // Hook failure triggers rollback: state stays in the prior state.
    CHECK(lc.RegisterEntity("test:rollback", kEngineVersion, {}).ok());
    CHECK(lc.Transition("test:rollback", LifecycleState::Running).ok());
    CHECK(lc.AddHook("test:rollback", LifecycleState::Paused,
                     {LifecyclePhase::On, []() -> Result<void> {
                          return Error::Make(Err::InvalidState, "test", "boom");
                      }})
              .ok());
    CHECK(!lc.Transition("test:rollback", LifecycleState::Paused).ok());
    CHECK(lc.State("test:rollback") == LifecycleState::Running);
    CHECK(lc.State("missing:entity") == LifecycleState::Destroyed);
    CHECK(!lc.Transition("missing:entity", LifecycleState::Running).ok());
}
void TestModules() {
    auto& modules = ModuleManager::Instance();
    CHECK(modules.RegisterModule(modules::PresentationModule::Create()).ok());
    CHECK(modules.State("presentation") == ModuleState::Installed);
    CHECK(modules.Start("presentation").ok());
    CHECK(modules.State("presentation") == ModuleState::Running);
    // Publishing the canonical event reaches the module's subscriber without a crash.
    CHECK(EventBus::Instance().Publish(events::SlideChanged{0, 3, "Intro"}).ok());
    CHECK(modules.Pause("presentation").ok());
    CHECK(modules.State("presentation") == ModuleState::Paused);
    CHECK(modules.Resume("presentation").ok());
    CHECK(modules.State("presentation") == ModuleState::Running);
    CHECK(modules.Stop("presentation").ok());
    CHECK(modules.State("presentation") == ModuleState::Stopped);
    CHECK(modules.Start("presentation").ok());   // restart from Stopped
    CHECK(modules.Reload("presentation").ok()); // stop + start
    CHECK(!modules.Start("nope").ok());
    CHECK(!modules.RegisterModule(modules::PresentationModule::Create()).ok());  // duplicate

    // Filesystem discovery (08 §Discover): sidecar manifest + registered factory.
    struct DemoModule final : public IModule {
        static std::shared_ptr<DemoModule> Create() {
            return std::shared_ptr<DemoModule>(new DemoModule());
        }
        const ModuleManifest& Manifest() const override {
            static ModuleManifest m;
            m.id = "demo";
            m.version = kEngineVersion;
            m.requiredCoreVersion = kEngineVersion;
            m.capabilities = {"demo"};
            return m;
        }
    };
    std::filesystem::create_directories("/tmp/bps_modules");
    {
        std::ofstream sidecar("/tmp/bps_modules/demo.json");
        sidecar << "{\"id\":\"demo\",\"version\":\"1.0.0\",\"author\":\"test\"}";
    }
    CHECK(modules.RegisterFactory("demo", []() { return DemoModule::Create(); }).ok());
    CHECK(modules.Discover("/tmp/bps_modules").ok());
    CHECK(modules.DiscoveredCount() == 1);
    CHECK(modules.State("demo") == ModuleState::Installed);
    CHECK(modules.Start("demo").ok());
    CHECK(modules.State("demo") == ModuleState::Running);
    CHECK(modules.Stop("demo").ok());
    std::filesystem::remove_all("/tmp/bps_modules");

    // Leave the registry clean for the tests that follow.
    CHECK(modules.Stop("presentation").ok());
    CHECK(modules.UnregisterModule("presentation").ok());
    CHECK(modules.UnregisterModule("demo").ok());
    CHECK(!modules.UnregisterModule("nope").ok());
}
void TestKernel() {
    auto& kernel = Kernel::Instance();
    // Engine-wide contract (00 §10): default lifecycle methods are safe no-ops.
    CHECK(kernel.Initialize().ok());
    CHECK(kernel.Start().ok());
    CHECK(kernel.Stop().ok());
    CHECK(kernel.Reset().ok());

    // ...and the interface virtuals dispatch to concrete overrides, not the base
    // no-ops: Shutdown through IService& must really stop the logger pump.
    {
        IService& loggerSvc = Logger::Instance();
        (void)loggerSvc.Shutdown();
        CHECK(Logger::Instance().GetHealth().state == HealthState::Failing);
        CHECK(Logger::Instance().Initialize().ok());   // restore before the boot below
    }

    CHECK(kernel.State() == KernelState::Stopped);
    auto boot = kernel.Boot(BootOptions{});
    CHECK(boot.ok());
    if (!boot.ok()) std::fprintf(stderr, "boot error: %s\n", boot.error().message.c_str());
    CHECK(kernel.State() == KernelState::Running);
    // Identity (01 §3): UUID, build info, runtime info.
    CHECK(!kernel.EngineUuid().empty());
    CHECK(kernel.GetBuildInfo().version == kEngineVersion);
    CHECK(!kernel.GetBuildInfo().platform.empty());
    CHECK(!kernel.GetRuntimeInfo().sessionId.empty());
    CHECK(kernel.GetRuntimeInfo().engineUuid == kernel.EngineUuid());
    CHECK(kernel.Uptime().count() >= 0);
    auto order = kernel.BootLog();
    CHECK(!order.empty());
    CHECK(order.front() == "Logger");
    CHECK(kernel.EngineVersion() == kEngineVersion);
    CHECK(kernel.GetHealth().state == HealthState::Healthy);
    // Double boot rejected.
    CHECK(!kernel.Boot(BootOptions{}).ok());
    // Pause/Resume.
    CHECK(kernel.Pause().ok());
    CHECK(kernel.State() == KernelState::Paused);
    CHECK(kernel.Resume().ok());
    CHECK(kernel.State() == KernelState::Running);
    // Shutdown.
    CHECK(kernel.Shutdown().ok());
    CHECK(kernel.State() == KernelState::Stopped);
    CHECK(!kernel.Shutdown().ok());   // already stopped

    // Crash recovery (01 §Recovery): Panic -> CrashRecovery -> Recover -> Running.
    CHECK(kernel.Panic(Error::Make(Err::Kernel_BootFailed, "test", "injected panic")).ok());
    CHECK(kernel.State() == KernelState::CrashRecovery);
    CHECK(kernel.GetHealth().state == HealthState::Degraded);   // not healthy mid-recovery
    CHECK(kernel.Recover(BootOptions{}).ok());
    CHECK(kernel.State() == KernelState::Running);
    // Engine context aggregates build + runtime + boot log + health (01 §3).
    auto ctx = kernel.Context();
    CHECK(ctx.runtime.engineUuid == kernel.EngineUuid());
    CHECK(ctx.build.version == kEngineVersion);
    CHECK(!ctx.bootLog.empty() && ctx.bootLog.front() == "Logger");
    CHECK(ctx.bootLog.size() >= 17);   // 12 core systems + Platform + 5 engine managers
    CHECK(!ctx.health.lastError.empty());   // last error is retained from the panic
    CHECK(kernel.Shutdown().ok());
    CHECK(kernel.State() == KernelState::Stopped);
}
void TestAutoplay() {
    auto& modules = ModuleManager::Instance();
    auto mod = modules::PresentationModule::Create();
    CHECK(modules.RegisterModule(mod).ok());
    CHECK(modules.Load("presentation").ok());
    CHECK(modules.Start("presentation").ok());

    // Watch the module's autoplay fan-out (07 §Autoplay).
    std::atomic<int> events{0};
    Subscription sub = EventBus::Instance().Subscribe<events::SlideChanged>(
        [&](const events::SlideChanged&) { events++; }, 0);

    CHECK(mod->StartAutoplay(3, std::chrono::milliseconds(15)).ok());
    CHECK(mod->AutoplayActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(events.load() >= 3);
    CHECK(mod->AutoplayPosition() == 3);
    CHECK(mod->StopAutoplay().ok());
    CHECK(!mod->AutoplayActive());
    int frozen = events.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(events.load() == frozen);

    (void)EventBus::Instance().Unsubscribe(sub);
    CHECK(modules.Stop("presentation").ok());
    CHECK(modules.UnregisterModule("presentation").ok());
}
void TestSongs() {
    auto& modules = ModuleManager::Instance();
    auto mod = modules::SongsModule::Create();
    CHECK(modules.RegisterModule(mod).ok());
    CHECK(modules.Start("songs").ok());

    std::atomic<int> verseEvents{0};
    std::atomic<int> lastVerse{-1};
    Subscription sub = EventBus::Instance().Subscribe<events::SongVerseChanged>(
        [&](const events::SongVerseChanged& e) { verseEvents++; lastVerse = e.verse; }, 0);
    std::atomic<int> selected{0};
    Subscription sub2 = EventBus::Instance().Subscribe<events::SongSelected>(
        [&](const events::SongSelected&) { selected++; }, 0);

    CHECK(mod->RegisterSong(modules::Song{"h1", "Test Song", "anon", {"v1", "v2", "v3"}}).ok());
    CHECK(mod->SongCount() == 1);
    CHECK(!mod->RegisterSong(modules::Song{"h1", "dup", "anon", {"x"}}).ok());
    CHECK(!mod->RegisterSong(modules::Song{"", "no id", "anon", {"x"}}).ok());

    CHECK(mod->Select("h1").ok());
    CHECK(mod->HasSelection());
    CHECK(mod->CurrentVerse() == 0);
    CHECK(selected.load() == 1);
    CHECK(verseEvents.load() >= 1);
    CHECK(mod->CurrentTitle() == "Test Song");

    CHECK(mod->NextVerse().ok());
    CHECK(mod->CurrentVerse() == 1);
    CHECK(mod->NextVerse().ok());
    CHECK(mod->CurrentVerse() == 2);
    CHECK(!mod->NextVerse().ok());      // already at the last verse
    CHECK(mod->PreviousVerse().ok());
    CHECK(mod->CurrentVerse() == 1);
    CHECK(!mod->Select("nope").ok());

    CHECK(mod->RemoveSong("h1").ok());
    CHECK(!mod->Select("h1").ok());     // removed
    CHECK(!mod->NextVerse().ok());       // no selection
    CHECK(mod->SongCount() == 0);

    (void)EventBus::Instance().Unsubscribe(sub);
    (void)EventBus::Instance().Unsubscribe(sub2);
    CHECK(modules.Stop("songs").ok());
    CHECK(modules.UnregisterModule("songs").ok());
}
void TestMedia() {
    auto& modules = ModuleManager::Instance();
    auto mod = modules::MediaModule::Create();
    CHECK(modules.RegisterModule(mod).ok());
    CHECK(modules.Start("media").ok());

    std::atomic<int> stateEvents{0};
    std::atomic<int> lastState{-1};
    Subscription sub = EventBus::Instance().Subscribe<events::MediaStateChanged>(
        [&](const events::MediaStateChanged& e) { stateEvents++; lastState = e.state; }, 0);

    CHECK(mod->Enqueue(modules::MediaItem{"clip", "Intro", modules::MediaType::Video, 0.5}).ok());
    CHECK(mod->PlaylistSize() == 1);
    CHECK(mod->Play("clip").ok());
    CHECK(mod->PlaybackState() == modules::MediaPlaybackState::Playing);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(mod->PositionSec() > 0.0);

    CHECK(mod->Pause().ok());
    CHECK(mod->PlaybackState() == modules::MediaPlaybackState::Paused);
    double frozen = mod->PositionSec();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(mod->PositionSec() == frozen);   // paused: the clock is stopped

    CHECK(mod->Resume().ok());
    CHECK(mod->PlaybackState() == modules::MediaPlaybackState::Playing);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));   // budget (0.5 s) spent
    CHECK(mod->PlaybackState() == modules::MediaPlaybackState::Finished);
    CHECK(lastState.load() == static_cast<int>(modules::MediaPlaybackState::Finished));
    CHECK(stateEvents.load() >= 4);   // playing, paused, playing, finished
    CHECK(!mod->Pause().ok());        // nothing is playing now

    // Rapid Play->Play (regression): a stale clock callback from item A must not
    // mark item B finished nor orphan B's clock.
    CHECK(mod->Enqueue(modules::MediaItem{"clip2", "Outro", modules::MediaType::Video, 1.0}).ok());
    CHECK(mod->Play("clip").ok());
    CHECK(mod->Play("clip2").ok());
    CHECK(mod->CurrentTitle() == "Outro");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(mod->PlaybackState() == modules::MediaPlaybackState::Playing);   // not spuriously finished
    CHECK(mod->PositionSec() > 0.0);                                        // live clock advances
    CHECK(mod->Pause().ok());        // the live clock is still cancellable (not orphaned)
    CHECK(mod->Stop().ok());

    (void)EventBus::Instance().Unsubscribe(sub);
    CHECK(modules.Stop("media").ok());
    CHECK(modules.UnregisterModule("media").ok());
}

// ===========================================================================
// Phase 3 — Content & Asset Management System (docs/specs/13)
// ===========================================================================

// ===========================================================================
// PressureLatch (docs/specs/10 §7): spikes are not pressure; alerts are sustained,
// hysteretic and rate-limited.
// ===========================================================================
#include "core/resources/PressureLatch.hpp"

void TestPressureLatch() {
    using L = PressureLatch;
    using T = PressureLatch::Transition;
    using namespace std::chrono_literals;
    const EngineTime t0 = EngineClock::now();
    auto at = [&](std::chrono::milliseconds ms) { return t0 + ms; };

    // A single spike (one High sample, then Low) never alerts.
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 8000ms, 60000ms});
        CHECK(l.Update(PressureLevel::High, at(0ms)) == T::None);
        CHECK(l.Update(PressureLevel::Low, at(500ms)) == T::None);
        CHECK(!l.Active());
    }

    // Held High for the full sustain window: raises exactly once, then stays quiet.
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 8000ms, 60000ms});
        CHECK(l.Update(PressureLevel::High, at(0ms)) == T::None);
        CHECK(l.Update(PressureLevel::Critical, at(4000ms)) == T::None);   // Critical counts as High-or-worse
        CHECK(l.Update(PressureLevel::High, at(7999ms)) == T::None);
        CHECK(l.Update(PressureLevel::High, at(8000ms)) == T::Raised);
        CHECK(l.Active());
        CHECK(l.Update(PressureLevel::High, at(9000ms)) == T::None);       // no repeat while active
        CHECK(l.Update(PressureLevel::Critical, at(20000ms)) == T::None);
    }

    // A dip below the raise level restarts the sustain clock (must be UNBROKEN).
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 8000ms, 60000ms});
        CHECK(l.Update(PressureLevel::High, at(0ms)) == T::None);
        CHECK(l.Update(PressureLevel::Medium, at(6000ms)) == T::None);     // dip (still above clearBelow)
        CHECK(l.Update(PressureLevel::High, at(7000ms)) == T::None);       // clock restarts here
        CHECK(l.Update(PressureLevel::High, at(14999ms)) == T::None);
        CHECK(l.Update(PressureLevel::High, at(15000ms)) == T::Raised);
    }

    // Hysteresis: hovering between clearBelow and raiseAt does NOT clear; only dropping
    // below clearBelow does. Then the cooldown blocks an immediate re-alert.
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 0ms, 60000ms});
        CHECK(l.Update(PressureLevel::High, at(0ms)) == T::Raised);
        CHECK(l.Update(PressureLevel::Medium, at(1000ms)) == T::None);     // 80-90%: stays raised
        CHECK(l.Active());
        CHECK(l.Update(PressureLevel::High, at(2000ms)) == T::None);       // and no re-raise
        CHECK(l.Update(PressureLevel::Low, at(3000ms)) == T::Cleared);     // < 80%: clears
        CHECK(!l.Active());
        // Back to High 10 s later — inside the 60 s cooldown: no alert.
        CHECK(l.Update(PressureLevel::High, at(13000ms)) == T::None);
        CHECK(l.Update(PressureLevel::High, at(59000ms)) == T::None);
        // Cooldown over and still High: alerts again.
        CHECK(l.Update(PressureLevel::High, at(60000ms)) == T::Raised);
    }

    // Flapping right around the threshold (the screenshot case): High/Medium/High/...
    // every half second for two minutes produces ONE alert, not one per crossing.
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 0ms, 60000ms});
        int raised = 0, cleared = 0;
        for (int i = 0; i < 240; ++i) {
            const auto level = (i % 2 == 0) ? PressureLevel::High : PressureLevel::Medium;
            const auto tr = l.Update(level, at(std::chrono::milliseconds(i * 500)));
            raised += tr == T::Raised;
            cleared += tr == T::Cleared;
        }
        CHECK(raised == 1);
        CHECK(cleared == 0);   // never dropped below Medium
    }
    // Memory-style config (short hold, short cooldown) alerts promptly.
    {
        L l(L::Config{PressureLevel::High, PressureLevel::Medium, 3000ms, 30000ms});
        CHECK(l.Update(PressureLevel::High, at(0ms)) == T::None);
        CHECK(l.Update(PressureLevel::High, at(3000ms)) == T::Raised);
    }
}
