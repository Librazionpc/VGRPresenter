#include "services/CallTrace.h"
#include "services/CrashHandler.h"
#include "services/DataRoot.h"
#include "services/EngineBridge.h"
#include "modules/presentation/LiveOutputController.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>   // std::strlen (the breadcrumb prefix in ConsumePendingCrashSummary)
#include <ctime>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>    // CreateProcess needs a mutable argv buffer

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>   // SYMBOL_INFO / IMAGEHLP_MODULE64 (dll still loaded dynamically)
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
std::string g_crashLogPath;

void AppendLine(FILE *f, const char *line)
{
    std::fputs(line, f);
    std::fputc('\n', f);
}

void AppendBreadcrumb(FILE *f)
{
    const char *chain = "";
    (void)calltrace::guiChainRaw(&chain);
    char buf[600];
    std::snprintf(buf, sizeof(buf), "  last-call: %s",
                  chain[0] != '\0'
                      ? chain
                      : "(none instrumented — the fault is outside the traced entry points)");
    AppendLine(f, buf);
}

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
    AppendBreadcrumb(f);
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
            auto FlushLine = [&](const char *line) {
                AppendLine(f, line);
                std::fflush(f);
            };

            FlushLine(buf);

            // The breadcrumb next to the header: the faulting call, before the
            // raw frames that need symbols to mean anything.
            {
                const char *chain = "";
                (void)calltrace::guiChainRaw(&chain);
                char crumb[600];
                std::snprintf(crumb, sizeof(crumb), "  last-call: %s",
                              chain[0] != '\0'
                                  ? chain
                                  : "(none instrumented — the fault is outside the traced entry points)");
                FlushLine(crumb);
            }

            void *frames[32];
            USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                std::snprintf(buf, sizeof(buf), "  #%u %p", i, frames[i]);
                FlushLine(buf);   // raw addresses FIRST — never lose them
            }

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

