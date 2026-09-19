#include "modules/presentation/PresentationSession.hpp"

#include <chrono>

namespace bps::presentation {

Result<void> PresentationSession::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return Ok();
}

Result<void> PresentationSession::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    initialized_ = false;
    return Ok();
}

Result<void> PresentationSession::Save(const SessionSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = snapshot;
    return Ok();
}

Result<void> PresentationSession::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    return Ok();
}

bool PresentationSession::HasSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_.has_value();
}

Result<SessionSnapshot> PresentationSession::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_)
        return Error::Make(Err::Presentation_RecoveryNotFound, "PresentationSession",
                           "no session snapshot");
    return *latest_;
}

std::string PresentationSession::ToJson(const SessionSnapshot& s) {
    json::Value::Object root;
    root["presentationId"] = json::Value::String(s.presentationId);
    root["presentationName"] = json::Value::String(s.presentationName);
    root["slideIndex"] = json::Value::Number(static_cast<double>(s.slideIndex));
    root["state"] = json::Value::Number(static_cast<double>(static_cast<int>(s.state)));
    root["timelineSec"] = json::Value::Number(s.timelineSec);
    root["mode"] = json::Value::Number(static_cast<double>(static_cast<int>(s.mode)));
    auto now = std::chrono::system_clock::to_time_t(s.savedAt);
    root["savedAt"] = json::Value::Number(static_cast<double>(now));
    return json::Value(std::move(root)).ToString();
}

Result<SessionSnapshot> PresentationSession::FromJson(std::string_view json) {
    auto parsed = json::Parse(json);
    if (!parsed.ok()) return parsed.error();
    const json::Value& root = parsed.value();
    if (!root.asObject()) return Error::Make(Err::ParseError, "PresentationSession",
                                             "snapshot JSON must be an object");
    SessionSnapshot s;
    s.presentationId = std::string(root.Find("presentationId")
                                       ? root.Find("presentationId")->asString()
                                       : "");
    s.presentationName = std::string(root.Find("presentationName")
                                         ? root.Find("presentationName")->asString()
                                         : "");
    s.slideIndex = root.Find("slideIndex") ? static_cast<int>(root.Find("slideIndex")->asInt()) : 0;
    s.state = static_cast<PresentationState>(root.Find("state") ? root.Find("state")->asInt() : 0);
    s.timelineSec = root.Find("timelineSec") ? root.Find("timelineSec")->asNumber() : 0.0;
    s.mode = static_cast<PlaybackMode>(root.Find("mode") ? root.Find("mode")->asInt() : 0);
    if (root.Find("savedAt"))
        s.savedAt = std::chrono::system_clock::from_time_t(
            static_cast<std::time_t>(root.Find("savedAt")->asInt()));
    else
        s.savedAt = std::chrono::system_clock::now();
    return s;
}

} // namespace bps::presentation
