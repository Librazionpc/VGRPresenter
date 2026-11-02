#pragma once

#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/project/IDocumentHandler.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace bps::presentation {
class PresentationDocument final : public bps::project::IDocumentHandler {
public:
    static constexpr const char* kType = "presentation";

    // --- IDocumentHandler ---
    const char* DocumentType() const noexcept override { return kType; }
    Result<void> Open(const std::string& path) override;
    // Writes `path` (also becomes the document's path). Empty path = the path it
    // was opened from / last saved to.
    Result<void> Save(const std::string& path) override;
    // Writes the show as it is NOW to `path` without making it the document's file: the path, the dirty flag and the
    // model stay as they were (backups and crash-recovery copies are written this way).
    Result<void> SaveCopy(const std::string& path);
    Result<void> Close() override;
    bool IsDirty() const override;

    void New(std::string name);
    // Replaces the model wholesale (a frontend pushing its edited state in).
    // Marks the document dirty.
    void Replace(Presentation presentation);
    Result<void> Edit(const std::function<Result<void>(Presentation&)>& fn);
    // The frontend edited the model outside the engine (e.g. a canvas edit):
    // flag the document modified so the UI can show it and prompt before discard.
    void MarkDirty();
    // The frontend has just built the document's initial state and there is nothing
    // worth saving yet (a brand-new show holding only its first empty slide).
    void MarkClean();
    Presentation Snapshot() const;   // copy of the current model
    std::string Path() const;        // "" until saved/opened
    bool HasDocument() const;

private:
    Result<void> WriteFile(Presentation& snapshot, const std::string& target);

    mutable std::mutex mutex_;
    Presentation model_;
    std::string path_;
    bool open_ = false;
    bool dirty_ = false;
    uint64_t revision_ = 0;   // bumped by every model change (see Save)
    PresentationSerializer serializer_;
};
} // namespace bps::presentation