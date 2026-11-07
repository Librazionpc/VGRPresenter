#pragma once

#include "core/logging/Logger.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace bps::timing {

// Where the time of a repeated unit of work goes, as ONE log line every few seconds:
//
//   StageTiming live: 41 passes — build 2.1/6.0 | compose 38.4/61.2 | outputs 90.1/140.3  (avg/max ms)
//
// Frames that take 100 ms are felt, not seen: without a per-stage split the only number is "the
// frame is slow", and the work ends up optimised where it is easy rather than where it costs.
// Cheap enough for a 60 Hz loop (one lock and a few adds per stage), and quiet when nothing runs.
class StageTiming {
public:
    explicit StageTiming(std::string group, double reportSeconds = 10.0)
        : group_(std::move(group)), reportSeconds_(reportSeconds) {}

    // Add one measured stage of the current unit of work.
    void Add(const char* stage, double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        Stage& s = stages_[stage];
        s.sum += ms;
        s.max = ms > s.max ? ms : s.max;
        ++s.count;
        if (order_.find(stage) == order_.end())
            order_[stage] = static_cast<int>(order_.size());
    }

    // One unit of work finished: log the summary when the window has elapsed.
    void EndUnit() {
        const auto now = std::chrono::steady_clock::now();
        std::string line;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++units_;
            if (windowStart_.time_since_epoch().count() == 0)
                windowStart_ = now;
            if (std::chrono::duration<double>(now - windowStart_).count() < reportSeconds_)
                return;
            std::map<int, std::string> byOrder;
            for (const auto& [name, st] : stages_) {
                char buf[128];
                const double perSample = st.count > 0 ? st.sum / static_cast<double>(st.count) : 0.0;
                const double perUnit = units_ > 0 ? st.sum / static_cast<double>(units_) : 0.0;
                const double samplesPerUnit = units_ > 0 ? static_cast<double>(st.count) / static_cast<double>(units_) : 0.0;
                // A stage that ran once per unit reads "name avg/max"; one that ran more (or less) often
                // also says how many times per unit and what that adds up to per unit: "name avg/max (x3.0 =12.0)".
                if (samplesPerUnit > 0.95 && samplesPerUnit < 1.05)
                    std::snprintf(buf, sizeof(buf), "%s %.1f/%.1f", name.c_str(), perSample, st.max);
                else
                    std::snprintf(buf, sizeof(buf), "%s %.1f/%.1f (x%.2f =%.1f)", name.c_str(),
                                  perSample, st.max, samplesPerUnit, perUnit);
                byOrder[order_[name]] = buf;
            }
            const double seconds = std::chrono::duration<double>(now - windowStart_).count();
            char head[96];
            std::snprintf(head, sizeof(head), "%s: %llu in %.1f s (%.1f/s) — ", group_.c_str(),
                          static_cast<unsigned long long>(units_), seconds,
                          seconds > 0.0 ? static_cast<double>(units_) / seconds : 0.0);
            line = head;
            bool first = true;
            for (const auto& [idx, text] : byOrder) {
                (void)idx;
                line += (first ? "" : " | ") + text;
                first = false;
            }
            line += "  (avg/max ms)";
            stages_.clear();
            units_ = 0;
            windowStart_ = now;
        }
        Logger::Instance().Info(line, "StageTiming");
    }

private:
    struct Stage {
        double sum = 0.0;
        double max = 0.0;
        unsigned long long count = 0;
    };
    std::string group_;
    double reportSeconds_;
    std::mutex mutex_;
    std::map<std::string, Stage> stages_;
    std::map<std::string, int> order_;
    unsigned long long units_ = 0;
    std::chrono::steady_clock::time_point windowStart_{};
};

// Milliseconds since `start`.
inline double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace bps::timing
