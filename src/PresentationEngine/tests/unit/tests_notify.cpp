// Unit tests: Notification service (docs/specs/14).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests notify
#include "TestHarness.hpp"

void TestNotifyQueue() {
    using notification::Notification;
    using notification::NotificationQueue;
    using notification::Priority;

    // Priority ordering: critical pops before low.
    NotificationQueue q(64, 1000);
    Notification low, high;
    low.priority = Priority::Low;
    low.message = "low";
    high.priority = Priority::Critical;
    high.message = "high";
    CHECK(q.Push(low));
    CHECK(q.Push(high));
    auto first = q.Pop();
    CHECK(first.has_value() && first->message == "high");
    CHECK(q.Pop().has_value());

    // Grouping: same groupKey within the window merges (no duplicate).
    NotificationQueue g(64, 1000);
    Notification a, b;
    a.id = 1;
    a.groupKey = "import";
    a.message = "one";
    b.id = 2;
    b.groupKey = "import";
    b.message = "two";
    CHECK(g.Push(a));
    CHECK(!g.Push(b));   // merged, not enqueued
    CHECK(g.Stats().grouped >= 1);
    auto merged = g.Pop();
    CHECK(merged.has_value() && merged->groupCount >= 2 && merged->message == "two");

    // Rate limiting: bursts beyond the per-second cap are dropped.
    NotificationQueue rl(64, 2);
    Notification n;
    CHECK(rl.Push(n));
    CHECK(rl.Push(n));
    CHECK(!rl.Push(n));   // third within the same window dropped
    CHECK(rl.Stats().dropped >= 1);

    // Progress update via id + dismiss/remove.
    NotificationQueue u(64, 1000);
    Notification p;
    p.id = 42;
    CHECK(u.Push(p));
    CHECK(u.Update(42, [](Notification& x) { x.progress = 0.5; }));
    CHECK(u.Remove(42));
    CHECK(u.Pending() == 0);
}
void TestNotifyRules() {
    using namespace notification;

    auto& ns = NotificationService::Instance();
    CHECK(ns.Initialize().ok());

    // Default rule set implements the documented policy examples.
    auto def = MakeDefaultRules();
    CHECK(def != nullptr);
    NotificationSeed s;
    s.sourceEvent = "project.saved";
    auto p = def->Evaluate(s);
    CHECK(p.has_value());
    if (p) {
        CHECK(p->autoDismissMs == 5000);
        CHECK(p->suppressInPresenting);   // never interrupt a live presentation
    }
    NotificationSeed debugSeed;
    debugSeed.severity = Severity::Debug;
    CHECK(def->Evaluate(debugSeed).has_value());   // debug → log-only policy

    // Severity defaults.
    auto crit = DefaultPolicyFor(Severity::Critical);
    CHECK(crit.persistent);
    CHECK(crit.priority == Priority::Critical);
    auto info = DefaultPolicyFor(Severity::Information);
    CHECK(info.autoDismissMs == 8000);

    // Factory: event → seed mapping.
    NotificationFactory f;
    auto saved = f.ProjectSaved("Sunday Service", false);
    CHECK(saved.sourceEvent == "project.saved");
    CHECK(saved.severity == Severity::Success);
    CHECK(saved.category == Category::kProject);
    auto crash = f.EngineCrash("boom");
    CHECK(crash.severity == Severity::Critical);
    CHECK(crash.persistent);
    auto imp = f.ImportFinished("hymn.mp4", 3);
    CHECK(imp.groupKey == "import");
    CHECK(imp.message.find("3 assets") != std::string::npos);

    // Register an extra rule (Open/Closed extension point).
    struct CustomRule final : INotificationRule {
        const char* Name() const noexcept override { return "custom"; }
        std::optional<NotificationPolicy> Evaluate(const NotificationSeed& seed) const override {
            if (seed.sourceEvent == "custom.important") {
                NotificationPolicy p;
                p.show = true;
                p.channels = {"center"};
                p.priority = Priority::High;
                return p;
            }
            return std::nullopt;
        }
    };
    CHECK(ns.RegisterRule(std::make_shared<CustomRule>()).ok());

    CHECK(ns.Shutdown().ok());
}
void TestNotifyPipeline() {
    using namespace notification;

    auto& ns = NotificationService::Instance();
    CHECK(ns.Initialize().ok());
    CHECK(ns.ProviderCount() >= 3);   // Console + Center + StatusBar defaults

    auto rec = std::make_shared<RecordingNotificationProvider>();
    CHECK(ns.RegisterProvider(rec).ok());

    // Publish a seed: Factory → Rules → Queue → Providers.
    NotificationSeed seed;
    seed.sourceModule = "Test";
    seed.sourceEvent = "test.info";
    seed.severity = Severity::Information;
    seed.category = Category::kSystem;
    seed.title = "Hello";
    seed.message = "World";
    auto id = ns.Publish(seed);
    CHECK(id.ok() && id.value() != 0);
    CHECK(rec->Count() >= 1);
    CHECK(rec->Last().title == "Hello");
    CHECK(ns.HistoryCount() >= 1);

    // NotifyNow with direct Notification model.
    Notification n;
    n.title = "Direct";
    n.message = "now";
    auto nid = ns.NotifyNow(n);
    CHECK(nid.ok() && nid.value() != 0);

    // Scheduled delivery (NotifyAfter → Scheduler → Queue on Poll).
    Notification sn;
    sn.title = "Later";
    sn.message = "scheduled";
    auto sid = ns.NotifyAfter(sn, 0);   // due immediately
    CHECK(sid.ok());
    ns.Poll();   // drain due items
    CHECK(ns.HistoryCount() >= 3);

    // Progress update on an in-queue notification (no duplicates).
    Notification qn;
    qn.id = 9001;
    qn.title = "Progress";
    CHECK(ns.Queue().Push(qn));
    CHECK(ns.UpdateProgress(9001, 0.5).ok());

    // Presenting mode: informational notifications degrade to console-only.
    ns.SetPresenting(true);
    NotificationSeed pseed = seed;
    (void)ns.Publish(pseed);
    ns.SetPresenting(false);

    // Dismiss + mark read.
    CHECK(ns.Dismiss(id.value()).ok());
    CHECK(ns.MarkRead(id.value()).ok());

    // EventBus wiring: a content event becomes a user notification.
    (void)EventBus::Instance().Publish(
        events::ContentAssetImported{"u1", "song.mp4", "mp4", 1});
    ns.Poll();
    bool sawImport = false;
    for (const auto& h : ns.History())
        if (h.sourceEvent == std::string(events::ContentAssetImported::kTopic)) sawImport = true;
    CHECK(sawImport);

    // Action invocation publishes an event (carries the identifier only).
    CHECK(ns.InvokeAction(id.value(), "retry").ok());

    CHECK(ns.Shutdown().ok());
}
void TestNotifyWebhook() {
    using namespace notification;
    namespace p = bps::platform;

    auto& plat = p::PlatformAccessor::Get();

    // A tiny local HTTP listener that captures the POST and answers 200.
    auto listener = plat.Sockets().Listen(0);
    CHECK(listener.ok());
    if (!listener.ok()) return;
    auto port = listener.value()->Port();
    CHECK(port > 0);

    std::string receivedBody;
    std::atomic<bool> served{false};
    std::thread server([&]() {
        auto acc = listener.value()->Accept(8000);
        if (!acc.ok()) return;
        char buf[8192] = {};
        auto n = acc.value()->ReceiveTimeout(buf, sizeof buf - 1, 8000);
        if (n.ok() && n.value() > 0) {
            std::string req(buf, n.value());
            auto bodyAt = req.find("\r\n\r\n");
            if (bodyAt != std::string::npos) receivedBody = req.substr(bodyAt + 4);
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            (void)acc.value()->Send(resp);
        }
        acc.value()->Close();
        served.store(true);
    });

    WebhookNotificationProvider webhook;
    CHECK(webhook.SupportedChannels() == std::vector<std::string>{"webhook"});
    webhook.SetEndpoint("127.0.0.1", port, "/hooks/engine");

    // Unconfigured provider degrades to Unsupported (service keeps running).
    WebhookNotificationProvider unconfigured;
    Notification n;
    n.id = 7;
    n.title = "hi";
    CHECK(unconfigured.Show(n).error().code == Err::Unsupported);

    // Configure() parses a full URL.
    json::Value::Object cfg;
    cfg["url"] = json::Value::String("http://127.0.0.1:" + std::to_string(port) + "/h");
    CHECK(webhook.Configure(json::Value(std::move(cfg))).ok());
    // Non-http scheme rejected.
    json::Value::Object badCfg;
    badCfg["url"] = json::Value::String("https://example.com/x");
    CHECK(!webhook.Configure(json::Value(std::move(badCfg))).ok());

    n.title = "Amazing Grace import";
    n.message = "import finished";
    n.category = Category::kImport;
    n.severity = Severity::Success;
    n.sourceEvent = "content.asset_imported";
    auto r = webhook.Show(n);
    CHECK(r.ok());
    server.join();
    CHECK(served.load());
    CHECK(webhook.Delivered() == 1);
    CHECK(webhook.Failed() == 0);
    // The POST carried the JSON payload with the notification fields.
    CHECK(receivedBody.find("\"title\":\"Amazing Grace import\"") != std::string::npos);
    CHECK(receivedBody.find("\"category\":\"Import\"") != std::string::npos);
    // The raw request was recorded (inspection hook).
    CHECK(webhook.LastRequest().find("POST /h HTTP/1.1") != std::string::npos);
    listener.value()->Close();

    // A failing endpoint increments the failure counter and returns an error.
    // Grab a port then close it so nothing is listening (deterministic refusal).
    auto scratch = plat.Sockets().Listen(0);
    CHECK(scratch.ok());
    uint16_t deadPort = 0;
    if (scratch.ok()) {
        deadPort = scratch.value()->Port();
        scratch.value()->Close();
    }
    WebhookNotificationProvider failing;
    failing.SetEndpoint("127.0.0.1", deadPort, "/x");
    CHECK(!failing.Show(n).ok());
    CHECK(failing.Delivered() == 0);
    CHECK(failing.Failed() == 1);
}

// ===========================================================================
// Phase 4 — Project & Data System (docs/specs/15)
// ===========================================================================
