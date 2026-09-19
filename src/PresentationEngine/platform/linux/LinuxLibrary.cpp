#include "platform/linux/LinuxLibrary.hpp"

#include <dlfcn.h>

namespace bps::platform {

namespace {
// dlerror() returns AND clears the last error; capture it exactly once per op.
std::string TakeDlError() {
    const char* e = dlerror();
    return e ? std::string(e) : std::string();
}
} // namespace

Result<ILibrary::Handle> LinuxLibrary::Load(std::string_view path) {
    dlerror(); // clear
    void* h = dlopen(std::string(path).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::string err = TakeDlError();
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "dlopen failed for '" + std::string(path) + "': " + err);
    }
    SetLastError({});
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_.insert(h);
    return h;
}

Result<void> LinuxLibrary::Unload(Handle handle) {
    if (!handle) return Ok();
    dlerror();
    if (dlclose(handle) != 0) {
        std::string err = TakeDlError();
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library", "dlclose failed: " + err);
    }
    SetLastError({});
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_.erase(handle);
    return Ok();
}

Result<void*> LinuxLibrary::Symbol(Handle handle, std::string_view name) {
    if (!handle) return Error::Make(Err::InvalidArgument, "Library", "null library handle");
    dlerror();
    void* sym = dlsym(handle, std::string(name).c_str());
    const char* err = dlerror();
    if (err) {
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "dlsym '" + std::string(name) + "' failed: " + std::string(err));
    }
    SetLastError({});
    return sym;
}

bool LinuxLibrary::IsLoaded(Handle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_.count(handle) > 0;
}

std::string LinuxLibrary::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void LinuxLibrary::SetLastError(std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::string(message);
}

Result<ILibrary::Handle> LinuxLibrary::Reload(Handle handle, std::string_view path) {
    if (auto r = Unload(handle); !r.ok()) return r.error();
    return Load(path);
}

} // namespace bps::platform
