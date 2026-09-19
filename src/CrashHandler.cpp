#include "CrashHandler.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <cstdio>
#include <csignal>
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
            char buf[128];
            std::snprintf(buf, sizeof(buf), "code=0x%08lx address=%p",
                          static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                          info->ExceptionRecord->ExceptionAddress);
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
            char buf[64];
            std::snprintf(buf, sizeof(buf), "signal=%d", sig);
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
