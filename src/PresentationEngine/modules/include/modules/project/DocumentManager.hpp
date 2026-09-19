#pragma once

// DocumentManager (docs/specs/15 §Document Manager): everything editable is a
// document. Hosts a registry of IDocumentHandler per document type and tracks
// per-document state: open, dirty, locked, read-only. Open/Close follow the
// standard workflow; document content work is delegated to handlers.

#include "modules/project/IDocumentHandler.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

class DocumentManager {
public:
    static DocumentManager& Instance();

    // --- Handler registry (extension point) ---
    Result<void> RegisterHandler(std::shared_ptr<IDocumentHandler> handler);
    Result<void> UnregisterHandler(std::string_view type);
    std::vector<std::string> HandlerTypes() const;

    // --- Document lifecycle ---
    // Open a document of `type` from `path`; generates a document id.
    Result<std::string> Open(std::string_view type, std::string_view path);
    Result<void> Save(std::string_view docId);
    Result<void> Close(std::string_view docId);
    Result<void> Lock(std::string_view docId, bool locked);
    bool IsLocked(std::string_view docId) const;
    Result<void> SetDirty(std::string_view docId, bool dirty);
    bool IsDirty(std::string_view docId) const;
    Result<void> SetReadOnly(std::string_view docId, bool readOnly);
    bool IsReadOnly(std::string_view docId) const;
    std::string DocumentPath(std::string_view docId) const;
    std::string DocumentType(std::string_view docId) const;

    size_t OpenCount() const;

private:
    DocumentManager() = default;

    struct Document {
        std::string id;
        std::string type;
        std::string path;
        std::shared_ptr<IDocumentHandler> handler;
        bool dirty = false;
        bool locked = false;
        bool readOnly = false;
    };
    std::shared_ptr<Document> FindLocked(std::string_view id) const;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<IDocumentHandler>, std::less<>> handlers_;  // type → handler
    std::map<std::string, std::shared_ptr<Document>, std::less<>> documents_;         // id → doc
    uint64_t nextId_ = 1;
};

} // namespace bps::project
