#include "modules/presentation/PresentationDocument.hpp"

#include "core/common/Common.hpp"
#include "modules/vgr/VgrFile.hpp"

#include <chrono>
#include <set>

namespace bps::presentation {

namespace {

constexpr const char* kModule = "PresentationDocument";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// A stable id for a brand-new show. (Presentations opened from a file keep the
// id stored in it.)
std::string NewShowId() {
    return "show-" + std::to_string(NowMs());
}

} // namespace

Result<void> PresentationDocument::Open(const std::string& path) {
    if (path.empty())
        return Error::Make(Err::InvalidArgument, kModule, "no file path given");
    // VgrFile::Read verifies magic, version and every section's CRC32 — a
    // truncated or damaged file is rejected here, before the current document is
    // touched.
    auto doc = vgr::VgrFile::Read(path);
    if (!doc.ok()) return doc.error();
    const vgr::DocumentType type = doc.value().header.type;
    if (type != vgr::DocumentType::Show && type != vgr::DocumentType::Presentation)
        return Error::Make(Err::InvalidArgument, kModule,
                           std::string("this .vgr file is a ") + vgr::ToString(type) +
                           " document, not a show");

    auto parsed = serializer_.Deserialize(doc.value().documentJson);
    if (!parsed.ok()) return parsed.error();

    Presentation p = std::move(parsed.value());
    // The container header is the file's identity of record; keep the model's
    // own fields consistent with it when the body left them blank.
    if (p.id.empty()) p.id = doc.value().header.uuid;
    if (p.name.empty()) p.name = doc.value().header.name;

    std::lock_guard<std::mutex> lock(mutex_);
    model_ = std::move(p);
    path_ = path;
    open_ = true;
    dirty_ = false;
    ++revision_;
    return Ok();
}

Result<void> PresentationDocument::Save(const std::string& requestedPath) {
    Presentation snapshot;
    std::string target;
    uint64_t savedRevision = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            return Error::Make(Err::InvalidState, kModule, "no document is open");
        target = requestedPath.empty() ? path_ : requestedPath;
        snapshot = model_;
        savedRevision = revision_;
    }
    if (target.empty())
        return Error::Make(Err::InvalidArgument, kModule,
                           "this show has never been saved — a file path is required");

    snapshot.path = target;
    snapshot.modifiedAt = std::chrono::system_clock::now();
    if (snapshot.createdAt.time_since_epoch().count() == 0) snapshot.createdAt = snapshot.modifiedAt;
    if (snapshot.id.empty()) snapshot.id = NewShowId();

    auto body = serializer_.Serialize(snapshot);
    if (!body.ok()) return body.error();

    vgr::VgrDocument doc;
    doc.header.engineVersion = kEngineVersion.ToString();
    doc.header.type = vgr::DocumentType::Show;
    doc.header.uuid = snapshot.id;
    doc.header.name = snapshot.name;
    doc.header.createdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               snapshot.createdAt.time_since_epoch()).count();
    doc.header.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                snapshot.modifiedAt.time_since_epoch()).count();
    // Dependency manifest: every asset any slide references (docs/specs/22).
    std::set<std::string> deps;
    for (const auto& s : snapshot.slides)
        deps.insert(s.assetIds.begin(), s.assetIds.end());
    doc.header.dependencyIds.assign(deps.begin(), deps.end());
    doc.header.searchMetadata["slides"] = std::to_string(snapshot.slides.size());
    // Library search reads these without opening the show body.
    std::string categoryNames;
    for (const auto& c : snapshot.categories) {
        if (!categoryNames.empty()) categoryNames += ", ";
        categoryNames += c.name;
    }
    doc.header.searchMetadata["categories"] = categoryNames;
    doc.documentJson = std::move(body.value());

    // Verified, crash-safe write (see VgrFile): one complete file on disk at every
    // instant, the previous one restored if the swap fails.
    if (auto w = vgr::VgrFile::Write(target, doc); !w.ok()) return w.error();

    std::lock_guard<std::mutex> lock(mutex_);
    path_ = target;
    // Saving is not instantaneous: if the frontend pushed newer edits while the
    // file was being written, the file holds the OLD state — keep the newer model
    // and leave the document dirty rather than silently marking those edits saved.
    if (revision_ == savedRevision) {
        model_ = std::move(snapshot);
        dirty_ = false;
    }
    return Ok();
}

Result<void> PresentationDocument::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    model_ = Presentation{};
    path_.clear();
    open_ = false;
    dirty_ = false;
    return Ok();
}

bool PresentationDocument::IsDirty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirty_;
}

void PresentationDocument::New(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_ = Presentation{};
    model_.id = NewShowId();
    model_.name = std::move(name);
    model_.createdAt = model_.modifiedAt = std::chrono::system_clock::now();
    path_.clear();
    open_ = true;
    dirty_ = true;   // never saved yet
    ++revision_;
}

void PresentationDocument::Replace(Presentation presentation) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_ = std::move(presentation);
    open_ = true;
    dirty_ = true;
    ++revision_;
}

Result<void> PresentationDocument::Edit(const std::function<Result<void>(Presentation&)>& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_)
        return Error::Make(Err::InvalidState, kModule, "no show is open");
    Presentation working = model_;
    if (auto r = fn(working); !r.ok()) return r;   // refused: the model is untouched
    working.modifiedAt = std::chrono::system_clock::now();
    model_ = std::move(working);
    dirty_ = true;
    ++revision_;
    return Ok();
}

void PresentationDocument::MarkClean() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = false;
}

void PresentationDocument::MarkDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) {
        dirty_ = true;
        ++revision_;
    }
}

Presentation PresentationDocument::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_;
}

std::string PresentationDocument::Path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

bool PresentationDocument::HasDocument() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

} // namespace bps::presentation
