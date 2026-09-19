#pragma once

// PAL dynamic library loader (Phase 2): the only way the engine loads shared
// libraries (.so / .dll / .dylib). PluginManager consumes this — it never calls
// dlopen/LoadLibrary directly. Handles are opaque pointers; never exposed to
// feature modules.

#include "core/common/Common.hpp"

#include <string>

namespace bps::platform {

class ILibrary {
public:
    using Handle = void*;

    virtual ~ILibrary() = default;

    virtual Result<Handle> Load(std::string_view path) = 0;
    virtual Result<void> Unload(Handle handle) = 0;
    virtual Result<void*> Symbol(Handle handle, std::string_view name) = 0;

    // Best-effort "is this handle still mapped" check (not guaranteed by every
    // OS; Linux tracks handles internally).
    virtual bool IsLoaded(Handle handle) const = 0;

    // Human-readable description of the last loader failure (dlopen/dlsym
    // errors). Empty string when the last operation succeeded.
    virtual std::string LastError() const = 0;

    // Unload `handle` then re-load `path`; returns the new handle.
    virtual Result<Handle> Reload(Handle handle, std::string_view path) = 0;
};

} // namespace bps::platform
