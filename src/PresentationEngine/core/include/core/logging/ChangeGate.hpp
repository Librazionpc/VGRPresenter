#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bps::logging {
// One line per CHANGE, for a diagnostic that is evaluated far more often than it changes:
// a frame sanity report, a feed probe, a selection readback.
//
// The signature is what decides, so it has to be the thing whose change IS the news — a
// frame's shape (size, black or not, the scene on air) rather than its mean luma, a feed's
// buffer/enabled/frame flags rather than its frames-sent counter. Gating on a number that
// moves every frame silently turns the gate into a per-frame writer, which is how
// engine.log reached 60 MB a session (docs/scroll_mode_thread_audit.md §F2). Three
// separate sites had grown their own copy of this map, mutex and comparison; this is the one.
class ChangeGate {
public:
    // True when `signature` differs from the one last reported under `name`.
    bool Changed(std::string_view name, std::string_view signature) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string& last = last_[std::string(name)];
        if (last == signature) return false;
        last.assign(signature);
        return true;
    }

    // Forget a name, so its next report counts as a change again (a buffer that went away,
    // a feed that was stopped).
    void Forget(std::string_view name) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_.erase(std::string(name));
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::string> last_;
};
} // namespace bps::logging
