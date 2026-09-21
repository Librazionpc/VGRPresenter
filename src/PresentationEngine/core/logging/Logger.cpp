#include "core/logging/Logger.hpp"

#include "core/config/Json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>

namespace bps {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------
FileSink::FileSink(std::string path, size_t maxBytes, size_t keepBackups)
    : path_(std::move(path)), maxBytes_(maxBytes), keepBackups_(keepBackups) {
    std::error_code ec;
    std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    file_.open(path_, std::ios::app);
}

FileSink::~FileSink() {
    if (file_.is_open()) file_.close();
}

void FileSink::Write(std::string_view formatted, const LogRecord&) {
    if (!file_.is_open()) return;
    // Emit the whole line (including its newline) in a single write call so that
    // concurrent writers (parallel test processes, remote apps) appending to the
    // same file never interleave mid-line (02 §Atomicity).
    std::string line(formatted);
    line.push_back('\n');
    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
    written_ += line.size();
    MaybeRotate();
}

Result<void> FileSink::Flush() {
    if (file_.is_open()) file_.flush();
    return Ok();
}

void FileSink::MaybeRotate() {
    if (written_ < maxBytes_) return;
    file_.close();
    // shift backups: path.k -> path.(k+1), drop the oldest
    for (size_t i = keepBackups_; i >= 1; --i) {
        std::string from = std::format("{}.{}", path_, i - 1);
        std::string to = std::format("{}.{}", path_, i);
        std::remove(to.c_str());
        std::rename(from.c_str(), to.c_str());
    }
    std::string rotated = path_ + ".0";
    std::remove(rotated.c_str());
    std::rename(path_.c_str(), rotated.c_str());
    ++rotationIndex_;
    file_.open(path_, std::ios::out | std::ios::trunc);
    written_ = 0;
}

// ---------------------------------------------------------------------------
// JsonSink
// ---------------------------------------------------------------------------
JsonSink::JsonSink(std::string path) : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    file_.open(path_, std::ios::app);
}

JsonSink::~JsonSink() {
    if (file_.is_open()) file_.close();
}

Result<void> JsonSink::Flush() {
    if (file_.is_open()) file_.flush();
    return Ok();
}

std::string LogJsonLine(const LogRecord& rec) {
    json::Value::Object o;
    o["level"] = json::Value::String(ToString(rec.level));
    std::time_t t = std::chrono::system_clock::to_time_t(rec.timestamp);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tmv);
    o["ts"] = json::Value::String(ts);
    o["thread"] = json::Value::String(
        std::format("Thread-{}", std::hash<std::thread::id>{}(rec.threadId) % 1000));
    o["module"] = json::Value::String(rec.module);
    if (!rec.category.empty()) o["category"] = json::Value::String(rec.category);
    o["message"] = json::Value::String(rec.message);
    if (!rec.session.empty()) o["session"] = json::Value::String(rec.session);
    return json::Value(std::move(o)).ToString();
}

void JsonSink::Write(std::string_view, const LogRecord& rec) {
    if (!file_.is_open()) return;
    std::string line = LogJsonLine(rec);
    line.push_back('\n');
    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
}

// ---------------------------------------------------------------------------
// DebuggerSink
// ---------------------------------------------------------------------------
void DebuggerSink::Write(std::string_view formatted, const LogRecord&) {
    // All platforms: stderr + a queryable ring. A Windows OutputDebugStringA
    // backend belongs in the PAL (DoD §1), not in core/logging.
    std::fprintf(stderr, "[DBG] %.*s\n", static_cast<int>(formatted.size()),
                 formatted.data());
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(std::string(formatted));
    if (lines_.size() > 256) lines_.pop_front();
}

std::vector<std::string> DebuggerSink::RecentLines(size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(std::min(max, lines_.size()));
    for (auto it = lines_.rbegin(); it != lines_.rend() && out.size() < max; ++it)
        out.push_back(*it);
    return out;
}

// ---------------------------------------------------------------------------
// RemoteSink
// ---------------------------------------------------------------------------
void RemoteSink::SetPublisher(std::function<void(const std::string&)> pub) {
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_ = std::move(pub);
}

