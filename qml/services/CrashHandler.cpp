#include "services/CrashHandler.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QStandardPaths>
#include <QString>

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>   // SYMBOL_INFO / IMAGEHLP_MODULE64 (dll still loaded dynamically)
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

// Shared by the signal path and the terminate handler. Same "signal=" record
// shape the SEH/POSIX records use, so ConsumePendingCrashSummary() reads it
// without knowing which door the crash came through.
void WriteAbortRecord(const char *header, const char *detail, int sig)
{
    if (g_crashLogPath.empty())
        return;
    FILE *f = std::fopen(g_crashLogPath.c_str(), "a");
    if (!f)
        return;
    AppendLine(f, header);
    char buf[256];
    const std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::snprintf(buf, sizeof(buf), "signal=%d (%s) at=%s", sig, detail, ts);
    AppendLine(f, buf);
    std::fclose(f);
}

void HandleAbort(int sig)
{
    // qFatal(), assert() and a plain abort() all arrive here as SIGABRT.
    WriteAbortRecord("---- UNHANDLED SIGNAL ----", "abort", sig);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void HandleTerminate()
{
#if defined(_WIN32)
    // The exception's own stack: without it a terminate record names nothing
    // ("signal=0") and the crash is unattributable. Same frame capture the
    // SEH path uses — RIPs resolve to modules offline.
    auto AppendTerminateStack = []() {
        if (g_crashLogPath.empty())
            return;
        if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
            AppendLine(f, "  frames:");
            void *frames[32];
            USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
            char buf[64];
            for (USHORT i = 0; i < count; ++i) {
                std::snprintf(buf, sizeof(buf), "  #%u %p", i, frames[i]);
                AppendLine(f, buf);
            }
            std::fclose(f);
        }
    };
#else
    auto AppendTerminateStack = []() {};
#endif
    // An exception that escaped every handler (e.g. thrown from a slot or an
    // engine thread) — the default terminate handler would abort() with no
    // record at all.
    const char *what = "terminate";
    try {
        if (auto ex = std::current_exception())
            std::rethrow_exception(ex);
    } catch (const std::exception &e) {
        WriteAbortRecord("---- UNHANDLED EXCEPTION ----", e.what(), 0);
        AppendTerminateStack();
        std::signal(SIGABRT, SIG_DFL);
        std::abort();
    } catch (...) {
    }
    WriteAbortRecord("---- UNHANDLED EXCEPTION ----", what, 0);
    AppendTerminateStack();
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

#if defined(_WIN32)

// In-crash symbolization via dbghelp, loaded dynamically (never a hard
// dependency): raw frame addresses are useless post-mortem — module layout
// dies with the process — so the faulting MODULE + SYMBOL must be resolved
// while the process still lives. Everything is GetProcAddress'ed; any
// failure falls back to the raw-address records (previous behavior).
// dbghelp is documented as not fully thread-safe, but the SEH filter runs
// once on the faulting thread with the process already broken — the
// accepted trade-off for a diagnosable crash over a silent one.
struct CrashSym
{
    BOOL initialized = FALSE;

    using FnSymInitialize = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
    using FnSymFromAddr = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using FnSymGetModuleInfo64 = BOOL(WINAPI *)(HANDLE, DWORD64, PIMAGEHLP_MODULE64);

    FnSymInitialize symInit = nullptr;
    FnSymFromAddr symFromAddr = nullptr;
    FnSymGetModuleInfo64 symModInfo = nullptr;

    CrashSym()
    {
        HMODULE dbg = LoadLibraryA("dbghelp.dll");
        if (!dbg)
            return;
        symInit = reinterpret_cast<FnSymInitialize>(
            reinterpret_cast<void *>(GetProcAddress(dbg, "SymInitialize")));
        symFromAddr = reinterpret_cast<FnSymFromAddr>(
            reinterpret_cast<void *>(GetProcAddress(dbg, "SymFromAddr")));
        symModInfo = reinterpret_cast<FnSymGetModuleInfo64>(
            reinterpret_cast<void *>(GetProcAddress(dbg, "SymGetModuleInfo64")));
        if (symInit && symFromAddr && symModInfo)
            initialized = symInit(GetCurrentProcess(), nullptr, FALSE);
        // invade=FALSE: SymInitialize(TRUE) walks every thread's stack, which
        // can deadlock on the loader lock if the fault happened inside one —
        // the cheap non-invasive init still resolves module/symbol names.
    }
};

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
            // EVERY line is flushed as it is written: if the process dies
            // mid-handler (nested fault inside dbghelp, or the corruption
            // spreading), stdio's buffer would otherwise take the evidence
            // with it — the first instrumented run lost all frames exactly
            // this way and left only the header.
            auto FlushLine = [&](const char *line) {
                AppendLine(f, line);
                std::fflush(f);
            };

            FlushLine(buf);

            void *frames[32];
            USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                std::snprintf(buf, sizeof(buf), "  #%u %p", i, frames[i]);
                FlushLine(buf);   // raw addresses FIRST — never lose them
            }

            // Best-effort symbolization AFTER the raw record is durable: a
            // driver-DLL crash (virtual cam) lands in a module we do not
            // build, and that name alone ends the "whose bug is it" fight.
            CrashSym sym;
            if (sym.initialized) {
                IMAGEHLP_MODULE64 faultMod{};
                faultMod.SizeOfStruct = sizeof(faultMod);
                if (sym.symModInfo(GetCurrentProcess(),
                                   reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress),
                                   &faultMod)) {
                    std::snprintf(buf, sizeof(buf), "  faulting-module: %s",
                                  faultMod.ImageName[0] ? faultMod.ImageName : "?");
                    FlushLine(buf);
                }
                for (USHORT i = 0; i < count; ++i) {
                    const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
                    char symBuf[sizeof(SYMBOL_INFO) + 160] = {};
                    auto *si = reinterpret_cast<SYMBOL_INFO *>(symBuf);
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen = 159;
                    DWORD64 disp = 0;
                    IMAGEHLP_MODULE64 modInfo{};
                    modInfo.SizeOfStruct = sizeof(modInfo);
                    const BOOL hasSym = sym.symFromAddr(GetCurrentProcess(), addr, &disp, si);
                    const BOOL hasMod = sym.symModInfo(GetCurrentProcess(), addr, &modInfo);
                    if (hasSym || hasMod) {
                        std::snprintf(buf, sizeof(buf), "  sym #%u %s!%s+%llu",
                                      i,
                                      hasMod && modInfo.ImageName[0]
                                          ? (strrchr(modInfo.ImageName, '\\')
                                                 ? strrchr(modInfo.ImageName, '\\') + 1
                                                 : modInfo.ImageName)
                                          : "?",
                                      hasSym ? si->Name : "?",
                                      static_cast<unsigned long long>(disp));
                        FlushLine(buf);
                    }
                }
            }
            // FULL-THREAD MINIDUMP, written LAST (after the text record is
            // durable): a heap-corruption detonation usually happens on an
            // innocent thread — the corrupting write lives on another stack —
            // and MiniDumpNormal carries every thread's stack + module map,
            // the cross-thread evidence the text record cannot hold. It also
            // occasionally faults inside the corrupted heap (observed once:
            // 0-byte dump, symbolization already done), which is why it goes
            // after everything else rather than before it.
            {
                using FnMiniDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                  PMINIDUMP_EXCEPTION_INFORMATION,
                                                  PMINIDUMP_USER_STREAM_INFORMATION,
                                                  PMINIDUMP_CALLBACK_INFORMATION);
                if (HMODULE dbgDump = LoadLibraryA("dbghelp.dll")) {
                    const auto miniDump = reinterpret_cast<FnMiniDump>(reinterpret_cast<void *>(
                        GetProcAddress(dbgDump, "MiniDumpWriteDump")));
                    if (miniDump) {
                        std::string dumpPath = g_crashLogPath;
                        const auto slash = dumpPath.rfind('/');
                        if (slash != std::string::npos)
                            dumpPath = dumpPath.substr(0, slash + 1)
                                       + "app-" + std::to_string(GetCurrentProcessId()) + ".dmp";
                        if (HANDLE hDump = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            hDump != INVALID_HANDLE_VALUE) {
                            MINIDUMP_EXCEPTION_INFORMATION mei;
                            mei.ThreadId = GetCurrentThreadId();
                            mei.ExceptionPointers = info;
                            mei.ClientPointers = FALSE;
                            miniDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                                     MiniDumpNormal, &mei, nullptr, nullptr);
                            CloseHandle(hDump);
                            std::snprintf(buf, sizeof(buf), "dump: %s", dumpPath.c_str());
                            FlushLine(buf);
                        }
                    }
                }
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

    // Keep only the newest few rotated records — they are diagnostics, not an
    // archive, and nothing else ever deletes them.
    constexpr int kKeepRotated = 5;
    QDir dir(QFileInfo(QString::fromStdString(g_crashLogPath)).absolutePath());
    const QStringList old = dir.entryList({QStringLiteral("crash.log.*.log")}, QDir::Files, QDir::Name);
    for (int i = 0; i < old.size() - kKeepRotated; ++i)
        dir.remove(old.at(i));
    return summary;
}

