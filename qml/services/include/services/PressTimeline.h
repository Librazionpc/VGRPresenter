#pragma once

#include <QDateTime>

namespace services::diagnostics {
struct PressTimeline
{
    qint64 startMs = 0;          // 0 = no press being traced
    int frames = 0;              // media frames handed over during the press
    double mediaWorstMs = 0.0;
    double mediaSumMs = 0.0;

    bool armed() const { return startMs != 0; }
    qint64 ms() const { return startMs ? QDateTime::currentMSecsSinceEpoch() - startMs : 0; }

    void arm()
    {
        startMs = QDateTime::currentMSecsSinceEpoch();
        frames = 0;
        mediaWorstMs = 0.0;
        mediaSumMs = 0.0;
    }
    // The press's last line: what it cost, and what this thread was busy with
    // meanwhile (the attribution a step list alone cannot give).
    void report(const char *what) const
    {
        if (!armed())
            return;
        qInfo("PRESS TIMELINE: %s +%lld ms | media frames handed over meanwhile=%d "
              "worst=%.0f ms total=%.0f ms",
              what, static_cast<long long>(ms()), frames, mediaWorstMs, mediaSumMs);
    }
    void close() { startMs = 0; }
};

// The one timeline. DEFINED in LiveOutputService.cpp (the presses live there).
extern PressTimeline g_pressTimeline;

// One line per step of the press: the timeline a reader greps, outermost first.
inline void pressTimelineStep(const char *what)
{
    if (g_pressTimeline.armed())
        qInfo("PRESS TIMELINE: step '%s' +%lld ms", what,
              static_cast<long long>(g_pressTimeline.ms()));
}
} // namespace services::diagnostics