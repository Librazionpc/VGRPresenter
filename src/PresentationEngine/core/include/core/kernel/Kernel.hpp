#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps {

// Build identity of the engine binary (01 §3 Build information).
struct BuildInfo {
    Version version;
    std::string buildType;   // "Debug" / "Release"
    std::string compiler;    // e.g. "GCC 13.3.0"
    std::string buildDate;   // __DATE__ __TIME__ of this binary
    std::string platform;    // "linux" / "windows" / "macos"
    std::string arch;        // "x86_64" / "aarch64" / ...
};

// Runtime identity of a live engine instance (01 §3 Runtime information).
struct RuntimeInfo {
    std::string engineUuid;  // stable per process (v4-style)
    std::string sessionId;   // unique per boot
    EngineTime startedAt;
    EngineTime bootedAt;
    KernelState state;
};

// Boot options for the Kernel (docs/specs/01 §3).
struct BootOptions {
    std::string configPath;                   // global config file (03)
    LogLevel logLevel = LogLevel::Info;
    size_t threadCount = 0;                   // 0 = hardware_concurrency (06)
    ResourceMode resourceMode = ResourceMode::Balanced;
    size_t eventHistoryLimit = 10000;         // (05)
    std::string profileDir;                   // (03) optional
    std::string profile;                      // (03) optional active profile
    std::string dataDir;                      // (core/database) persistence dir
    std::vector<std::string> pluginDirs;      // scanned by (09)
    uint16_t ipcPort = 0;                     // >0 boots the TCP+JSON IPC server (core/ipc)
};

// A subsystem that failed to initialize during Boot. The kernel runs without it
// rather than refusing to start — see Kernel::Boot — so this is the honest record
// of what the session is missing, and what a caller should turn into a visible
// "this part is unavailable" message instead of a dead application.
struct DegradedSubsystem {
    std::string system;   // the boot step's name, e.g. "Database"
    int code = 0;         // the Error code the step failed with
    std::string message;
};

// Aggregate engine context (01 §3 Engine context): one object describing the
// live engine — build, runtime, boot options, boot log, degraded systems, health.
struct EngineContext {
    BuildInfo build;
    RuntimeInfo runtime;
    BootOptions options;
    std::vector<std::string> bootLog;
    std::vector<DegradedSubsystem> degraded;
    HealthReport health;
};

// The operating system of the engine (docs/specs/01).
// Nothing in the engine initializes before the Kernel; everything goes through it.
class Kernel final : public IService {
public:
    static Kernel& Instance();

    Result<void> Boot(const BootOptions& options = {});
    Result<void> Shutdown() override;
    Result<void> Pause();
    Result<void> Resume();
    Result<void> Panic(const Error& reason);   // ends in CrashRecovery state
    Result<void> Recover(const BootOptions& options = {});   // re-boot after a panic (01 §Recovery)

    KernelState State() const noexcept { return state_.load(); }
    Version EngineVersion() const noexcept { return kEngineVersion; }
    std::chrono::microseconds Uptime() const;
    EngineTime Now() const { return bps::Now(); }
    const BootOptions& Options() const noexcept { return options_; }
    std::vector<std::string> BootLog() const;   // systems initialized, in order

    // Systems that FAILED to initialize during Boot and are therefore absent
    // from this session (empty on a clean boot). Boot still reaches Running when
    // this is non-empty: the engine comes up with a hole in it, and this is how
    // a caller finds out which hole, instead of the whole UI answering "the
    // engine is not running yet" because one data file was unreadable.
    std::vector<DegradedSubsystem> Degraded() const;

    // Identity (01 §3): engine UUID, build information, runtime information, context.
    const std::string& EngineUuid() const noexcept { return uuid_; }
    BuildInfo GetBuildInfo() const;
    RuntimeInfo GetRuntimeInfo() const;
    EngineContext Context() const;

    const char* ServiceName() const noexcept override { return "Kernel"; }
    HealthReport GetHealth() const override;

private:
    Kernel();
    // Note: there is no FailBoot. Boot never aborts on a subsystem error any
    // more — see BootStep — so a failure has nowhere to go but degraded_.

    Result<void> TransitionState(KernelState next);
    // Run one boot step. A failure — or a throw — records the system in
    // degraded_ and lets Boot continue; true means the step succeeded and the
    // caller may register/announce the system.
    bool BootStep(std::string_view system, const std::function<Result<void>()>& step);
    // Record + log + publish one degraded system (used by BootStep).
    void Degrade(std::string_view system, const Error& cause);
    void AppendBootLog(std::string_view system);
    Result<void> ShutdownSystems();   // reverse-order teardown

    std::atomic<KernelState> state_{KernelState::Stopped};
    EngineTime bootTime_{};
    BootOptions options_;
    std::string uuid_;                                 // set once in the ctor
    std::string sessionId_;                            // set at Boot
    std::string lastError_;                            // guarded by mutex_
    std::vector<std::string> bootLog_;                 // guarded by mutex_
    std::vector<DegradedSubsystem> degraded_;          // guarded by mutex_
    mutable std::mutex mutex_;
};

} // namespace bps
