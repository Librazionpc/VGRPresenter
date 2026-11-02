#pragma once

#include "core/common/Common.hpp"

#include <string>

namespace bps::project {
class IDocumentHandler {
public:
    virtual ~IDocumentHandler() = default;

    // Stable document type id, e.g. "presentation", "song", "theme".
    virtual const char* DocumentType() const noexcept = 0;

    virtual Result<void> Open(const std::string& path) = 0;
    virtual Result<void> Save(const std::string& path) = 0;
    virtual Result<void> Close() = 0;
    virtual bool IsDirty() const = 0;
};
} // namespace bps::project