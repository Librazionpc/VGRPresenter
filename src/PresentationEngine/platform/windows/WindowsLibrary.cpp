#include "platform/windows/WindowsLibrary.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>

namespace bps::platform {

Result<ILibrary::Handle> WindowsLibrary::Load(std::string_view path) {
    HMODULE h = LoadLibraryA(std::string(path).c_str());
    if (!h) {
        std::string err = win::LastErrorString(GetLastError());
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "LoadLibraryA failed for '" + std::string(path) + "': " + err);
    }
    SetLastError({});
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_.insert(h);
    return h;
}

Result<void> WindowsLibrary::Unload(Handle handle) {
    if (!handle) return Ok();
    if (!FreeLibrary(static_cast<HMODULE>(handle))) {
        std::string err = win::LastErrorString(GetLastError());
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library", "FreeLibrary failed: " + err);
    }
    SetLastError({});
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_.erase(handle);
    return Ok();
}

Result<void*> WindowsLibrary::Symbol(Handle handle, std::string_view name) {
    if (!handle) return Error::Make(Err::InvalidArgument, "Library", "null library handle");
    FARPROC sym = GetProcAddress(static_cast<HMODULE>(handle), std::string(name).c_str());
    if (!sym) {
        std::string err = win::LastErrorString(GetLastError());
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "GetProcAddress '" + std::string(name) + "' failed: " + err);
    }
    SetLastError({});
    return reinterpret_cast<void*>(sym);
}

bool WindowsLibrary::IsLoaded(Handle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_.count(handle) > 0;
}

std::string WindowsLibrary::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

Result<ILibrary::Handle> WindowsLibrary::Reload(Handle handle, std::string_view path) {
    if (auto r = Unload(handle); !r.ok()) return r.error();
    return Load(path);
}

void WindowsLibrary::SetLastError(std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::string(message);
}

} // namespace bps::platform
