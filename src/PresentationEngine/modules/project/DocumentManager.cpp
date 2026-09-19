#include "modules/project/DocumentManager.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>

namespace bps::project {

DocumentManager& DocumentManager::Instance() {
    static DocumentManager instance;
    return instance;
}

Result<void> DocumentManager::RegisterHandler(std::shared_ptr<IDocumentHandler> handler) {
    if (!handler)
        return Error::Make(Err::InvalidArgument, "DocumentManager", "null handler");
    std::lock_guard<std::mutex> lock(mutex_);
    std::string type(handler->DocumentType());
    if (handlers_.count(type))
        return Error::Make(Err::AlreadyExists, "DocumentManager", "handler for '" + type + "' exists");
    handlers_[type] = std::move(handler);
    return Ok();
}

Result<void> DocumentManager::UnregisterHandler(std::string_view type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handlers_.erase(std::string(type)))
        return Error::Make(Err::NotFound, "DocumentManager", "no handler for '" + std::string(type) + "'");
    return Ok();
}

std::vector<std::string> DocumentManager::HandlerTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [type, h] : handlers_) {
        (void)h;
        out.push_back(type);
    }
    return out;
}

Result<std::string> DocumentManager::Open(std::string_view type, std::string_view path) {
    std::shared_ptr<IDocumentHandler> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handlers_.find(type);
        if (it == handlers_.end())
            return Error::Make(Err::Unsupported, "DocumentManager",
                               "no handler for document type '" + std::string(type) + "'");
        handler = it->second;
    }
    if (auto r = handler->Open(std::string(path)); !r.ok()) return r.error();

    std::lock_guard<std::mutex> lock(mutex_);
    auto doc = std::make_shared<Document>();
    doc->id = std::format("doc-{}", nextId_++);
    doc->type = std::string(type);
    doc->path = std::string(path);
    doc->handler = handler;
    documents_[doc->id] = doc;
    Logger::Instance().Info("Document opened: " + doc->id + " (" + doc->type + ")", "DocumentManager");
    return Result<std::string>{doc->id};
}

Result<void> DocumentManager::Save(std::string_view docId) {
    auto doc = FindLocked(docId);
    if (!doc)
        return Error::Make(Err::Project_NotFound, "DocumentManager", "no such document");
    if (doc->readOnly)
        return Error::Make(Err::Project_DocumentLocked, "DocumentManager",
                           "document is read-only");
    if (doc->locked)
        return Error::Make(Err::Project_DocumentLocked, "DocumentManager", "document is locked");
    if (auto r = doc->handler->Save(doc->path); !r.ok()) return r.error();
    doc->dirty = false;
    return Ok();
}

Result<void> DocumentManager::Close(std::string_view docId) {
    auto doc = FindLocked(docId);
    if (!doc)
        return Error::Make(Err::Project_NotFound, "DocumentManager", "no such document");
    if (auto r = doc->handler->Close(); !r.ok()) return r.error();
    std::lock_guard<std::mutex> lock(mutex_);
    documents_.erase(std::string(docId));
    return Ok();
}

Result<void> DocumentManager::Lock(std::string_view docId, bool locked) {
    auto doc = FindLocked(docId);
    if (!doc)
        return Error::Make(Err::Project_NotFound, "DocumentManager", "no such document");
    doc->locked = locked;
    return Ok();
}

bool DocumentManager::IsLocked(std::string_view docId) const {
    auto doc = FindLocked(docId);
    return doc && doc->locked;
}

Result<void> DocumentManager::SetDirty(std::string_view docId, bool dirty) {
    auto doc = FindLocked(docId);
    if (!doc)
        return Error::Make(Err::Project_NotFound, "DocumentManager", "no such document");
    doc->dirty = dirty;
    return Ok();
}

bool DocumentManager::IsDirty(std::string_view docId) const {
    auto doc = FindLocked(docId);
    return doc && doc->dirty;
}

Result<void> DocumentManager::SetReadOnly(std::string_view docId, bool readOnly) {
    auto doc = FindLocked(docId);
    if (!doc)
        return Error::Make(Err::Project_NotFound, "DocumentManager", "no such document");
    doc->readOnly = readOnly;
    return Ok();
}

bool DocumentManager::IsReadOnly(std::string_view docId) const {
    auto doc = FindLocked(docId);
    return doc && doc->readOnly;
}

std::string DocumentManager::DocumentPath(std::string_view docId) const {
    auto doc = FindLocked(docId);
    return doc ? doc->path : std::string{};
}

std::string DocumentManager::DocumentType(std::string_view docId) const {
    auto doc = FindLocked(docId);
    return doc ? doc->type : std::string{};
}

size_t DocumentManager::OpenCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return documents_.size();
}

std::shared_ptr<DocumentManager::Document> DocumentManager::FindLocked(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = documents_.find(id);
    return it == documents_.end() ? nullptr : it->second;
}

} // namespace bps::project
