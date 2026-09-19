#pragma once

#include "core/common/Common.hpp"

namespace bps {

// Base interface for every service in the engine (docs/specs/04).
// Systems are registered into the ServiceManager and resolved through it;
// nothing depends on concrete implementations (00 §2).
class IService {
public:
    IService() : created_(Now()) {}
    virtual ~IService() = default;

    // Stable name used in diagnostics, logs, and manifests (00 §6).
    virtual const char* ServiceName() const noexcept = 0;
    virtual Version ServiceVersion() const noexcept { return kEngineVersion; }
    virtual std::vector<std::string> ServiceDependencies() const { return {}; }
    virtual std::chrono::microseconds ServiceUptime() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(EngineClock::now() - created_);
    }

    // Engine-wide lifecycle contract (00 §Engine-wide manager contract): every
    // service supports Initialize/Start/Stop/Shutdown/Reload/Reset. Defaults are
    // safe no-ops; services override the phases they own. No exceptions are
    // thrown across this boundary — failures travel as Result<T> (00 §1).
    virtual Result<void> Initialize() { return Ok(); }
    virtual Result<void> Start() { return Ok(); }
    virtual Result<void> Stop() { return Ok(); }
    virtual Result<void> Shutdown() { return Ok(); }
    virtual Result<void> Reload() { return Ok(); }
    virtual Result<void> Reset() { return Ok(); }

    // Diagnostics contract (00 §7) — never throws, never blocks long.
    virtual HealthReport GetHealth() const { return HealthReport{}; }
    virtual Metrics MetricsSnapshot() const { return Metrics{}; }

protected:
    EngineTime created_{};
};

} // namespace bps
