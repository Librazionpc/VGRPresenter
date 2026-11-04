#pragma once

#include <QElapsedTimer>
#include <QString>

#include "services/EngineBridge.h"

namespace services::diagnostics {

// ONE PRESS, ONE LINE — on both press paths, in the same words.
//
// The two ways a reader moves the picture are LiveOutputService::stepPassage (Slide mode:
// the engine's own Next/Previous) and LiveOutputService::lineStep (Scroll mode: the reading
// run). They are supposed to feel the same, and the only way to know which one is paying
// for what is to measure both with the same clock — a press that takes 700 ms on one path
// and 200 ms on the other is a comparison, one number on its own is not.
//
// The probe times the WHOLE call, which includes every signal handler the press runs
// synchronously (Qt calls a direct connection on this thread, inside the emit), so a tile
// storm lands in `total` rather than hiding outside it.
//
// Scoped, not explicit: both functions have several exits, and a step that refuses to move
// is worth measuring too (a press that takes 700 ms to say no is the report).
struct StepCost
{
    static constexpr qint64 kReportMs = 40;   // below this a press is not worth a log line

    QElapsedTimer clock;
    const char* path;
    int direction;

    StepCost(const char* path_, int direction_)
        : path(path_), direction(direction_)
    {
        clock.start();
    }

    StepCost(const StepCost&) = delete;
    StepCost& operator=(const StepCost&) = delete;

    ~StepCost()
    {
        const qint64 ms = clock.elapsed();
        if (ms < kReportMs)
            return;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StepCost"),
                            QStringLiteral("path=%1 dir=%2 total=%3ms")
                                .arg(QLatin1String(path))
                                .arg(direction)
                                .arg(ms));
    }
};

} // namespace services::diagnostics