// A BAD FREE ENDS THE PROCESS BEFORE THE FILTER ABOVE CAN RUN. ntdll raises STATUS_HEAP_CORRUPTION
// (0xC0000374) when the heap finds a block that was written past its end, freed twice or freed
// after something else reused it — and then terminates the process itself, so the unhandled-
// exception filter never sees it and nothing was recorded: ~130 Windows crash reports and not one
// line here. A vectored handler is called first, on the thread that found the damage; its stack
// names what was being freed or allocated, which is the closest the heap can point to the culprit.
LONG WINAPI HandleHeapCorruption(EXCEPTION_POINTERS *info)
{
    static std::atomic<bool> reported{false};
    if (info->ExceptionRecord->ExceptionCode == 0xC0000374UL && !reported.exchange(true))
        return HandleSEH(info);
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

#if defined(_WIN32)

// ---- gdb self-attach (the symbolized stack, with no debugger installed) ----

// Where to find a gdb: an explicit override, then the gdb the portable dist
// ships beside the exe (tools/gdb/gdb.exe), then whatever is on PATH.
std::string ResolveGdb()
{
    char env[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("VGR_GDB_BIN", env, sizeof(env));
    if (n > 0 && n < sizeof(env) && GetFileAttributesA(env) != INVALID_FILE_ATTRIBUTES)
        return std::string(env);

    char exe[MAX_PATH];
    const DWORD m = GetModuleFileNameA(nullptr, exe, sizeof(exe));
    if (m > 0 && m < sizeof(exe)) {
        std::string dir(exe);
        const auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string cand = dir.substr(0, slash + 1) + "tools\\gdb\\gdb.exe";
            if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                return cand;
        }
    }

    char found[MAX_PATH];
    if (SearchPathA(nullptr, "gdb.exe", nullptr, sizeof(found), found, nullptr) > 0)
        return std::string(found);
    return {};   // no gdb on this machine — the text record + minidump remain
}

std::atomic<bool> g_gdbCaptureInFlight{false};
std::chrono::steady_clock::time_point g_lastGdbCapture{};
constexpr auto kGdbCaptureCooldown = std::chrono::seconds(60);

bool GdbCaptureHasContent(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return size > 0;
}

bool SpawnGdbCapture(const std::string &outPath)
{
    const std::string gdb = ResolveGdb();
    if (gdb.empty())
        return false;

    bool idle = false;
    if (!g_gdbCaptureInFlight.compare_exchange_strong(idle, true))
        return false;   // an attach from an earlier report is still walking
    struct ReleaseInFlight {
        ~ReleaseInFlight() { g_gdbCaptureInFlight.store(false); }
    } releaseInFlight;

    const auto now = std::chrono::steady_clock::now();
    if (g_lastGdbCapture.time_since_epoch().count() != 0
        && now - g_lastGdbCapture < kGdbCaptureCooldown)
        return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileA(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
        return false;

    // thread apply all bt: every thread, not just the blocked one — a deadlock
    // needs BOTH stacks (who holds the lock, who waits).
    std::string cmd = "\"" + gdb + "\" -batch"
                      " -ex \"set pagination off\""
                      " -ex \"set confirm off\""
                      " -ex \"info threads\""
                      " -ex \"thread apply all bt\""
                      " -p " + std::to_string(GetCurrentProcessId());

    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    const UINT savedErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    if (CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                       TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetErrorMode(savedErrorMode);
    CloseHandle(hOut);

    if (!GdbCaptureHasContent(outPath))
        return false;
    g_lastGdbCapture = std::chrono::steady_clock::now();
    return true;
}

void WriteHangDump(const char *reason)
{
    if (g_crashLogPath.empty())
        return;
    const std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));

    const char *chain = "";
    (void)calltrace::guiChainRaw(&chain);

    char buf[768];
    if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
        AppendLine(f, "---- GUI HANG (watchdog) ----");
        std::snprintf(buf, sizeof(buf), "at=%s pid=%lu", ts,
                      static_cast<unsigned long>(GetCurrentProcessId()));
        AppendLine(f, buf);
        if (reason != nullptr && reason[0] != '\0') {
            std::snprintf(buf, sizeof(buf), "  reason: %s", reason);
            AppendLine(f, buf);
        }
        std::snprintf(buf, sizeof(buf), "  last-call: %s",
                      chain[0] != '\0'
                          ? chain
                          : "(none instrumented — the stall is outside the traced entry points)");
        AppendLine(f, buf);
        std::fclose(f);
    }

    // The dump sits beside the crash log: hang-<pid>-<time>.dmp. The TIME is part of the name: two
    // stalls in one run (the usual case — a freeze and then a second one) used to share
    // hang-<pid>.dmp, so the second report overwrote the first one's evidence.
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H%M%S", std::localtime(&now));
    const std::string hangTag = std::to_string(GetCurrentProcessId()) + "-" + stamp;
    std::string dumpPath = g_crashLogPath;
    const auto slash = dumpPath.rfind('/');
    if (slash != std::string::npos)
        dumpPath = dumpPath.substr(0, slash + 1) + "hang-" + hangTag + ".dmp";

    using FnMiniDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                      PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION,
                                      PMINIDUMP_CALLBACK_INFORMATION);
    bool wroteDump = false;
    if (HMODULE dbgDump = LoadLibraryA("dbghelp.dll")) {
        const auto miniDump = reinterpret_cast<FnMiniDump>(
            reinterpret_cast<void *>(GetProcAddress(dbgDump, "MiniDumpWriteDump")));
        if (miniDump) {
            if (HANDLE hDump = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                hDump != INVALID_HANDLE_VALUE) {
                const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
                wroteDump = miniDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                                     type, nullptr, nullptr, nullptr) == TRUE;
                CloseHandle(hDump);
            }
        }
    }
    if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
        std::snprintf(buf, sizeof(buf), "dump: %s (written=%s)",
                      dumpPath.c_str(), wroteDump ? "yes" : "no");
        AppendLine(f, buf);
        std::fclose(f);
    }

    std::string gdbOut;
    bool captured = false;
    {
        const auto sep = dumpPath.find_last_of("\\/");
        const std::string dir = (sep != std::string::npos) ? dumpPath.substr(0, sep + 1)
                                                           : std::string();
        gdbOut = dir + "hang-" + hangTag + "-gdb.txt";
        captured = SpawnGdbCapture(gdbOut);
        if (!captured)
            DeleteFileA(gdbOut.c_str());
    }
    if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
        if (captured)
            std::snprintf(buf, sizeof(buf), "gdb capture: %s (every thread, symbolized)",
                          gdbOut.c_str());
        else
            std::snprintf(buf, sizeof(buf),
                          "gdb capture: FAILED — no symbolized stacks (tools\\gdb could not start, "
                          "a capture was already running, or none was resolvable). The dump above "
                          "still holds every thread's stack; check the bundled gdb with "
                          "\"tools\\gdb\\gdb.exe --batch -ex \"show version\"\".");
        AppendLine(f, buf);
        std::fclose(f);
    }
}
#else
void WriteHangDump(const char *reason)
{
    if (g_crashLogPath.empty())
        return;
    if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a")) {
        AppendLine(f, "---- GUI HANG (watchdog) ----");
        AppendLine(f, reason != nullptr ? reason : "GUI thread stalled");
        AppendBreadcrumb(f);
        std::fclose(f);
    }
}
#endif

