#include "services/CallTrace.h"
#include "services/CrashHandler.h"
#include "services/EngineBridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>   // std::strlen (the breadcrumb prefix in ConsumePendingCrashSummary)
#include <ctime>
#include <exception>
#include <string>
#include <vector>    // CreateProcess needs a mutable argv buffer

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

// The GUI thread's last entered breadcrumb (services/CallTrace.h), reached
// through guiChainRaw(): a crash handler may not allocate, lock or call into
// Qt, so this reads a buffer the GUI thread already published. Empty is a
// legitimate answer — the chain is "" until something instrumented runs — and
// it is reported as such rather than left blank, so a reader knows the absence
// is information (the fault is outside the traced entry points).
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
    // The call the GUI thread was inside when the process died: a frame list
    // only becomes a culprit once symbols are resolved offline, and an
    // exception's frames say nothing at all about which QML call site it was.
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

#if defined(_WIN32)

// ---- gdb self-attach (the symbolized stack, with no debugger installed) ----
// A Windows minidump cannot be read by the MinGW gdb (it only understands ELF
// core files, which is why `gdb app.exe -c x.dmp` answers "not a core dump"),
// and the dist has no WinDbg. So instead of writing a dump and hoping someone
// can open it, the app runs the very capture an agent would: it spawns gdb
// ATTACHED TO ITSELF (-p <pid> suspends every thread, walks them, detaches).
// The exe's MinGW/DWARF symbols are exactly what gdb resolves, so the resulting
// file is a fully symbolized per-thread backtrace — on a target PC that has
// only the app folder.

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

// ONE attach at a time, and not again inside this window. The attach SUSPENDS
// EVERY THREAD while gdb walks them, so a stall that persists must not spawn a
// fresh attach on every watchdog report — that converts one slow stall into a
// run of them. Observed before this guard: five hang records minutes apart in a
// single session (5.2 s, 20.4 s, 5.5 s, 5.2 s, 5.9 s), each one re-freezing the
// very GUI thread it was trying to diagnose.
std::atomic<bool> g_gdbCaptureInFlight{false};
std::chrono::steady_clock::time_point g_lastGdbCapture{};
constexpr auto kGdbCaptureCooldown = std::chrono::seconds(60);

// A capture is only a capture if it has CONTENT. The file exists either way:
// CreateFileA creates it before gdb is even launched, so an empty one is a
// decoy, not evidence.
bool GdbCaptureHasContent(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return size > 0;
}

