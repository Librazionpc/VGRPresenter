#pragma once

// PresentationSession (docs/specs/19 §Session Manager + Recovery). Tracks the
// active presentation, playback position and state; stores snapshots so a crash
// can be recovered ("Restart → Recover Session → Restore Presentation → Restore
// Slide → Continue"). Storage is in-memory; the caller may persist the JSON.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <mutex>
#include <optional>
#include <string>

namespace bps::presentation {

class PresentationSession {
public:
    Result<void> Initialize();
    Result<void> Shutdown();

    // Captures the current state of the active presentation.
    Result<void> Save(const SessionSnapshot& snapshot);
    Result<void> Clear();
    bool HasSnapshot() const;
    Result<SessionSnapshot> Latest() const;

    // JSON round-trip (persistence via the PAL filesystem is the caller's job).
    static std::string ToJson(const SessionSnapshot& snapshot);
    static Result<SessionSnapshot> FromJson(std::string_view json);

private:
    mutable std::mutex mutex_;
    std::optional<SessionSnapshot> latest_;
    bool initialized_ = false;
};

} // namespace bps::presentation