// ---------------------------------------------------------------------------
// The micro-stall sampler — ALWAYS ON (BPS_GUI_STALL_MS=<ms> changes the threshold, 0 turns it off; default 100).
// The hang watchdog above reports a freeze of SECONDS and names the traced call it was in; a 300 ms hitch on a
// camera take is far below that and usually outside every traced entry point. This samples WHERE the interface
// thread actually is: when it has not ticked for the threshold, its stack is captured a few times DURING the
// stall (module + offset per frame, plus the symbol where the OS can name it), and one line says how long the
// stall lasted when it ends. The same loop runs on every OS; only the capture differs:
//   Windows  suspend the thread, unwind its saved context (RtlVirtualUnwind), resume - nothing allocates while it
//            is suspended (it might hold the heap lock).
//   Linux / macOS  a SIGUSR2 sent to that thread: its handler records backtrace() into a static buffer.
// Resolve a line afterwards against the exe: gdb -batch -ex "info symbol 0x<imagebase+rva>" appVGRPresenterUI.
// ---------------------------------------------------------------------------
namespace {

constexpr int kStallMaxFrames = 24;
constexpr int kStallMaxSamples = 6;   // per stall: enough to see what dominates, cheap enough not to perturb it

#if defined(_WIN32)

// Captures the interface thread's stack. The thread handle is opened once and kept.
int CaptureGuiStack(unsigned long long nativeId, unsigned long long *frames, int maxFrames)
{
    static HANDLE gui = nullptr;
    if (!gui) {
        gui = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                         static_cast<DWORD>(nativeId));
        if (!gui)
            return 0;
    }
    int count = 0;
    // ---- suspended: nothing below may allocate, log or take a lock ----
    if (SuspendThread(gui) != static_cast<DWORD>(-1)) {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(gui, &ctx)) {
            for (; count < maxFrames && ctx.Rip != 0; ++count) {
                frames[count] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!entry) {   // a leaf with no unwind data: the return address is on top of the stack
                    ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData,
                                     &establisher, nullptr);
                }
            }
        }
        ResumeThread(gui);
    }
    return count;
}

std::string DescribeFrame(unsigned long long address)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};
    unsigned long long rva = address;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module) {
        GetModuleFileNameA(module, name, MAX_PATH);
        rva = address - reinterpret_cast<unsigned long long>(module);
    }
    const char *base = std::strrchr(name, '\\');
    char one[128];
    std::snprintf(one, sizeof(one), " %s+0x%llx", base ? base + 1 : (name[0] ? name : "?"), rva);
    return one;
}

#else   // Linux and macOS

std::atomic<int> g_stallCount{-1};
void *g_stallFrames[kStallMaxFrames];

extern "C" void vgrStallSignalHandler(int)
{
    // async-signal context: backtrace() into a static buffer, publish the count, nothing else.
    void *local[kStallMaxFrames];
    const int n = backtrace(local, kStallMaxFrames);
    for (int i = 0; i < n; ++i)
        g_stallFrames[i] = local[i];
    g_stallCount.store(n, std::memory_order_release);
}