bool RemoteSink::HasPublisher() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publisher_ != nullptr;
}

void RemoteSink::Write(std::string_view, const LogRecord& rec) {
    std::function<void(const std::string&)> pub;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pub = publisher_;
    }
    if (pub) pub(LogJsonLine(rec));   // publish outside the lock
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
Result<void> Logger::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) return Ok();
        // A re-initialization after Shutdown must clear the shutdown flag, or the
        // new pump would see it already set and exit on its first wakeup.
        shutdownRequested_.store(false);
        running_.store(true);
        if (sinks_.empty()) {
            sinks_.push_back(std::make_shared<ConsoleSink>());
            sinks_.push_back(std::make_shared<FileSink>("logs/engine.log"));
        }
    }
    thread_ = std::thread(&Logger::Pump, this);
    return Ok();
}

Result<void> Logger::Shutdown() {
    shutdownRequested_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    std::vector<std::shared_ptr<ISink>> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks = sinks_;
    }
    for (auto& sink : sinks) (void)sink->Flush();
    return Ok();
}

void Logger::Log(LogLevel level, std::string_view module, std::string_view category,
                 std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(globalLevel_.load())) return;
    if (!PassesFilter(level, category)) return;

    if (!running_.load()) {
        // Boot-safe fallback path: direct to stderr (Kernel has no logger yet).
        fprintf(stderr, "[%s][%.*s] %.*s\n", ToString(level), static_cast<int>(module.size()),
                module.data(), static_cast<int>(message.size()), message.data());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueue) {
            dropped_.fetch_add(1);
            return;
        }
        LogRecord rec;
        rec.level = level;
        rec.timestamp = std::chrono::system_clock::now();
        rec.threadId = std::this_thread::get_id();
        rec.module = std::string(module);
        rec.category = std::string(category);
        rec.message = std::string(message);
        rec.session = sessionId_;
        queue_.push_back(std::move(rec));
        emitted_.fetch_add(1);
    }
    cv_.notify_one();
}

bool Logger::PassesFilter(LogLevel level, std::string_view category) const {
    if (category.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = categoryLevels_.find(std::string(category));
    if (it == categoryLevels_.end()) return true;
    return static_cast<int>(level) >= static_cast<int>(it->second);
}

void Logger::SetCategoryLevel(std::string_view category, LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    categoryLevels_[std::string(category)] = level;
}

void Logger::SetRingLimit(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    ringLimit_ = limit;
    while (ring_.size() > ringLimit_) ring_.pop_front();
}

void Logger::SetSessionId(std::string session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionId_ = std::move(session);
}

std::string Logger::SessionId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionId_;
}

void Logger::CrashLog(std::string_view module, std::string_view message) {
    Log(LogLevel::Fatal, module, "Crash", message);
    (void)Flush();   // synchronous: ensure the fatal line reaches the sinks
}

Result<void> Logger::AddSink(std::shared_ptr<ISink> sink) {
    if (!sink) return Error::Make(Err::InvalidArgument, "Logger", "null sink");
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sinks_)
        if (std::string_view(s->Name()) == std::string_view(sink->Name()))
            return Error::Make(Err::AlreadyExists, "Logger", "sink already added: " + std::string(sink->Name()));
    sinks_.push_back(std::move(sink));
    return Ok();
}

Result<void> Logger::RemoveSink(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
        if (std::string_view((*it)->Name()) == name) {
            sinks_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::NotFound, "Logger", "sink not found: " + std::string(name));
}

