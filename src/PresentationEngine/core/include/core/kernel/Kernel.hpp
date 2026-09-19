#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
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

// Aggregate engine context (01 §3 Engine context): one object describing the
// live engine — build, runtime, boot options, boot log, and health.
struct EngineContext {
    BuildInfo build;
    RuntimeInfo runtime;
    BootOptions options;
    std::vector<std::string> bootLog;
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

    // Identity (01 §3): engine UUID, build information, runtime information, context.
    const std::string& EngineUuid() const noexcept { return uuid_; }
    BuildInfo GetBuildInfo() const;
    RuntimeInfo GetRuntimeInfo() const;
    EngineContext Context() const;

    const char* ServiceName() const noexcept override { return "Kernel"; }
    HealthReport GetHealth() const override;

private:
    Kernel();

    Result<void> TransitionState(KernelState next);
    Result<void> FailBoot(const Error& cause);
    void AppendBootLog(std::string_view system);
    Result<void> ShutdownSystems();   // reverse-order teardown

    std::atomic<KernelState> state_{KernelState::Stopped};
    EngineTime bootTime_{};
    BootOptions options_;
    std::string uuid_;                                 // set once in the ctor
    std::string sessionId_;                            // set at Boot
    std::string lastError_;                            // guarded by mutex_
    std::vector<std::string> bootLog_;                 // guarded by mutex_
    mutable std::mutex mutex_;
};

} // namespace bps