int CaptureGuiStack(unsigned long long nativeId, unsigned long long *frames, int maxFrames)
{
    static bool installed = false;
    if (!installed) {
        // The first backtrace() may load libgcc / allocate: warm it up here, outside any signal handler.
        void *warm[4];
        (void)backtrace(warm, 4);
        struct sigaction action = {};
        action.sa_handler = vgrStallSignalHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (sigaction(SIGUSR2, &action, nullptr) != 0)
            return 0;
        installed = true;
    }
    g_stallCount.store(-1, std::memory_order_release);
#if defined(__APPLE__)
    const pthread_t target = reinterpret_cast<pthread_t>(static_cast<uintptr_t>(nativeId));   // a pointer type there
#else
    const pthread_t target = static_cast<pthread_t>(nativeId);                                // an integer type here
#endif
    if (pthread_kill(target, SIGUSR2) != 0)
        return 0;
    for (int waited = 0; waited < 40; ++waited) {   // up to ~40 ms for the thread to run its handler
        const int n = g_stallCount.load(std::memory_order_acquire);
        if (n >= 0) {
            const int count = n < maxFrames ? n : maxFrames;
            for (int i = 0; i < count; ++i)
                frames[i] = reinterpret_cast<unsigned long long>(g_stallFrames[i]);
            return count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

std::string DescribeFrame(unsigned long long address)
{
    Dl_info info = {};
    char one[256];
    if (dladdr(reinterpret_cast<void *>(address), &info) != 0 && info.dli_fname) {
        const char *base = std::strrchr(info.dli_fname, '/');
        const unsigned long long offset = address - reinterpret_cast<unsigned long long>(info.dli_fbase);
        std::snprintf(one, sizeof(one), " %s+0x%llx%s%s", base ? base + 1 : info.dli_fname, offset,
                      info.dli_sname ? ":" : "", info.dli_sname ? info.dli_sname : "");
    } else {
        std::snprintf(one, sizeof(one), " 0x%llx", address);
    }
    return one;
}

#endif

} // namespace

void StartLoopProfiler()
{
#if defined(_WIN32)
    static std::atomic<bool> started{false};
    static std::atomic<unsigned long> loopTid{0};
    if (started.exchange(true))
        return;
    bps::live::LiveOutputController::SetLoopStartHook([] { loopTid.store(GetCurrentThreadId()); });
    std::thread([] {
        constexpr int kFrames = 14;
        std::unordered_map<std::string, int> counts;
        std::string key;
        HANDLE handle = nullptr;
        unsigned long handleFor = 0;
        int total = 0, idle = 0;
        auto lastReport = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            const unsigned long tid = loopTid.load();
            if (tid != 0) {
                if (handleFor != tid) {
                    if (handle) CloseHandle(handle);
                    handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                    handleFor = tid;
                }
                unsigned long long frames[kFrames];
                int count = 0;
                if (handle && SuspendThread(handle) != static_cast<DWORD>(-1)) {
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(handle, &ctx)) {
                        for (; count < kFrames && ctx.Rip != 0; ++count) {
                            frames[count] = ctx.Rip;
                            DWORD64 imageBase = 0;
                            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                            if (!entry) {
                                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                                ctx.Rsp += 8;
                            } else {
                                PVOID handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData,
                                                 &establisher, nullptr);
                            }
                        }
                    }
                    ResumeThread(handle);
                }
                if (count > 0) {
                    ++total;
                    key.clear();
                    char buf[24];
                    for (int i = 0; i < count; ++i) {
                        std::snprintf(buf, sizeof buf, "%llx ", frames[i]);
                        key += buf;
                    }
                    ++counts[key];
                }
            }
            if (std::chrono::steady_clock::now() - lastReport >= std::chrono::seconds(15) && total > 0) {
                lastReport = std::chrono::steady_clock::now();
                std::vector<std::pair<int, std::string>> top;
                for (const auto &kv : counts) top.emplace_back(kv.second, kv.first);
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                qInfo("LiveProf: %d samples in 15 s (every ~3 ms); %d distinct stacks", total, static_cast<int>(top.size()));
                for (size_t i = 0; i < top.size() && i < 30; ++i) {
                    std::string line;
                    unsigned long long addr;
                    const char *p = top[i].second.c_str();
                    int shown = 0;
                    while (*p && shown < 10) {
                        addr = std::strtoull(p, nullptr, 16);
                        line += DescribeFrame(addr);
                        while (*p && *p != ' ') ++p;
                        while (*p == ' ') ++p;
                        ++shown;
                    }
                    qInfo("LiveProf: %5d x%s", top[i].first, line.c_str());
                }
                counts.clear();
                total = idle = 0;
            }
        }
    }).detach();
#endif
}