Result<void> Logger::Flush() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Crash path: if the pump is not running, drain synchronously so records
        // (e.g. a Fatal crash marker) still reach the sinks (02 §Crash).
        if (!running_.load() && !queue_.empty()) {
            std::vector<std::shared_ptr<ISink>> sinks = sinks_;
            while (!queue_.empty()) {
                LogRecord rec = std::move(queue_.front());
                queue_.pop_front();
                ring_.push_back(rec);
                if (ring_.size() > ringLimit_) ring_.pop_front();
                std::string formatted = Format(rec);
                for (auto& sink : sinks)
                    if (static_cast<int>(rec.level) >= static_cast<int>(sink->MinLevel()))
                        sink->Write(formatted, rec);
            }
            for (auto& sink : sinks) (void)sink->Flush();
            return Ok();
        }
        // Wait until the queue is drained AND the pump has finished writing the
        // last batch to the sinks (02 §6 Flush semantics).
        cv_.wait(lock, [&] { return queue_.empty() && idle_.load(); });
        std::vector<std::shared_ptr<ISink>> sinks = sinks_;
        lock.unlock();
        for (auto& sink : sinks) (void)sink->Flush();
    }
    return Ok();
}

std::vector<LogRecord> Logger::Search(const LogQuery& query) const {
    std::vector<LogRecord> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = ring_.rbegin(); it != ring_.rend() && out.size() < query.maxResults; ++it) {
        const LogRecord& r = *it;
        if (static_cast<int>(r.level) < static_cast<int>(query.minLevel)) continue;
        if (!query.module.empty() && r.module != query.module) continue;
        if (!query.category.empty() && r.category != query.category) continue;
        if (!query.text.empty() && !r.message.contains(query.text)) continue;
        out.push_back(r);
    }
    return out;
}

std::string Logger::Format(const LogRecord& rec) const {
    // Local wall-clock time first, so a tailed log reads as a timeline of what the app is doing.
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(rec.timestamp.time_since_epoch()).count() % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(rec.timestamp);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    std::string out = std::format("{:02}:{:02}:{:02}.{:03} [{}][{}][Thread-{}]", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                                  static_cast<int>(ms), ToString(rec.level), rec.module, ThreadName(rec.threadId));
    if (!rec.category.empty()) out += std::format("[{}]", rec.category);
    out += " ";
    out += rec.message;
    return out;
}

int Logger::ThreadName(std::thread::id id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threadNames_.find(id);
    if (it != threadNames_.end()) return it->second;
    int n = static_cast<int>(threadNames_.size()) + 1;
    threadNames_[id] = n;
    return n;
}

void Logger::Pump() {
    while (true) {
        std::deque<LogRecord> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return shutdownRequested_.load() || !queue_.empty(); });
            if (queue_.empty() && shutdownRequested_.load()) break;
            batch.swap(queue_);
            idle_.store(false);
            cv_.notify_all();   // wake Flush() waiters so they re-check
        }
        std::vector<std::shared_ptr<ISink>> sinks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sinks = sinks_;
            for (auto& rec : batch) {
                ring_.push_back(rec);
                if (ring_.size() > ringLimit_) ring_.pop_front();
            }
        }
        auto writeBegin = EngineClock::now();
        for (auto& rec : batch) {
            std::string formatted = Format(rec);
            for (auto& sink : sinks) {
                if (static_cast<int>(rec.level) >= static_cast<int>(sink->MinLevel())) {
                    sink->Write(formatted, rec);
                }
            }
        }
        writeTimeNs_.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(EngineClock::now() - writeBegin)
                .count());
        for (auto& sink : sinks) (void)sink->Flush();
        writes_.fetch_add(batch.size());
        idle_.store(true);
        cv_.notify_all();   // batch fully written; Flush() can return
    }
}

HealthReport Logger::GetHealth() const {
    HealthReport r;
    if (running_.load() && dropped_.load() == 0) {
        r.state = HealthState::Healthy;
        r.detail = "logger thread running";
    } else if (running_.load()) {
        r.state = HealthState::Degraded;
        r.detail = std::format("{} records dropped", dropped_.load());
        r.errorCount = dropped_.load();
    } else {
        r.state = HealthState::Failing;
        r.detail = "logger not initialized";
    }
    return r;
}

Metrics Logger::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = queue_.size();
    m.errorCount = dropped_.load();
    m.threadCount = 1;   // the async pump thread
    m.latencyP95 = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double, std::micro>(AvgWriteTimeMicros()));
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
