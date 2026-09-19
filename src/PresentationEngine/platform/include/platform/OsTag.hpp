#pragma once

// Compile-time platform tags (PAL). The core never uses #if _WIN32/__linux__/
// __APPLE__ directly (PAL DoD §1 — no OS knowledge outside the PAL); when the
// binary identity is needed (e.g. BuildInfo) it is read from here instead.
// This header is the single macro home of the project (DoD §25).
//
// Architecture names are canonical across every OS:
//   x86_64 · x86 · arm64 · arm · riscv64
// so a Windows ARM64 build, an Apple Silicon build and a Linux aarch64 build
// all report "arm64"; a Win32 x86 build, an i686 Linux build and a macOS x86
// build all report "x86" (32-bit ARM reports "arm").

#include <string>
#include <string_view>

#if defined(__linux__) && !defined(NDEBUG)
#include <cstdlib>
#include <execinfo.h>
#endif

namespace bps::platform {

inline constexpr const char* kCompileOs =
#if defined(_WIN32)
    "windows"
#elif defined(__APPLE__)
    "macos"
#elif defined(__linux__)
    "linux"
#else
    "unknown"
#endif
    ;

// Canonical compile-time architecture — covers every supported target:
// Windows x86/x64/ARM64/ARM, Linux/macOS x86_64/arm64/arm, plus riscv64.
inline constexpr const char* CompileArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_ARM) || defined(__arm__) || defined(__thumb__)
    return "arm";
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__) || defined(__i686__)
    return "x86";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unknown";
#endif
}

inline constexpr const char* kCompileArch = CompileArch();

// Pointer width of this build (32 for Win32 x86 / Linux armv7, 64 for every
// 64-bit target) — matches sizeof(void*) * 8 at runtime.
inline constexpr int kCompileBits = static_cast<int>(sizeof(void*) * 8);

// Canonicalizes an OS-reported architecture string ("aarch64", "armv7l",
// "AMD64", ...) into the canonical form used across the engine, so Arch() and
// EnvironmentInfo.arch are consistent on every platform.
inline std::string CanonicalArch(std::string_view raw) {
    if (raw == "aarch64" || raw == "arm64" || raw == "ARM64") return "arm64";
    if (raw == "arm" || raw == "ARM" || raw == "armv6l" || raw == "armv7l" ||
        raw == "armv8l" || raw == "armhf")
        return "arm";
    if (raw == "x86_64" || raw == "amd64" || raw == "AMD64") return "x86_64";
    if (raw == "x86" || raw == "i386" || raw == "i486" || raw == "i586" || raw == "i686")
        return "x86";
    if (raw == "riscv64") return "riscv64";
    return std::string(raw);   // passthrough for anything unrecognized
}

// Debug-build call-site stack capture for Error.stack (DoD §19). Empty string
// in release builds and on platforms without a backtrace API. glibc only;
// Windows/macOS capture is a documented follow-up.
inline std::string CaptureStack() {
#if defined(__linux__) && !defined(NDEBUG)
    void* frames[16] = {};
    int n = ::backtrace(frames, 16);
    if (n <= 0) return {};
    char** symbols = ::backtrace_symbols(frames, n);
    if (!symbols) return {};
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += symbols[i];
        out += '\n';
    }
    ::free(symbols);
    return out;
#else
    return {};
#endif
}

} // namespace bps::platform