// Returns true only when gdb actually wrote a backtrace; false means the caller
// got no symbolized stacks (gdb missing, unable to start, still running from a
// previous hang, or inside the cooldown).
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
    // SEM_FAILCRITICALERRORS FOR THE SPAWN ONLY. A gdb that cannot load its own
    // DLLs would otherwise raise a MODAL loader dialog ("The code execution
    // cannot proceed because ... was not found") in the operator's face — which
    // is exactly what a broken bundled gdb produced, instead of the hang
    // capture it was spawned for, on top of the frozen UI it was meant to
    // explain. The child inherits this process's error mode, so this makes the
    // failure silent on its side; the log line below then reports it honestly.
    const UINT savedErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    if (CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                       TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        // Bounded: gdb walks + loads the (large) symbol table, then detaches.
        // The wait runs on the watchdog thread, which gdb suspends along with
        // every other thread and resumes on detach, so this returns normally.
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

// A HANG is not a crash: nothing faulted, so the SEH/signal handlers never run
// and Windows eventually closes the ghosted window (Event Log Id 1002). The
// only durable evidence is a snapshot taken WHILE the process is intact, from
// the watchdog's own (healthy) thread — a full-thread minidump plus a text
// record naming the stall. See CrashHandler.h.
void WriteHangDump(const char *reason)
{
    if (g_crashLogPath.empty())
        return;
    const std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));

    const char *chain = "";
    (void)calltrace::guiChainRaw(&chain);

    // Text record FIRST: it is tiny and survives even if the (much larger)
    // dump write fails on a corrupted heap — the same ordering rule the SEH
    // path follows.
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

    // The dump sits beside the crash log: hang-<pid>.dmp.
    std::string dumpPath = g_crashLogPath;
    const auto slash = dumpPath.rfind('/');
    if (slash != std::string::npos)
        dumpPath = dumpPath.substr(0, slash + 1) + "hang-"
                   + std::to_string(GetCurrentProcessId()) + ".dmp";

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
                // MiniDumpNormal = every thread's stack + module list (what gdb
                // needs to walk each thread); WithThreadInfo adds the TEB, so a
                // reader can name which thread is the GUI one.
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

    // The READABLE half: gdb attached to ourselves, writing symbolized
    // per-thread backtraces beside the dump. This is what actually names the
    // blocked call on a machine with no debugger — see SpawnGdbCapture.
    std::string gdbOut;
    bool captured = false;
    {
        const auto sep = dumpPath.find_last_of("\\/");
        const std::string dir = (sep != std::string::npos) ? dumpPath.substr(0, sep + 1)
                                                           : std::string();
        gdbOut = dir + "hang-" + std::to_string(GetCurrentProcessId()) + "-gdb.txt";
        captured = SpawnGdbCapture(gdbOut);
        // No capture → no artifact. Leaving the empty file behind is worse than
        // leaving none: the old record reported "present=yes" on the strength of
        // a file gdb had not written a byte to, so a session whose bundled gdb
        // could not even start read as if the capture had worked.
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
        const size_t lineEnd = end == std::string::npos ? contents.size() : end;
        summary = QString::fromStdString(contents.substr(pos, lineEnd - pos));
        summary = summary.trimmed();
        // The BREADCRUMB rides along with the summary. It is the one line that
        // says which call the process died in, and the summary is what reaches
        // the operator — the QML banner AND (see main.cpp) the engine log. The
        // raw record keeps everything, but nobody reads a crash file unless
        // they already know something went wrong.
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

// ---- startup self-check of the shipped forensics tooling --------------------
// See CrashHandler.h. The point is that every failure here is SILENT until the
// moment evidence is needed, so the check RUNS the chain instead of looking at
// it. Each probe mirrors the real path deliberately: the gdb is chosen in the
// same order ResolveGdb() chooses it, dbghelp is loaded the way the hang path
// loads it, and the crash folder is opened for append the way the handlers open
// it. If one of those moves, this must move with it.

namespace {

// The dist's layout, relative to the exe (what tools/make_portable.sh ships).
constexpr const char *kGdbRel = "tools/gdb/gdb.exe";
constexpr const char *kAnalyzeRel = "analyze.bat";

// The folder the shipped tools sit in: beside the exe, or wherever
// VGR_FORENSICS_DIR points (a packaged build with its tools elsewhere, or a
// deliberately broken layout to exercise this check).
QString ForensicsRoot()
{
    const QString dir = qEnvironmentVariable("VGR_FORENSICS_DIR");
    return dir.isEmpty() ? QCoreApplication::applicationDirPath() : dir;
}

// Runs a candidate gdb ONCE and insists it ANSWERS — the same proof
// tools/make_portable.sh runs before shipping one, because the failure it
// catches is invisible otherwise: a gdb that cannot load its own runtime prints
// no error, it dies at process start (and the capture spawn suppresses the
// loader dialog that would otherwise say so). `why` always receives a reason.
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
        // An NTSTATUS (negative) exit code means the process never reached
        // main() — the loader refused it, which on a packaged build is the
        // broken-bundle failure and nothing else: gdb's own runtime DLL is
        // missing (0xC0000135, STATUS_DLL_NOT_FOUND) or the copy in the bundle
        // dir is not a usable image (0xC000007B/0xC0000020). Name it, because
        // "gdb did not run" is not something a support log can act on.
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

// Every problem at once, so ONE log line is enough to act on. Empty means the
// tooling works (the healthy case, and also a dev tree that ships no tools —
// the text record and minidump remain, which is a working configuration).
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
        // A dev tree with no gdb is not a fault: the hang path falls back to the
        // text record plus the minidump, which is exactly what it does on a
        // machine with no debugger installed.
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
    //    mkpath()'s result (a read-only profile or a denied folder is silent),
    //    so probe it the way the handlers do — append-open. "a" creates the file
    //    if it is absent, which is exactly what the first crash would do, and
    //    nothing is discarded.
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
        // And SAY so, rather than leaving the UI on "checking" forever: an
        // unasked question is a different answer from an unanswered one, and
        // the Settings row shows them differently (see EngineBridge's
        // crashToolingState property).
        EngineBridge::instance().publishCrashTooling(QStringLiteral("skipped"), QString());
        return;
    }

    // On a WORKER: the gdb probe SPAWNS A PROCESS AND WAITS for it, which must
    // not happen on the thread that draws (see tools/check_gui_thread_blocks.mjs).
    // The lambda captures nothing — it must not touch this translation unit's
    // state from another thread.
    QThread *worker = QThread::create([] {
        const QString problems = ForensicsToolingProblems();
        // Answer from the INTERFACE thread: one line, on the thread (and in the
        // log) everything else reports through. qApp is the context object, so
        // Qt drops the call if the app is already tearing down.
        QMetaObject::invokeMethod(
            qApp,
            [problems] {
                if (problems.isEmpty())
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Forensics"),
                                        QStringLiteral("crash tooling ready: a crash or hang from this run "
                                                       "can be captured and read"));
                else
                    // "warning", not "warn": EngineBridge::write() maps a level
                    // it does not know down to Info, so the wrong word here
                    // would bury the one line this check exists to produce.
                    EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Forensics"),
                                        QStringLiteral("crash tooling is INCOMPLETE — ") + problems);
                // The SAME verdict, to the UI (EngineBridge's crashTooling*
                // properties): the log line is for whoever opens the log, and a
                // crash that produced no evidence is read by nobody. Published
                // here, on the interface thread, which is the only thread that
                // touches those members (see the header).
                EngineBridge::instance().publishCrashTooling(
                    problems.isEmpty() ? QStringLiteral("ready") : QStringLiteral("incomplete"),
                    problems.isEmpty() ? QString() : problems);
            },
            Qt::QueuedConnection);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
