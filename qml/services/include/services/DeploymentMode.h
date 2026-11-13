#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#include <atomic>

// DEPLOYMENT MODE — the app as an operator on a Sunday morning should get it.
//
// ON: the log keeps only warnings and errors (no per-pick / per-frame / startup narration, no QML console chatter),
// the developer diagnostics that cost CPU are not started (the interface-stall stack sampler, the startup heartbeat,
// the render-thread profiler), and technical notices — raw engine and Qt warnings turned into toasts
// ("Engine · Core ...", "QML ...") — are logged but never shown. Notices written for the operator ("Recovered from
// an unexpected shutdown", "NDI is not sending ...") still show.
//
// OFF (DEBUG LOGGING): the full developer behaviour — everything above is back, and engine.log carries the whole story
// (picks, timings, GuiStall stacks, every warning). This is what a test build sent to another computer runs in, so
// the log that comes back is complete.
//
// Decided once, never from a setting (so a stored preference cannot leave a developer without diagnostics, or an
// operator with them). In order:
//   1. VGR_DEPLOY=1|0     explicit: on / off. tools/dev_cycle.sh sets 0.
//   2. a developer switch in the environment (VGR_SELFTEST, VGR_LOG_LEVEL, BPS_LIVE_PROFILE, BPS_GUI_STALL_MS) = OFF.
//   3. the file  <app folder>/assets/debug-logging.flag  = OFF. tools/make_installer.sh puts it in the build
//      unless it is run with --quiet, so an installed test build logs everything with no environment variable set.
//   4. otherwise ON.
// Asked before the Qt application exists (the very first lines of main), the answer is ON and is not remembered; the
// first ask after the application exists settles it.
namespace deployment {
inline constexpr const char *kDebugFlagFile = "assets/debug-logging.flag";

inline int decidedFromEnvironment()   // 1 on, 0 off, -1 not decided by the environment
{
    if (qEnvironmentVariableIsSet("VGR_DEPLOY"))
        return qEnvironmentVariableIntValue("VGR_DEPLOY") != 0 ? 1 : 0;
    for (const char *dev : {"VGR_SELFTEST", "VGR_LOG_LEVEL", "BPS_LIVE_PROFILE", "BPS_GUI_STALL_MS"})
        if (qEnvironmentVariableIsSet(dev))
            return 0;
    return -1;
}

inline bool enabled()
{
    static std::atomic<int> settled{-1};
    const int known = settled.load();
    if (known >= 0)
        return known != 0;
    int answer = decidedFromEnvironment();
    if (answer < 0) {
        if (!QCoreApplication::instance())
            return true;   // too early to read the app folder: ON for now, decided properly later
        answer = QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(kDebugFlagFile)))
                     ? 0 : 1;
    }
    settled.store(answer);
    return answer != 0;
}
} // namespace deployment
