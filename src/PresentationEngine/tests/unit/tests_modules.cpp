// Unit tests: Plugin manager + IPC log streaming.
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests modules
#include "TestHarness.hpp"

void TestPluginManager() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/bps_plugin_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto& pm = PluginManager::Instance();
    {
        std::ofstream m(dir + "/plug.json");
        m << "{\"id\":\"plug\",\"version\":\"1.0.0\",\"author\":\"test\","
             "\"libraryPath\":\"plug.so\",\"abiVersion\":1,"
             "\"capabilities\":[\"network\",\"slides\"]}";
    }
    CHECK(pm.Discover(dir).ok());
    CHECK(pm.DiscoveredCount() >= 1);

    // Sandbox (09 §Sandbox): profile + capability gate + denial accounting.
    SandboxProfile lockdown;
    lockdown.name = "lockdown";
    lockdown.denyNetwork = true;
    CHECK(pm.SetSandbox("plug", lockdown).ok());
    CHECK(pm.SandboxOf("plug").denyNetwork);
    CHECK(pm.CheckCapability("plug", "slides").ok());      // declared, not denied
    CHECK(!pm.CheckCapability("plug", "network").ok());     // declared but denied
    CHECK(pm.CheckCapability("plug", "network").error().code == Err::Plugin_CapabilityDenied);
    CHECK(!pm.CheckCapability("plug", "video").ok());       // undeclared
    CHECK(pm.SandboxDenials("plug") >= 2);
    CHECK(!pm.SetSandbox("nosuchplugin", lockdown).ok());
    CHECK(!pm.CheckCapability("nosuchplugin", "slides").ok());

    // Isolation policy (09 §Isolation): OutOfProcess is explicitly post-v1.
    CHECK(pm.SetIsolationPolicy(IsolationPolicy::InProcess).ok());
    CHECK(!pm.SetIsolationPolicy(IsolationPolicy::OutOfProcess).ok());
    CHECK(pm.Isolation() == IsolationPolicy::InProcess);

    // Update (09 §Update): staging a newer manifest bumps the registry entry.
    {
        std::ofstream m(dir + "/plug_v2.json");
        m << "{\"id\":\"plug\",\"version\":\"1.1.0\",\"author\":\"test\","
             "\"libraryPath\":\"plug.so\",\"abiVersion\":1,\"capabilities\":[\"slides\"]}";
    }
    auto up = pm.Update("plug", dir + "/plug_v2.json");
    CHECK(!up.ok());   // the library is absent, so (re)load fails — manifest is staged
    bool vBumped = false;
    for (const auto& info : pm.Snapshot())
        if (info.id == "plug" && info.version.ToString() == "1.1.0") vBumped = true;
    CHECK(vBumped);
    CHECK(!pm.Update("plug", dir + "/plug_v2.json").ok());    // not newer
    CHECK(!pm.Update("other", dir + "/plug_v2.json").ok());   // id mismatch

    std::error_code ec2;
    fs::remove_all(dir, ec2);
}

#if !defined(_WIN32)
void TestIpcLogStream() {
    auto& server = IpcServer::Instance();
    CHECK(server.Start(0).ok());
    const uint16_t port = server.Port();

    IpcClient client;
    CHECK(client.Connect("127.0.0.1", port).ok());
    auto sub = client.Call("log.subscribe", json::Value::Object{});
    CHECK(sub.ok());
    CHECK(server.SubscriberCount() == 1);

    // Two back-to-back broadcasts may land in one TCP segment: the client must
    // preserve every line (regression: ReadLine once dropped the second line).
    CHECK(server.Broadcast("{\"level\":\"INFO\",\"message\":\"stream-one\"}").ok());
    CHECK(server.Broadcast("{\"level\":\"INFO\",\"message\":\"stream-two\"}").ok());
    auto r1 = client.ReadNextLine();
    auto r2 = client.ReadNextLine();
    CHECK(r1.ok() && r2.ok());
    if (r1.ok() && r2.ok()) {
        bool sawOne = r1.value().find("stream-one") != std::string::npos ||
                      r2.value().find("stream-one") != std::string::npos;
        bool sawTwo = r1.value().find("stream-two") != std::string::npos ||
                      r2.value().find("stream-two") != std::string::npos;
        CHECK(sawOne && sawTwo);
    }

    auto unsub = client.Call("log.unsubscribe", json::Value::Object{});
    CHECK(unsub.ok());
    CHECK(server.SubscriberCount() == 0);
    client.Close();
    CHECK(server.Stop().ok());
}
#endif

// ===========================================================================
// Phase 4 — Notification Service (docs/specs/14)
// ===========================================================================