void StartGuiStallSampler(int thresholdMs)
{
    static std::atomic<bool> started{false};
    if (thresholdMs <= 0 || started.exchange(true))
        return;
    std::thread([thresholdMs] {
        bool inStall = false;
        int samples = 0;
        double worstMs = 0.0;
        std::string lastCall;
        auto lastSample = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (calltrace::heartbeatTickCount() < 20)
                continue;   // still starting
            const long long beat = calltrace::lastHeartbeatTicks();
            const auto nowPoint = std::chrono::steady_clock::now();
            const long long now = nowPoint.time_since_epoch().count();
            const double stalledMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::duration(now - beat)).count();
            if (stalledMs < thresholdMs) {
                if (inStall) {   // it is ticking again: one line for the whole stall
                    // qInfo, not qWarning: warnings are surfaced to the operator as toasts, and a
                    // diagnostic about the interface freezing must not itself pop up on the interface.
                    qInfo("GuiStall: the interface thread was silent for about %.0f ms%s", worstMs,
                             lastCall.empty() ? "" : (" - last traced call: " + lastCall).c_str());
                    inStall = false;
                    worstMs = 0.0;
                }
                continue;
            }
            if (!inStall) {
                inStall = true;
                samples = 0;
                worstMs = 0.0;
                lastCall.clear();
            }
            worstMs = std::max(worstMs, stalledMs);
            if (samples >= kStallMaxSamples
                || std::chrono::duration<double, std::milli>(nowPoint - lastSample).count() < 15.0)
                continue;
            const unsigned long long tid = calltrace::guiNativeThreadId();
            if (tid == 0)
                continue;
            unsigned long long frames[kStallMaxFrames] = {};
            const int count = CaptureGuiStack(tid, frames, kStallMaxFrames);
            lastSample = std::chrono::steady_clock::now();
            if (count <= 0)
                continue;
            ++samples;
            if (lastCall.empty())
                lastCall = calltrace::guiChain().toStdString();
            std::string text;
            for (int i = 0; i < count; ++i)
                text += DescribeFrame(frames[i]);
            qInfo("GuiStall: interface thread silent for %.0f ms - sample %d:%s", stalledMs, samples, text.c_str());
        }
    }).detach();
}

QString ConsumePendingCrashSummary()
{
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
        const size_t lineEnd = end == std::string::npos ? contents.size() : end;
        summary = QString::fromStdString(contents.substr(pos, lineEnd - pos));
        summary = summary.trimmed();
        size_t callPos = contents.find("  last-call: ", lineEnd);
        if (callPos == std::string::npos)
            callPos = contents.find("  last-call: ", pos);
        if (callPos != std::string::npos) {
            const size_t callStart = callPos + std::strlen("  last-call: ");
            const size_t callEnd = contents.find('\n', callStart);
            summary += QStringLiteral(" | last call in flight: ")
                       + QString::fromStdString(contents.substr(
                             callStart, (callEnd == std::string::npos ? contents.size() : callEnd) - callStart)).trimmed();
        }
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
    const QString dir = dataroot::appDataRoot() + QStringLiteral("/crashes");
    QDir().mkpath(dir);
    g_crashLogPath = (dir + QStringLiteral("/crash.log")).toStdString();

#ifdef _WIN32
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
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(HandleSEH);
    AddVectoredExceptionHandler(1, HandleHeapCorruption);
#else
    std::signal(SIGSEGV, HandleSignal);
    std::signal(SIGFPE, HandleSignal);
    std::signal(SIGILL, HandleSignal);
#endif
}

// ---- startup self-check of the shipped forensics tooling --------------------

namespace {
// The dist's layout, relative to the exe (what tools/make_portable.sh ships).
constexpr const char *kGdbRel = "tools/gdb/gdb.exe";
constexpr const char *kAnalyzeRel = "analyze.bat";

QString ForensicsRoot()
{
    const QString dir = qEnvironmentVariable("VGR_FORENSICS_DIR");
    return dir.isEmpty() ? QCoreApplication::applicationDirPath() : dir;
}

bool GdbAnswers(const QString &gdb, QString *why)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(gdb, {QStringLiteral("--batch"), QStringLiteral("-ex"),
                     QStringLiteral("show version")});
    if (!proc.waitForStarted(5000)) {
        *why = QStringLiteral("could not be started");
        return false;
    }
    if (!proc.waitForFinished(10000)) {
        proc.kill();
        proc.waitForFinished(2000);
        *why = QStringLiteral("did not answer within 10 s (killed)");
        return false;
    }
    const QString out = QString::fromLocal8Bit(proc.readAll());
    if (!out.contains(QStringLiteral("GNU gdb"))) {
        const int code = static_cast<int>(proc.exitCode());
        const QString hex = QStringLiteral("0x%1")
                                .arg(static_cast<quint32>(code), 8, 16, QLatin1Char('0'));
        *why = code < 0
                   ? QStringLiteral("does not load (exit %1 — a required DLL is missing or corrupt)").arg(hex)
                   : QStringLiteral("does not report a gdb version (exit %1)").arg(hex);
        return false;
    }
    return true;
}

