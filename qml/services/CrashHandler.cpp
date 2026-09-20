#include "services/CrashHandler.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <cstdio>
#include <csignal>
#include <ctime>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace {

// Set once on the GUI thread before any handler can fire, then only ever
// read (never mutated) from the handler itself — the handler must not touch
// QString/QStandardPaths, so the path is pre-resolved to a plain
// std::string here.
std::string g_crashLogPath;

void AppendLine(FILE *f, const char *line)
{
    std::fputs(line, f);
    std::fputc('\n', f);
}

#if defined(_WIN32)
LONG WINAPI HandleSEH(EXCEPTION_POINTERS *info)
{
    if (!g_crashLogPath.empty()) {
        FILE *f = std::fopen(g_crashLogPath.c_str(), "a");
        if (f) {
            AppendLine(f, "---- UNHANDLED SEH EXCEPTION ----");
            char buf[192];
            const std::time_t now = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
            std::snprintf(buf, sizeof(buf), "code=0x%08lx address=%p at=%s",
                          static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                          info->ExceptionRecord->ExceptionAddress, ts);
            AppendLine(f, buf);

            void *frames[32];
            USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                std::snprintf(buf, sizeof(buf), "  #%u %p", i, frames[i]);
                AppendLine(f, buf);
            }
            std::fclose(f);
        }
    }
    // We only log — let whatever handler (debugger, OS crash dialog, or
    // process termination) would normally run still run.
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void HandleSignal(int sig)
{
    if (!g_crashLogPath.empty()) {
        FILE *f = std::fopen(g_crashLogPath.c_str(), "a");
        if (f) {
            AppendLine(f, "---- UNHANDLED SIGNAL ----");
            char buf[96];
            const std::time_t now = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
            std::snprintf(buf, sizeof(buf), "signal=%d at=%s", sig, ts);
            AppendLine(f, buf);

            void *frames[32];
            int count = backtrace(frames, 32);
            backtrace_symbols_fd(frames, count, fileno(f));
            std::fclose(f);
        }
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

} // namespace

QString ConsumePendingCrashSummary()
{
    // Delivered-exactly-once: read the crash log, summarize its LAST record,
    // then rotate it aside so the next launch starts clean. Runs on the GUI
    // thread at first QML access — normal file I/O is fine here.
    if (g_crashLogPath.empty())
        return {};
    FILE *f = std::fopen(g_crashLogPath.c_str(), "rb");
    if (!f)
        return {};
    std::string contents;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        contents.append(buf, n);
    std::fclose(f);

    if (contents.find("UNHANDLED") == std::string::npos) {
        // Log exists but holds no crash record (header-only/legacy) — leave it.
        return {};
    }

    // Last "code=" / "signal=" line is the summary of the most recent crash.
    QString summary;
    size_t pos = contents.rfind("code=");
    const size_t sigPos = contents.rfind("signal=");
    if (sigPos != std::string::npos && (pos == std::string::npos || sigPos > pos))
        pos = sigPos;
    if (pos != std::string::npos) {
        const size_t end = contents.find('\n', pos);
        summary = QString::fromStdString(
            contents.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        summary = summary.trimmed();
    }

    // Rotate: crash-<epoch>.log next to the original.
    const QString rotated = QString::fromStdString(g_crashLogPath)
                                + QStringLiteral(".%1.log").arg(QDateTime::currentSecsSinceEpoch());
    QDir().rename(QString::fromStdString(g_crashLogPath), rotated);
    return summary;
}

void InstallCrashHandler()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/crashes");
    QDir().mkpath(dir);
    g_crashLogPath = (dir + QStringLiteral("/crash.log")).toStdString();

#if defined(_WIN32)
    SetUnhandledExceptionFilter(HandleSEH);
#else
    std::signal(SIGSEGV, HandleSignal);
    std::signal(SIGABRT, HandleSignal);
    std::signal(SIGFPE, HandleSignal);
    std::signal(SIGILL, HandleSignal);
#endif
}
