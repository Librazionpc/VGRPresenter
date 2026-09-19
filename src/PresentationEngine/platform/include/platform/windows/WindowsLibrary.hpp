#pragma once

// Windows PAL backend for the dynamic library loader (LoadLibraryA /
// GetProcAddress / FreeLibrary).

#include "platform/ILibrary.hpp"

#include <mutex>
#include <unordered_set>

namespace bps::platform {

class WindowsLibrary final : public ILibrary {
public:
    Result<Handle> Load(std::string_view path) override;
    Result<void> Unload(Handle handle) override;
    Result<void*> Symbol(Handle handle, std::string_view name) override;
    bool IsLoaded(Handle handle) const override;
    std::string LastError() const override;
    Result<Handle> Reload(Handle handle, std::string_view path) override;

private:
    void SetLastError(std::string_view message);

    mutable std::mutex mutex_;
    std::unordered_set<Handle> loaded_;
    std::string lastError_;
};

} // namespace bps::platform