void InstallCrashHandler()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/crashes");
    QDir().mkpath(dir);
    g_crashLogPath = (dir + QStringLiteral("/crash.log")).toStdString();

#ifdef _WIN32
    // One boot line into the SAME crash log: the preferred module's runtime
    // base address. Crash-record frame addresses minus this base = file
    // offsets, so a raw-address-only record becomes symbolizable with
    // `llvm-symbolizer`/`addr2line` later, even when the in-crash dbghelp
    // path produced nothing.
    {
        HMODULE exeBase = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&InstallCrashHandler), &exeBase);
        if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
            std::fprintf(f, "---- BOOT base=%p pid=%lu ----\n",
                         reinterpret_cast<void *>(exeBase), GetCurrentProcessId());
            std::fclose(f);
        }
    }
#endif

    std::set_terminate(HandleTerminate);
    std::signal(SIGABRT, HandleAbort);

#if defined(_WIN32)
    // Reserve stack for the handler itself: after a stack overflow the filter
    // otherwise runs on the exhausted stack and faults again with no record.
    // Applies to the calling (GUI) thread.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(HandleSEH);
#else
    std::signal(SIGSEGV, HandleSignal);
    std::signal(SIGFPE, HandleSignal);
    std::signal(SIGILL, HandleSignal);
#endif
}
