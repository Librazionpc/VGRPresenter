// bps_remote — remote-control app for a running engine (SystemArchitecture.md
// §2 Communication layer; the "Remote App" frontend). Talks to `bps_cli --ipc
// <port>` over the TCP + JSON-line transport: ping, health, live log streaming,
// a presentation.slide command, and engine shutdown.
//
//   bps_remote <port> [command]     command: ping (default) | health | logs | slide | shutdown

#include "core/common/Common.hpp"
#include "core/ipc/IpcClient.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace bps;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bps_remote <port> [ping|health|logs|slide|shutdown]\n");
        return 2;
    }
    const std::string host = "127.0.0.1";
    const uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    const std::string cmd = argc > 2 ? argv[2] : "ping";

    IpcClient client;
    auto conn = client.Connect(host, port);
    if (!conn.ok()) {
        std::fprintf(stderr, "remote: connect to %s:%u failed: %s\n", host.c_str(), port,
                     conn.error().message.c_str());
        return 1;
    }
    std::printf("remote: connected to %s:%u (mode=%s)\n", host.c_str(), port, cmd.c_str());

    auto show = [](const Result<json::Value>& r) {
        if (!r.ok()) {
            std::fprintf(stderr, "remote: error: %s\n", r.error().message.c_str());
            return;
        }
        std::printf("%s\n", r.value().ToString().c_str());
    };

    if (cmd == "ping") {
        json::Value::Object p;
        p["from"] = json::Value::String("bps_remote");
        p["v"] = json::Value::Number(1);
        show(client.Call("engine.ping", json::Value(std::move(p))));
    } else if (cmd == "health") {
        show(client.Call("engine.health", json::Value::Object{}));
    } else if (cmd == "slide") {
        json::Value::Object p;
        p["index"] = json::Value::Number(3);
        show(client.Call("presentation.slide", json::Value(std::move(p))));
    } else if (cmd == "shutdown") {
        show(client.Call("engine.shutdown", json::Value::Object{}));
        std::printf("remote: shutdown requested\n");
    } else if (cmd == "logs") {
        show(client.Call("log.subscribe", json::Value::Object{}));
        std::printf("remote: streaming logs... (Ctrl-C to stop)\n");
        // Stream JSON log lines until the engine closes the connection.
        while (true) {
            auto line = client.ReadNextLine();
            if (!line.ok()) {
                std::printf("remote: stream ended (%s)\n", line.error().message.c_str());
                break;
            }
            std::printf("%s\n", line.value().c_str());
            std::fflush(stdout);
        }
    } else {
        std::fprintf(stderr, "remote: unknown command '%s'\n", cmd.c_str());
        return 2;
    }

    client.Close();
    return 0;
}
