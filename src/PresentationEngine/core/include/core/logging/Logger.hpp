#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bps {

// ---------------------------------------------------------------------------
// Log record & query
// ---------------------------------------------------------------------------
struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::chrono::system_clock::time_point timestamp;
    std::thread::id threadId;
    std::string module;
    std::string category;
    std::string message;
    std::string session;   // engine session id (02 §Session logging)
};

struct LogQuery {
    LogLevel minLevel = LogLevel::Trace;
    std::string module;    // empty = any
    std::string category;  // empty = any
    std::string text;      // substring match on message
    size_t maxResults = 1024;
};

// ---------------------------------------------------------------------------
// Sinks (docs/specs/02 §3)
// ---------------------------------------------------------------------------
class ISink {
public:
    virtual ~ISink() = default;
    virtual const char* Name() const noexcept = 0;
    virtual void Write(std::string_view formatted, const LogRecord& record) = 0;
    virtual Result<void> Flush() { return Ok(); }

    LogLevel MinLevel() const noexcept { return minLevel_.load(); }
    void SetMinLevel(LogLevel level) { minLevel_.store(level); }

private:
    std::atomic<LogLevel> minLevel_{LogLevel::Trace};
};

class ConsoleSink final : public ISink {
public:
    const char* Name() const noexcept override { return "Console"; }
    void Write(std::string_view formatted, const LogRecord&) override {
        fprintf(stderr, "%.*s\n", static_cast<int>(formatted.size()), formatted.data());
    }
};

class FileSink final : public ISink {
public:
    explicit FileSink(std::string path, size_t maxBytes = 16ull * 1024 * 1024, size_t keepBackups = 3);
    ~FileSink() override;
    const char* Name() const noexcept override { return "File"; }
    void Write(std::string_view formatted, const LogRecord&) override;
    Result<void> Flush() override;

private:
    void MaybeRotate();
    std::string path_;
    size_t maxBytes_;
    size_t keepBackups_;
    std::ofstream file_;
    size_t written_ = 0;
    int rotationIndex_ = 0;
};

// JSON-lines sink: one compact JSON object per record (02 §3 JSON output).
class JsonSink final : public ISink {
public:
    explicit JsonSink(std::string path);
    ~JsonSink() override;
    const char* Name() const noexcept override { return "Json"; }
    void Write(std::string_view formatted, const LogRecord& record) override;
    Result<void> Flush() override;

private:
    std::string path_;
    std::ofstream file_;
};

// Debugger sink (02 §3): on Windows this would call OutputDebugStringA; on
// POSIX it writes a [DBG]-prefixed line to stderr and keeps a small ring a
// debugger / diagnostics can inspect via RecentLines().
class DebuggerSink final : public ISink {
public:
    const char* Name() const noexcept override { return "Debugger"; }
    void Write(std::string_view formatted, const LogRecord& record) override;
    std::vector<std::string> RecentLines(size_t max = 64) const;

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;   // guarded by mutex_
};

// Remote sink (02 §3): serializes each record as a JSON line and hands it to a
// publisher callback. The Kernel wires the publisher to IpcServer::Broadcast so
// `log.subscribe` clients receive live engine logs (SystemArchitecture §2).
class RemoteSink final : public ISink {
public:
    const char* Name() const noexcept override { return "Remote"; }
    void SetPublisher(std::function<void(const std::string& jsonLine)> pub);
    bool HasPublisher() const;
    void Write(std::string_view formatted, const LogRecord& record) override;

private:
    mutable std::mutex mutex_;
    std::function<void(const std::string&)> publisher_;   // guarded by mutex_
};

// Shared JSON-line formatter for JsonSink and RemoteSink (02 §3).
std::string LogJsonLine(const LogRecord& rec);

// ---------------------------------------------------------------------------
// Logger (docs/specs/02)
// ---------------------------------------------------------------------------
class Logger final : public IService {
public:
    static Logger& Instance();

    Result<void> Initialize() override;   // starts logger thread, installs Console+File sinks
    Result<void> Shutdown() override;     // flush + join thread + flush sinks

    void Log(LogLevel level, std::string_view module, std::string_view category,
             std::string_view message);
    void Trace(std::string_view m, std::string_view c = {})   { Log(LogLevel::Trace,   "Core", c, m); }
    void Debug(std::string_view m, std::string_view c = {})   { Log(LogLevel::Debug,   "Core", c, m); }
    void Info(std::string_view m, std::string_view c = {})    { Log(LogLevel::Info,    "Core", c, m); }
    void Warning(std::string_view m, std::string_view c = {}) { Log(LogLevel::Warning, "Core", c, m); }
    void Error(std::string_view m, std::string_view c = {})   { Log(LogLevel::Error,   "Core", c, m); }
    void Fatal(std::string_view m, std::string_view c = {})   { Log(LogLevel::Fatal,   "Core", c, m); }

    Result<void> AddSink(std::shared_ptr<ISink> sink);
    Result<void> RemoveSink(std::string_view name);
    void SetGlobalLevel(LogLevel level) { globalLevel_.store(level); }
    void SetCategoryLevel(std::string_view category, LogLevel level);
    LogLevel GlobalLevel() const noexcept { return globalLevel_.load(); }

    // Session logging (02 §Session): stamped into every record after this call.
    void SetSessionId(std::string session);
    std::string SessionId() const;

    // Crash logging (02 §Crash): Fatal record + synchronous flush so the line
    // survives an imminent crash (used by Kernel::Panic).
    void CrashLog(std::string_view module, std::string_view message);

    // Performance logging (02 §Performance): mean sink-write time per record.
    double AvgWriteTimeMicros() const noexcept {
        uint64_t w = writes_.load();
        return w ? static_cast<double>(writeTimeNs_.load()) / static_cast<double>(w) / 1000.0
                 : 0.0;
    }

    Result<void> Flush();
    std::vector<LogRecord> Search(const LogQuery& query) const;   // memory buffer (02 §3)
    size_t RingSize() const noexcept { return ringLimit_; }
    void SetRingLimit(size_t limit);

    const char* ServiceName() const noexcept override { return "Logger"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const;

private:
    Logger() = default;
    ~Logger() override = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void Pump();
    bool PassesFilter(LogLevel level, std::string_view category) const;
    std::string Format(const LogRecord& rec) const;
    int ThreadName(std::thread::id id) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<LogRecord> queue_;       // pending records (guarded by mutex_)
    std::deque<LogRecord> ring_;        // memory buffer for Search (guarded by mutex_)
    std::string sessionId_;             // guarded by mutex_
    std::vector<std::shared_ptr<ISink>> sinks_;               // guarded by mutex_
    std::unordered_map<std::string, LogLevel> categoryLevels_; // guarded by mutex_
    mutable std::unordered_map<std::thread::id, int> threadNames_; // guarded by mutex_

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<bool> idle_{true};   // pump finished writing the last batch
    std::atomic<LogLevel> globalLevel_{LogLevel::Trace};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> emitted_{0};
    std::atomic<uint64_t> writeTimeNs_{0};   // cumulative sink write time
    std::atomic<uint64_t> writes_{0};        // records written to sinks

    static constexpr size_t kMaxQueue = 100000;
    size_t ringLimit_ = 8192;  // guarded by mutex_
};

} // namespace bps
