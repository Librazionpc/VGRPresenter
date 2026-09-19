#include "core/ipc/IpcClient.hpp"

#include "platform/PlatformAccessor.hpp"

#include <cstring>
#include <string>

namespace bps {

Result<void> IpcClient::Connect(std::string host, uint16_t port) {
    Close();
    pending_.clear();
    auto r = platform::PlatformAccessor::Get().Sockets().Connect(std::move(host), port);
    if (!r.ok()) return r.error();
    socket_ = std::move(r.value());
    return Ok();
}

Result<json::Value> IpcClient::Call(std::string_view method, const json::Value& params) {
    if (!socket_ || !socket_->IsOpen())
        return Error::Make(Err::InvalidState, "IpcClient", "not connected");
    json::Value::Object req;
    req["id"] = json::Value::Number(1);
    req["method"] = json::Value::String(std::string(method));
    req["params"] = params;
    std::string line = json::Value(std::move(req)).ToString() + "\n";
    auto sent = socket_->Send(line);
    if (!sent.ok()) return sent.error();

    std::string resp = ReadLine();
    if (resp.empty()) return Error::Make(Err::IoError, "IpcClient", "connection closed");
    auto parsed = json::Parse(resp);
    if (!parsed.ok())
        return Error::Make(Err::ParseError, "IpcClient", parsed.error().message);
    const json::Value& r = parsed.value();
    if (const json::Value* err = r.Find("error")) {
        const json::Value* msg = err->Find("message");
        return Error::Make(Err::IoError, "IpcServer",
                           msg ? std::string(msg->asString()) : "rpc error");
    }
    const json::Value* result = r.Find("result");
    if (!result) return Error::Make(Err::ParseError, "IpcClient", "response missing result");
    return *result;
}

Result<std::string> IpcClient::ReadNextLine() {
    if (!socket_ || !socket_->IsOpen())
        return Error::Make(Err::InvalidState, "IpcClient", "not connected");
    std::string line = ReadLine();
    if (line.empty()) return Error::Make(Err::IoError, "IpcClient", "connection closed");
    return line;
}

std::string IpcClient::ReadLine() {
    char tmp[4096];
    while (true) {
        size_t pos = pending_.find('\n');
        if (pos != std::string::npos) {
            std::string line = pending_.substr(0, pos);
            pending_.erase(0, pos + 1);
            return line;
        }
        if (pending_.size() > 64 * 1024) return {};
        auto n = socket_->Receive(tmp, sizeof tmp);
        if (!n.ok() || n.value() == 0) return {};
        pending_.append(tmp, static_cast<size_t>(n.value()));
    }
}

void IpcClient::Close() {
    if (socket_) socket_->Close();
    socket_.reset();
    pending_.clear();
}

} // namespace bps