QString ForensicsToolingProblems()
{
    QStringList problems;
    const QDir root(ForensicsRoot());

    const QString bundledGdb = root.filePath(QString::fromLatin1(kGdbRel));
    const QString analyze = root.filePath(QString::fromLatin1(kAnalyzeRel));
    const bool hasBundledGdb = QFileInfo::exists(bundledGdb);
    const bool hasAnalyze = QFileInfo::exists(analyze);
    // A dist carries the gdb folder and analyze.bat; a dev tree carries neither.
    // Only a dist is expected to be complete.
    const bool packaged = hasBundledGdb || hasAnalyze;

    // 1. The gdb a capture would actually attach, in ResolveGdb()'s order:
    //    override, then the bundled copy, then PATH.
    const QString fromEnv = qEnvironmentVariable("VGR_GDB_BIN");
    QString gdb;
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        gdb = fromEnv;
    else if (hasBundledGdb)
        gdb = bundledGdb;
    else
        gdb = QStandardPaths::findExecutable(QStringLiteral("gdb"));

    QString why;
    if (gdb.isEmpty()) {
        if (packaged)
            problems << QStringLiteral("no gdb: %1 is missing and none is on PATH, "
                                       "so a hang capture would produce no stack")
                            .arg(QString::fromLatin1(kGdbRel));
    } else if (!GdbAnswers(gdb, &why)) {
        problems << QStringLiteral("the gdb a capture would use (%1) %2, "
                                   "so a hang capture would produce no stack")
                        .arg(QDir::toNativeSeparators(gdb), why);
    }

    // 2. The one double-click that reads a report without developer tools.
    //    Expected in a dist, missing by construction in a dev tree.
    if (packaged && !hasAnalyze)
        problems << QStringLiteral("%1 is missing beside the app, so a report could not be read "
                                   "on a machine with no developer tools")
                        .arg(QString::fromLatin1(kAnalyzeRel));
    else if (hasAnalyze && QFileInfo(analyze).size() == 0)
        problems << QStringLiteral("%1 is empty, so a report could not be read")
                        .arg(QString::fromLatin1(kAnalyzeRel));

    // 3. The dump writer, resolved the same way the hang path resolves it.
#ifdef _WIN32
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        const bool hasWriter = GetProcAddress(dbg, "MiniDumpWriteDump") != nullptr;
        FreeLibrary(dbg);
        if (!hasWriter)
            problems << QStringLiteral("dbghelp.dll has no MiniDumpWriteDump, "
                                       "so no hang minidump could be written");
    } else {
        problems << QStringLiteral("dbghelp.dll cannot be loaded, "
                                   "so no hang minidump could be written");
    }
#endif

    // 4. The folder every artifact goes into. InstallCrashHandler() ignores
    if (!g_crashLogPath.empty()) {
        if (FILE *f = std::fopen(g_crashLogPath.c_str(), "a"))
            std::fclose(f);
        else
            problems << QStringLiteral("the crash folder is not writable (%1), "
                                       "so nothing would be recorded at all")
                            .arg(QFileInfo(QString::fromStdString(g_crashLogPath)).absolutePath());
    }

    return problems.isEmpty() ? QString() : problems.join(QStringLiteral("; "));
}
} // namespace

void VerifyForensicsTooling()
{
    // An explicit off-switch, for a packaging/CI smoke launch that must not pay
    // a process spawn. The normal path never sets it.
    if (qEnvironmentVariableIsSet("VGR_SKIP_FORENSICS_CHECK")) {
        EngineBridge::instance().publishCrashTooling(QStringLiteral("skipped"), QString());
        return;
    }

    QThread *worker = QThread::create([] {
        const QString problems = ForensicsToolingProblems();
        QMetaObject::invokeMethod(
            qApp,
            [problems] {
                if (problems.isEmpty())
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Forensics"),
                                        QStringLiteral("crash tooling ready: a crash or hang from this run "
                                                       "can be captured and read"));
                else
                    EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Forensics"),
                                        QStringLiteral("crash tooling is INCOMPLETE — ") + problems);
                EngineBridge::instance().publishCrashTooling(
                    problems.isEmpty() ? QStringLiteral("ready") : QStringLiteral("incomplete"),
                    problems.isEmpty() ? QString() : problems);
            },
            Qt::QueuedConnection);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}