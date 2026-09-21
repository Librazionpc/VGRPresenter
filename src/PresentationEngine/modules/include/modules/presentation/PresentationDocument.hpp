#pragma once

// PresentationDocument (docs/architecture/ProjectSystem.md §Document lifecycle,
// docs/specs/22): the IDocumentHandler for the "presentation" document type —
// a show/presentation that opens from and saves to a native .vgr file.
//
//   DocumentManager -> IDocumentHandler -> PresentationDocument
//                                              |-- PresentationSerializer (JSON body)
//                                              '-- VgrSerializer (container: header,
//                                                  CRC32, dependency manifest)
//
// It owns ONE working presentation (the show being edited) and its dirty flag.
// The frontend never touches files or the container: it edits the model through
// the engine and asks for Save/Open, so any frontend (Qt, CLI, remote) gets the
// same file behaviour.
//
// Save is crash-safe: the new file is written next to the target, read back and
// CRC-verified, and only then swapped in (the old file is kept as a .bak until
// the swap succeeds and restored if it doesn't). A failed or interrupted save
// never leaves the user with a half-written show.

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
    // `path` is a .vgr file. Fails (leaving the current document untouched) when
    // the file is missing, corrupt (bad magic/CRC), a newer format, or not a
    // Show/Presentation document.
    Result<void> Open(const std::string& path) override;
    // Writes `path` (also becomes the document's path). Empty path = the path it
    // was opened from / last saved to.
    Result<void> Save(const std::string& path) override;
    Result<void> Close() override;
    bool IsDirty() const override;

    // --- Working model ---
    // A brand-new empty show ("New show"): one presentation, no path, dirty until
    // first saved so the frontend prompts before discarding it.
    void New(std::string name);
    // Replaces the model wholesale (a frontend pushing its edited state in).
    // Marks the document dirty.
    void Replace(Presentation presentation);
    // The engine-side way to change the show (ShowEditor operations: categories,
    // templates, overlays, slides). `fn` runs on a COPY of the model and is
    // committed — and the document marked dirty — only if it returns Ok, so a
    // multi-step edit is all-or-nothing and a refused one changes nothing.
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
    mutable std::mutex mutex_;
    Presentation model_;
    std::string path_;
    bool open_ = false;
    bool dirty_ = false;
    uint64_t revision_ = 0;   // bumped by every model change (see Save)
    PresentationSerializer serializer_;
};

} // namespace bps::presentation
