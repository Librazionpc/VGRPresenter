#include "platform/windows/WindowsThreading.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace bps::platform {

unsigned WindowsThreading::HardwareConcurrency() const {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
}

Result<void> WindowsThreading::SetCurrentThreadName(std::string_view name) {
    // SetThreadDescription is Win10 1607+; resolve dynamically so the binary
    // still loads on older systems (naming is then a no-op there).
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (!fn) return Ok();   // older Windows: naming unavailable, not an error
    std::wstring wide = win::Wide(name);
    HRESULT hr = fn(GetCurrentThread(), wide.c_str());
    if (FAILED(hr))
        return Error::Make(Err::IoError, "Threading", "SetThreadDescription failed");
    return Ok();
}

std::string WindowsThreading::CurrentThreadName() const {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto fn = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!fn) return {};
    PWSTR desc = nullptr;
    if (FAILED(fn(GetCurrentThread(), &desc)) || !desc) return {};
    std::string out = win::Utf8(desc);
    LocalFree(desc);
    return out;
}

uint64_t WindowsThreading::CurrentThreadId() const {
    return static_cast<uint64_t>(GetCurrentThreadId());
}

Result<void> WindowsThreading::SetCurrentThreadPriority(int priority) {
    if (priority == 0) return Ok();
    int p;
    switch (std::max(1, std::min(5, priority))) {
        case 1: p = THREAD_PRIORITY_LOWEST; break;
        case 2: p = THREAD_PRIORITY_BELOW_NORMAL; break;
        case 4: p = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case 5: p = THREAD_PRIORITY_HIGHEST; break;
        default: p = THREAD_PRIORITY_NORMAL; break;
    }
    if (SetThreadPriority(GetCurrentThread(), p) == 0)
        return Error::Make(Err::IoError, "Threading",
                           "SetThreadPriority failed: " + win::LastErrorString(GetLastError()));
    return Ok();
}

Result<void> WindowsThreading::SetThreadAffinity(std::thread& thread, unsigned cpu) {
    // DWORD_PTR is 32-bit on Win32 x86/ARM builds: shifting by >= 32 is UB, so
    // reject out-of-range CPUs for the current pointer width.
    if (cpu >= sizeof(DWORD_PTR) * 8)
        return Error::Make(Err::InvalidArgument, "Threading",
                           "cpu " + std::to_string(cpu) +
                               " out of range for a " + std::to_string(sizeof(DWORD_PTR) * 8) +
                               "-bit affinity mask");
    DWORD_PTR mask = DWORD_PTR(1) << cpu;
    // MinGW-w64's libstdc++ std::thread::native_handle_type is an integer
    // (the raw HANDLE value), not HANDLE itself like MSVC's — the numeric
    // value is a real Windows HANDLE either way, just needs the cast.
    HANDLE h = reinterpret_cast<HANDLE>(thread.native_handle());
    if (h == nullptr)
        return Error::Make(Err::InvalidArgument, "Threading", "invalid thread handle");
    if (SetThreadAffinityMask(h, mask) == 0)
        return Error::Make(Err::IoError, "Threading",
                           "SetThreadAffinityMask failed: " + win::LastErrorString(GetLastError()));
    return Ok();
}

} // namespace bps::platform
