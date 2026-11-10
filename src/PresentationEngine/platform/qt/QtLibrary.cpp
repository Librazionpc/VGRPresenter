#include "platform/qt/QtServices.hpp"

#include <QLibrary>
#include <QString>

namespace bps::platform {

QtLibrary::QtLibrary() = default;

QtLibrary::~QtLibrary() {
    // The libraries stay mapped: a plugin's code may still be on some thread's stack at shutdown,
    // and the OS unmaps everything at process exit anyway. Only the bookkeeping objects go.
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_.clear();
}

Result<ILibrary::Handle> QtLibrary::Load(std::string_view path) {
    auto lib = std::make_unique<QLibrary>(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
    // The file is loaded as named: no version suffix guessing, symbols private to the library.
    if (!lib->load()) {
        const std::string err = lib->errorString().toStdString();
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "loading '" + std::string(path) + "' failed: " + err);
    }
    SetLastError({});
    Handle handle = lib.get();
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_[handle] = std::move(lib);
    return handle;
}

Result<void> QtLibrary::Unload(Handle handle) {
    if (!handle) return Ok();
    std::unique_ptr<QLibrary> lib;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = loaded_.find(handle);
        if (it == loaded_.end())
            return Error::Make(Err::InvalidArgument, "Library", "that library handle is not loaded");
        lib = std::move(it->second);
        loaded_.erase(it);
    }
    // Outside the lock: unloading runs the library's own teardown.
    if (!lib->unload()) {
        const std::string err = lib->errorString().toStdString();
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library", "unloading failed: " + err);
    }
    SetLastError({});
    return Ok();
}

Result<void*> QtLibrary::Symbol(Handle handle, std::string_view name) {
    if (!handle) return Error::Make(Err::InvalidArgument, "Library", "null library handle");
    QFunctionPointer sym = nullptr;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = loaded_.find(handle);
        if (it == loaded_.end())
            return Error::Make(Err::InvalidArgument, "Library", "that library handle is not loaded");
        sym = it->second->resolve(std::string(name).c_str());
        if (!sym) err = it->second->errorString().toStdString();
    }
    if (!sym) {
        SetLastError(err);
        return Error::Make(Err::Plugin_LoadFailed, "Library",
                           "symbol '" + std::string(name) + "' not found: " + err);
    }
    SetLastError({});
    return reinterpret_cast<void*>(sym);
}

bool QtLibrary::IsLoaded(Handle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_.count(handle) > 0;
}

std::string QtLibrary::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

Result<ILibrary::Handle> QtLibrary::Reload(Handle handle, std::string_view path) {
    if (auto r = Unload(handle); !r.ok()) return r.error();
    return Load(path);
}

void QtLibrary::SetLastError(std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::string(message);
}

} // namespace bps::platform
