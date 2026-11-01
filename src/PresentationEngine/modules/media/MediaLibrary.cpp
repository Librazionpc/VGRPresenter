#include "modules/media/MediaLibrary.hpp"

#include "core/config/Json.hpp"
#include "core/recovery/DocumentRecovery.hpp"
#include "modules/search/TextMatching.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>

namespace bps::media {

namespace {
namespace textmatch = bps::search::textmatch;

constexpr const char* kModule = "MediaLibrary";

// A runaway scan (a whole drive added by mistake) must not fill memory.
constexpr size_t kMaxItemsPerFolder = 20000;

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Path comparison key: one separator style, no trailing slash, case-folded.
std::string Norm(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return Lower(path);
}

std::string LeafName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

std::string StripExtension(std::string name) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

// "img2" sorts before "img10": digit runs compare as numbers, everything else
// case-insensitively.
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            // Ignore leading zeros, then a longer number is bigger.
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            const size_t la = ie - is, lb = je - js;
            if (la != lb) return la < lb;
            const int c = a.compare(is, la, b, js, lb);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return (a.size() - i) < (b.size() - j);
}

// The item list's order (the one Scan() stores and Items() hands out): natural name
// order, then the path as the tie-breaker. Shared so an item that arrives by a MOVE
// takes its place in the destination folder's order instead of landing at the end of
// the grid.
bool ItemLess(const LibraryItem& a, const LibraryItem& b) {
    if (NaturalLess(a.name, b.name)) return true;
    if (NaturalLess(b.name, a.name)) return false;
    return a.path < b.path;
}

std::string ParentDir(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

std::vector<std::string> Components(const std::string& path) {
    std::vector<std::string> out;
    std::string part;
    for (char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (!part.empty()) out.push_back(std::move(part)), part.clear();
        } else {
            part.push_back(ch);
        }
    }
    if (!part.empty()) out.push_back(std::move(part));
    return out;
}

} // namespace

std::string MediaPathHash(std::string_view path) {
    // FreeShow's filePathHashCode: hash = hash * 31 + char, wrapped to 32 bits.
    uint32_t hash = 0;
    for (char raw : path) {
        const unsigned char c = static_cast<unsigned char>(raw == '\\' ? '/' : raw);
        hash = hash * 31u + c;
    }
    const int32_t signedHash = static_cast<int32_t>(hash);
    if (signedHash < 0) return "i" + std::to_string(-static_cast<int64_t>(signedHash));
    return "a" + std::to_string(signedHash);
}

std::optional<MediaKind> MediaKindOf(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return std::nullopt;
    const std::string ext = Lower(path.substr(dot + 1));
    // What is listed is what the library shows; whether a picture can be made of it depends on the
    // decoders the machine has (a file nothing can decode still appears, with a placeholder tile).
    // Audio files are listed too (their "picture" is their cover art, when they have one).
    for (const char* e : {"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif", "webp", "tif", "tiff",
                          "ico", "heic", "heif", "avif", "svg", "jp2", "tga"})
        if (ext == e) return MediaKind::Image;
    for (const char* e : {"mp4", "m4v", "mov", "mkv", "avi", "webm", "wmv", "asf", "flv", "f4v", "mpg",
                          "mpeg", "mpe", "m2v", "ts", "m2ts", "mts", "3gp", "3g2", "ogv", "vob", "divx", "mxf"})
        if (ext == e) return MediaKind::Video;
    for (const char* e : {"mp3", "wav", "wave", "flac", "aac", "m4a", "m4b", "ogg", "oga", "opus", "wma", "aif",
                          "aiff", "aifc", "mka", "amr", "ac3", "weba"})
        if (ext == e) return MediaKind::Audio;
    return std::nullopt;
}

MediaLibrary::MediaLibrary(std::string storageFile) : storageFile_(std::move(storageFile)) {
    const size_t cut = storageFile_.find_last_of("/\\");
    const std::string dir = cut == std::string::npos ? std::string() : storageFile_.substr(0, cut + 1);
    indexFile_ = dir + "media-index.json";
    // The per-file fills live in their OWN document: the index is rebuilt from
    // the disk by every scan (and is the big one), while a fit is a choice the
    // operator made and must survive a rescan untouched.
    fitsFile_ = dir + "media-fits.json";
}

// The PAL half: resolve the path (a filesystem call, so callers run this with
// NO lock held).
std::string MediaLibrary::CanonicalKey(std::string_view path) const {
    // An empty query matches nothing. Without this guard the lookup below
    // would compare an empty key against every stored folder — and any folder
    // whose normalised form is also empty would be returned as a match.
    if (path.empty()) return {};
    auto absolute = platform::PlatformAccessor::Get().Filesystem().Absolute(path);
    return Norm(absolute.ok() ? absolute.value() : std::string(path));
}

// The in-memory half: the library's own spelling of the folder with this key.
std::string MediaLibrary::MatchKey(const std::string& key) const {
    if (key.empty()) return {};
    for (const std::string& f : order_)
        if (Norm(f) == key) return f;
    return {};
}

Result<void> MediaLibrary::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();

    // THE PER-FILE FILLS, read first and OUTSIDE every lock (a PAL read): this
    // document is independent of the folder list and of the index, and a damaged
    // one is simply ignored — the files keep their output's default, which is
    // exactly "nobody chose one".
    std::map<std::string, std::string, std::less<>> storedFits;
    if (fs.Exists(fitsFile_)) {
        if (auto fitsText = fs.ReadText(fitsFile_); fitsText.ok()) {
            if (auto parsedFits = json::Parse(fitsText.value()); parsedFits.ok()) {
                if (const auto* object = parsedFits.value().Find("fits")
                                            ? parsedFits.value().Find("fits")->asObject()
                                            : nullptr) {
                    for (const auto& [path, value] : *object) {
                        const std::string fit(value.asString());
                        if (fit == "contain" || fit == "cover" || fit == "fill")
                            storedFits[Norm(path)] = fit;
                    }
                }
            }
        }
    }

    if (!fs.Exists(storageFile_)) {
        std::lock_guard<std::mutex> lock(mutex_);
        fits_ = std::move(storedFits);
        return {};
    }
    auto text = fs.ReadText(storageFile_);
    if (!text.ok()) return text.error();
    auto parsed = json::Parse(text.value());
    // A parse failure AND a document with no 'folders' array are the same loss:
    // nothing loadable is in there, and reading it as "no folders" then saving
    // over it is how the bytes disappear.
    const std::string problem =
        parsed.ok() ? recovery::DocumentShapeProblem(parsed.value(), "folders", json::Value::Type::Array)
                    : parsed.error().message;
    if (!problem.empty()) {
        // SELF-HEALING: the folder list is an internal document; set the damaged
        // file aside as evidence and load no folders, so the Media pane opens on
        // an empty (re-scannable) library instead of failing every read.
        (void)recovery::SetAsideDamagedDocument(storageFile_, kModule, problem);
        std::lock_guard<std::mutex> lock(mutex_);
        order_.clear();
        items_.clear();
        scanned_.clear();
        fits_ = std::move(storedFits);   // a lost folder list loses no FILL
        return {};
    }

    std::vector<std::string> loadedOrder;
    std::map<std::string, std::vector<LibraryItem>, std::less<>> loadedItems;
    if (const auto* list = parsed.value().Find("folders"))
        if (const auto* arr = list->asArray())
            for (const auto& v : *arr) {
                const std::string path(v.asString());
                if (path.empty()) continue;
                const std::string key = Norm(path);
                const bool known = std::any_of(loadedOrder.begin(), loadedOrder.end(),
                                               [&](const std::string& f) { return Norm(f) == key; });
                if (!known) loadedOrder.push_back(path);
            }

    // The remembered index (what was in each folder last time). It is an
    // internal document like the folder list above, so a damaged one is set
    // aside as evidence and the library comes up unscanned — the first Scan()
    // of each folder rebuilds it — instead of the bytes being overwritten by a
    // save the operator never got told about.
    if (fs.Exists(indexFile_)) {
        auto indexText = fs.ReadText(indexFile_);
        if (indexText.ok()) {
            auto index = json::Parse(indexText.value());
            const std::string indexProblem =
                index.ok() ? recovery::DocumentShapeProblem(index.value(), "folders", json::Value::Type::Object)
                           : index.error().message;
            if (!indexProblem.empty()) {
                (void)recovery::SetAsideDamagedDocument(indexFile_, kModule, indexProblem);
            } else {
                const auto* object = index.value().Find("folders")->asObject();
                for (const std::string& folder : loadedOrder) {
                    auto entry = object->find(folder);
                    if (entry == object->end()) continue;
                    const auto* list = entry->second.asArray();
                    if (!list) continue;
                    std::vector<LibraryItem> restored;
                    restored.reserve(list->size());
                    for (const auto& row : *list) {
                        LibraryItem item;
                        item.path = std::string(row.Find("p") ? row.Find("p")->asString() : "");
                        const auto kind = MediaKindOf(item.path);
                        if (item.path.empty() || !kind) continue;
                        item.id = MediaPathHash(item.path);
                        item.name = StripExtension(LeafName(item.path));
                        item.kind = *kind;
                        item.folder = folder;
                        item.sizeBytes = static_cast<uint64_t>(row.Find("s") ? row.Find("s")->asInt() : 0);
                        item.modifiedMs = static_cast<int64_t>(row.Find("m") ? row.Find("m")->asInt() : 0);
                        restored.push_back(std::move(item));
                    }
                    loadedItems[folder] = std::move(restored);
                }
            }
        }
    }

    // Commit the fully loaded snapshot only after every filesystem operation.
    // Readers never wait behind a slow index read or damaged-file recovery.
    std::lock_guard<std::mutex> lock(mutex_);
    order_ = std::move(loadedOrder);
    items_ = std::move(loadedItems);
    scanned_.clear();
    fits_ = std::move(storedFits);
    return {};
}

std::string MediaLibrary::BuildIndexLocked() const {
    json::Value::Object byFolder;
    for (const std::string& folder : order_) {
        auto it = items_.find(folder);
        if (it == items_.end()) continue;
        json::Value::Array rows;
        rows.reserve(it->second.size());
        for (const LibraryItem& item : it->second) {
            json::Value::Object row;
            row["p"] = json::Value::String(item.path);
            row["s"] = json::Value::Number(static_cast<double>(item.sizeBytes));
            row["m"] = json::Value::Number(static_cast<double>(item.modifiedMs));
            rows.push_back(json::Value(std::move(row)));
        }
        byFolder[folder] = json::Value(std::move(rows));
    }
    json::Value::Object root;
    root["folders"] = json::Value(std::move(byFolder));
    return json::Value(std::move(root)).ToString();
}

std::string MediaLibrary::BuildFitsJsonLocked() const {
    json::Value::Object byPath;
    for (const auto& [key, fit] : fits_) byPath[key] = json::Value::String(fit);
    json::Value::Object root;
    root["fits"] = json::Value(std::move(byPath));
    return json::Value(std::move(root)).ToString();
}

void MediaLibrary::SaveFits(const std::string& text) const {
    if (fitsFile_.empty()) return;
    // ATOMIC, like the index: a truncated document would silently lose every
    // choice it held. Best-effort by signature — the fit is a preference.
    (void)platform::PlatformAccessor::Get().Filesystem().WriteAtomic(fitsFile_, text);
}

void MediaLibrary::SaveIndex(const std::string& text) const {
    // ATOMIC: the media index is the only record of the user's folders and
    // metadata; a truncated one loses the whole library view. Best-effort by
    // signature, but the swap still guarantees a readable file.
    (void)platform::PlatformAccessor::Get().Filesystem().WriteAtomic(indexFile_, text);
}

std::string MediaLibrary::BuildFoldersJsonLocked() const {
    json::Value::Array folders;
    for (const std::string& f : order_) folders.push_back(json::Value::String(f));
    json::Value::Object root;
    root["folders"] = json::Value(std::move(folders));
    return json::Value(std::move(root)).ToString();
}

Result<void> MediaLibrary::CommitFolders(const std::string& json) const {
    if (storageFile_.empty() || json.empty()) return {};
    // ATOMIC: same document class as the index above (folder order, metadata).
    // PAL: callers call this AFTER releasing mutex_ (it used to be Save(), called
    // from inside AddFolder's and RemoveFolder's locked scopes).
    return platform::PlatformAccessor::Get().Filesystem().WriteAtomic(storageFile_, json);
}

Result<void> MediaLibrary::AddFolder(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (path.empty() || !fs.IsDirectory(path))
        return Error::Make(Err::InvalidArgument, kModule, std::format("'{}' is not a folder", path));
    auto absolute = fs.Absolute(path);
    std::string folder = absolute.ok() ? absolute.value() : std::string(path);
    while (folder.size() > 3 && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    const std::string key = Norm(folder);   // no PAL left to do under the lock
    std::string json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchKey(key).empty())
            return Error::Make(Err::AlreadyExists, kModule, "that folder is already in the library");
        order_.push_back(folder);
        json = BuildFoldersJsonLocked();
    }
    if (auto saved = CommitFolders(json); !saved.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find(order_.begin(), order_.end(), folder);
        if (it != order_.end()) order_.erase(it);
        return saved.error();
    }
    return {};
}

Result<void> MediaLibrary::RemoveFolder(std::string_view path) {
    // The path resolution (PAL) and both writes (PAL) happen with the lock
    // RELEASED; only the map edits above them are under it.
    const std::string key = CanonicalKey(path);
    std::string json, index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string folder = MatchKey(key);
        if (folder.empty())
            return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
        order_.erase(std::find(order_.begin(), order_.end(), folder));
        items_.erase(folder);
        scanned_.erase(folder);
        json = BuildFoldersJsonLocked();
        index = BuildIndexLocked();
    }
    auto saved = CommitFolders(json);
    SaveIndex(index);
    return saved;
}

std::vector<LibraryFolder> MediaLibrary::Folders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryFolder> out;
    for (const std::string& f : order_) {
        LibraryFolder folder;
        folder.path = f;
        folder.name = LeafName(f).empty() ? f : LeafName(f);
        if (auto it = items_.find(f); it != items_.end()) folder.count = it->second.size();
        if (auto it = scanned_.find(f); it != scanned_.end()) folder.scanned = it->second;
        out.push_back(std::move(folder));
    }
    return out;
}

Result<size_t> MediaLibrary::Scan(std::string_view path) {
    // Empty path: CanonicalKey()/MatchKey() answer {} for it, which the check
    // below reports as "not in the library". Stated explicitly so the intent is
    // readable — an empty folder name is not a library folder.
    if (path.empty())
        return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
    const std::string key = CanonicalKey(path);   // PAL, resolved before any lock
    std::string folder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folder = MatchKey(key);
    }
    if (folder.empty())
        return Error::Make(Err::NotFound, kModule, "that folder is not in the library");

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::vector<LibraryItem> found;
    if (fs.IsDirectory(folder)) {
        auto files = fs.FindFiles(folder, "");
        if (!files.ok()) return files.error();
        for (const std::string& file : files.value()) {
            const auto kind = MediaKindOf(file);
            if (!kind) continue;
            if (found.size() >= kMaxItemsPerFolder) break;
            LibraryItem item;
            item.path = file;
            item.id = MediaPathHash(file);
            item.name = StripExtension(LeafName(file));
            item.kind = *kind;
            item.folder = folder;
            if (auto meta = fs.Metadata(file); meta.ok()) {
                item.sizeBytes = meta.value().sizeBytes;
                item.modifiedMs = meta.value().modifiedEpochNs / 1000000;
            }
            found.push_back(std::move(item));
        }
        std::sort(found.begin(), found.end(), ItemLess);
    }   // a folder that has gone missing is scanned as empty

    const size_t count = found.size();
    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (MatchKey(key).empty())   // removed while it was being scanned
            return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
        items_[folder] = std::move(found);
        scanned_[folder] = true;
        index = BuildIndexLocked();
    }
    SaveIndex(index);   // written outside the lock: it can be a few MB for a big library
    return count;
}

// ---------------------------------------------------------------------------
// The FILES themselves: rename, delete, move
// ---------------------------------------------------------------------------
// One shape for all three, and each half is where it is on purpose: the PAL
// (resolve the path, rename/delete/move the file, read the new file's metadata)
// runs with NO lock held, and only the index re-key does — the same split this
// module's folder writes already use (BuildFoldersJsonLocked/CommitFolders).
bool MediaLibrary::FindItemLocked(const std::string& key, std::string& folderOut,
                                  size_t& indexOut) const {
    if (key.empty()) return false;
    for (const std::string& folder : order_) {
        auto it = items_.find(folder);
        if (it == items_.end()) continue;
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (Norm(it->second[i].path) == key) {
                folderOut = folder;
                indexOut = i;
                return true;
            }
        }
    }
    return false;
}

std::string MediaLibrary::ApplyItemChangeLocked(const std::string& oldKey,
                                                const LibraryItem* replacement) {
    // THE FILL FOLLOWS THE FILE. Rename and move re-key the item, and the fit
    // is about the FILE, not about where it sat: leaving it on the old key would
    // hand it to whatever is written at that path next. A permanent Remove drops
    // it (replacement == nullptr) — the file it described is gone.
    if (const auto held = fits_.find(oldKey); held != fits_.end()) {
        const std::string fit = held->second;
        fits_.erase(held);
        if (replacement)
            fits_[Norm(replacement->path)] = fit;
    }
    std::string folder;
    size_t index = 0;
    if (FindItemLocked(oldKey, folder, index)) {
        auto it = items_.find(folder);
        if (it != items_.end()) {
            if (!replacement) {
                it->second.erase(it->second.begin() + static_cast<std::ptrdiff_t>(index));
            } else if (Norm(replacement->folder) == Norm(folder)) {
                it->second[index] = *replacement;
            } else {
                // A MOVE into another library folder re-FILES the item: the list it
                // lives in is keyed by that folder, so leaving it in the old one
                // would keep the file showing where it no longer is (and hide it
                // where it now is).
                it->second.erase(it->second.begin() + static_cast<std::ptrdiff_t>(index));
                auto& target = items_[replacement->folder];
                target.push_back(*replacement);
                std::sort(target.begin(), target.end(), ItemLess);
            }
        }
    }
    return BuildIndexLocked();
}

std::string MediaLibrary::ContainingFolderLocked(const std::string& targetKey) const {
    if (targetKey.empty()) return {};
    if (const std::string exact = MatchKey(targetKey); !exact.empty())
        return exact;
    // A folder INSIDE one of them (the tree's own nested rows): its key extends a
    // library folder's by a separator.
    for (const std::string& f : order_) {
        const std::string fk = Norm(f);
        if (targetKey.size() > fk.size() && targetKey.compare(0, fk.size(), fk) == 0
                && targetKey[fk.size()] == '/')
            return f;
    }
    return {};
}

Result<void> MediaLibrary::RenameItem(std::string_view path, std::string_view newName) {
    std::string name(newName);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty())
        return Error::Make(Err::InvalidArgument, kModule, "a file needs a name");
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return Error::Make(Err::InvalidArgument, kModule,
                           "a file name cannot contain a path separator");
    if (name == "." || name == "..")
        return Error::Make(Err::InvalidArgument, kModule, "'" + name + "' is not a file name");

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string key = CanonicalKey(path);   // PAL, resolved before any lock
    LibraryItem current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string folder;
        size_t index = 0;
        if (!FindItemLocked(key, folder, index))
            return Error::Make(Err::NotFound, kModule, "that file is not in the library");
        current = items_.at(folder)[index];
    }
    // A name with no extension of its own KEEPS the file's: renaming a video must
    // not leave behind a file the library can no longer classify.
    if (fs.Extension(name).empty())
        name += fs.Extension(current.path);
    const std::string parent = ParentDir(current.path);
    const std::string newPath = parent.empty() ? name : fs.Join(parent, name);
    if (Norm(newPath) == key) return {};   // the same name — nothing to do, nothing to write
    if (fs.Exists(newPath))
        return Error::Make(Err::AlreadyExists, kModule,
                           std::format("'{}' is already in that folder", name));
    if (auto renamed = fs.Rename(current.path, newPath); !renamed.ok())
        return renamed.error();

    LibraryItem replacement;
    replacement.path = newPath;
    replacement.id = MediaPathHash(newPath);   // the item id IS the path hash
    replacement.name = StripExtension(name);
    replacement.kind = MediaKindOf(newPath).value_or(current.kind);
    replacement.folder = current.folder;       // a rename stays in its library folder
    if (auto meta = fs.Metadata(newPath); meta.ok()) {
        replacement.sizeBytes = meta.value().sizeBytes;
        replacement.modifiedMs = meta.value().modifiedEpochNs / 1000000;
    }
    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = ApplyItemChangeLocked(key, &replacement);
    }
    SaveIndex(index);   // PAL, outside the lock (a big library's index is a few MB)
    return {};
}

Result<void> MediaLibrary::RemoveItem(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string key = CanonicalKey(path);
    LibraryItem current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string folder;
        size_t index = 0;
        if (!FindItemLocked(key, folder, index))
            return Error::Make(Err::NotFound, kModule, "that file is not in the library");
        current = items_.at(folder)[index];
    }
    // The FILE goes; its cached pictures stay in the cache. They are keyed by the
    // file's path hash, so they are reused if the same file is put back, and the
    // cache is not the library's to clean.
    if (auto removed = fs.Remove(current.path); !removed.ok())
        return removed.error();
    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = ApplyItemChangeLocked(key, nullptr);
    }
    SaveIndex(index);
    return {};
}

// ---------------------------------------------------------------------------
// Undoable delete: the file goes into the library's own trash
// ---------------------------------------------------------------------------
namespace {

// A DELETE MUST SURVIVE LEAVING THE DRIVE. TrashItem puts the file in the
// library's trash, which lives in the app's data folder — and a rename cannot
// cross volumes (EXDEV), which is exactly the case of media kept on a second
// drive or a USB stick. So: rename, and when that fails, copy then delete. The
// copy path costs the file's size once on the way out and once on the way back,
// on a worker thread, and only ever for a delete the operator asked for.
Result<void> MoveFileAcross(platform::IFilesystem& fs, const std::string& from,
                            const std::string& to) {
    if (auto moved = fs.Move(from, to); moved.ok())
        return moved;
    if (auto copied = fs.Copy(from, to); !copied.ok())
        return copied.error();
    if (auto removed = fs.Remove(from); !removed.ok()) {
        (void)fs.Remove(to);   // do not leave a second copy behind
        return removed.error();
    }
    return {};
}

// How long a deleted file waits in the trash before the sweep removes it. The
// undo that makes the trash worth having lives for seconds; a week covers "I
// deleted this last night" without letting a dead library hold gigabytes.
constexpr int64_t kTrashRetentionMs = 7LL * 24 * 60 * 60 * 1000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string MediaLibrary::TrashDir() const {
    // Beside the library's own documents (media-folders.json, the index), and
    // deliberately NOT under any library folder: a scan walks folders
    // recursively, so trash inside one would list the file the operator just
    // deleted right back into the grid.
    const std::string parent = ParentDir(storageFile_);
    return parent.empty() ? std::string("media-trash")
                          : platform::PlatformAccessor::Get().Filesystem().Join(parent, "media-trash");
}

void MediaLibrary::SweepTrash() const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = TrashDir();
    auto entries = fs.Enumerate(dir);
    if (!entries.ok())
        return;   // no trash yet is the normal case
    const int64_t cutoff = NowMs() - kTrashRetentionMs;
    for (const std::string& entry : entries.value()) {
        // Only OUR names are ever touched: <epoch-ms>-<leaf>. Anything else in
        // there (a file the user dropped in) is not the sweep's to delete.
        const std::string leaf = LeafName(entry);
        const size_t dash = leaf.find('-');
        if (dash == std::string::npos || dash < 10) continue;
        int64_t stamp = 0;
        bool digits = true;
        for (size_t i = 0; i < dash; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(leaf[i]))) { digits = false; break; }
            stamp = stamp * 10 + (leaf[i] - '0');
        }
        if (!digits || stamp > cutoff) continue;
        (void)fs.Remove(entry);
    }
}

std::string MediaLibrary::InsertItemLocked(const LibraryItem& item) {
    auto it = items_.find(item.folder);
    if (it == items_.end())
        return {};   // its folder left the library: the file comes back, the row does not
    it->second.push_back(item);
    std::sort(it->second.begin(), it->second.end(), ItemLess);
    return BuildIndexLocked();
}

Result<std::string> MediaLibrary::TrashItem(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string key = CanonicalKey(path);
    LibraryItem current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string folder;
        size_t index = 0;
        if (!FindItemLocked(key, folder, index))
            return Error::Make(Err::NotFound, kModule, "that file is not in the library");
        current = items_.at(folder)[index];
    }

    SweepTrash();   // PAL, before this delete adds to what the sweep looks at
    const std::string dir = TrashDir();
    if (auto made = fs.CreateDirectories(dir); !made.ok())
        return made.error();
    // The name keeps the leaf (extension and all) so the trashed file is still
    // recognisably itself, and starts with the time so the sweep can date it.
    const std::string leaf = LeafName(current.path);
    const std::string base = fs.Join(dir, std::format("{}-{}", NowMs(), leaf));
    std::string token = base;
    for (int n = 2; fs.Exists(token) && n < 1000; ++n)
        token = std::format("{}-{}", base, n);

    if (auto moved = MoveFileAcross(fs, current.path, token); !moved.ok())
        return moved.error();

    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trash_[token] = TrashEntry{ token, current };
        index = ApplyItemChangeLocked(key, nullptr);
    }
    SaveIndex(index);   // PAL, outside the lock
    return token;
}

Result<std::string> MediaLibrary::RestoreTrashedItem(std::string_view token) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    TrashEntry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = trash_.find(std::string(token));
        if (it == trash_.end())
            return Error::Make(Err::NotFound, kModule,
                               "that deletion can no longer be undone");
        entry = it->second;
    }
    // THE WAY HAS TO BE CLEAR: a photo that came back while this one was deleted
    // must not be overwritten by the restore — the trashed copy stays where it is
    // and the same Undo works again once the name is free.
    if (fs.Exists(entry.item.path))
        return Error::Make(Err::AlreadyExists, kModule,
                           std::format("'{}' is back in that folder — the deleted copy stays in the trash",
                                       LeafName(entry.item.path)));
    if (!fs.Exists(entry.token))
        return Error::Make(Err::NotFound, kModule, "the deleted file is no longer in the trash");

    const std::string parent = ParentDir(entry.item.path);
    if (!parent.empty())
        if (auto made = fs.CreateDirectories(parent); !made.ok())
            return made.error();
    if (auto moved = MoveFileAcross(fs, entry.token, entry.item.path); !moved.ok())
        return moved.error();

    // The item comes back as it WAS (id, name, kind — the id is the path's hash, so
    // it is the same id it had), with the size and timestamp of the restored file.
    LibraryItem restored = entry.item;
    if (auto meta = fs.Metadata(restored.path); meta.ok()) {
        restored.sizeBytes = meta.value().sizeBytes;
        restored.modifiedMs = meta.value().modifiedEpochNs / 1000000;
    }
    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trash_.erase(std::string(token));
        index = InsertItemLocked(restored);
    }
    if (!index.empty())
        SaveIndex(index);
    return restored.path;
}

Result<void> MediaLibrary::MoveItem(std::string_view path, std::string_view targetFolder) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string key = CanonicalKey(path);
    const std::string targetKey = CanonicalKey(targetFolder);
    // The destination as the UI spelled it (the key above is folded for the
    // COMPARISON only): a move must never rewrite the folder's own name.
    std::string target(targetFolder);
    while (target.size() > 3 && (target.back() == '/' || target.back() == '\\')) target.pop_back();
    if (target.empty())
        return Error::Make(Err::InvalidArgument, kModule, "no folder to move it into");

    LibraryItem current;
    std::string root;
    bool knownFolder = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = ContainingFolderLocked(targetKey);
        if (root.empty())
            return Error::Make(Err::InvalidArgument, kModule, "that folder is not in the library");
        // A target that IS one of the library's own folders takes the library's own
        // spelling: a move must never mint a second spelling of a folder the
        // library already knows ("C:\\media" and "c:/media" would otherwise both
        // name a real folder and only one of them would match later lookups).
        knownFolder = !MatchKey(targetKey).empty();
        std::string folder;
        size_t index = 0;
        if (!FindItemLocked(key, folder, index))
            return Error::Make(Err::NotFound, kModule, "that file is not in the library");
        current = items_.at(folder)[index];
    }
    const std::string leaf = LeafName(current.path);
    const std::string newPath = fs.Join(knownFolder ? root : target, leaf);
    if (Norm(newPath) == key) return {};   // it is already in that folder
    if (fs.Exists(newPath))
        return Error::Make(Err::AlreadyExists, kModule,
                           std::format("'{}' is already in that folder", leaf));
    if (auto moved = fs.Move(current.path, newPath); !moved.ok())
        return moved.error();

    LibraryItem replacement;
    replacement.path = newPath;
    replacement.id = MediaPathHash(newPath);
    replacement.name = StripExtension(leaf);
    replacement.kind = MediaKindOf(newPath).value_or(current.kind);
    replacement.folder = root;   // the library folder it now lives under (may differ)
    if (auto meta = fs.Metadata(newPath); meta.ok()) {
        replacement.sizeBytes = meta.value().sizeBytes;
        replacement.modifiedMs = meta.value().modifiedEpochNs / 1000000;
    }
    std::string index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = ApplyItemChangeLocked(key, &replacement);
    }
    SaveIndex(index);
    return {};
}

std::vector<LibrarySubfolder> MediaLibrary::Subfolders(std::string_view libraryFolder) const {
    const std::string folderKey = CanonicalKey(libraryFolder);   // PAL, before the lock
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string root = MatchKey(folderKey);
    std::vector<LibrarySubfolder> out;
    auto it = items_.find(root);
    if (root.empty() || it == items_.end()) return out;

    // Every directory that has a media file somewhere beneath it, with how many. A file counts for its own
    // directory and for every directory between that one and the library folder.
    const std::string rootNorm = Norm(root);
    std::map<std::string, LibrarySubfolder> byKey;   // by normalised path
    for (const LibraryItem& item : it->second) {
        std::string dir = ParentDir(item.path);
        while (true) {
            const std::string key = Norm(dir);
            if (key.size() <= rootNorm.size() || key.compare(0, rootNorm.size(), rootNorm) != 0) break;   // reached the library folder
            auto slot = byKey.find(key);
            if (slot == byKey.end()) {
                LibrarySubfolder sub;
                sub.path = dir;
                std::replace(sub.path.begin(), sub.path.end(), '\\', '/');   // one separator style for the UI
                sub.name = LeafName(dir);
                sub.parent = ParentDir(sub.path);
                sub.depth = static_cast<int>(Components(key).size()) - static_cast<int>(Components(rootNorm).size());
                slot = byKey.emplace(key, std::move(sub)).first;
            }
            ++slot->second.count;
            dir = ParentDir(dir);
        }
    }
    for (auto& [key, sub] : byKey) out.push_back(std::move(sub));
    // Tree order: compare folder by folder (natural, case-insensitive), so a folder is always followed by
    // what is inside it - a plain string sort would put "a b" between "a" and "a/b".
    std::sort(out.begin(), out.end(), [](const LibrarySubfolder& a, const LibrarySubfolder& b) {
        const auto ca = Components(a.path), cb = Components(b.path);
        for (size_t i = 0; i < ca.size() && i < cb.size(); ++i) {
            if (NaturalLess(ca[i], cb[i])) return true;
            if (NaturalLess(cb[i], ca[i])) return false;
        }
        return ca.size() < cb.size();
    });
    return out;
}

Result<void> MediaLibrary::SetItemFit(std::string_view path, std::string_view fit) {
    const std::string value(fit);
    // "" is a legitimate answer (follow the output's own Fill); every other
    // spelling has to be one of the three the renderer knows.
    if (!value.empty() && value != "contain" && value != "cover" && value != "fill")
        return Error::Make(Err::InvalidArgument, kModule,
                           std::format("'{}' is not a fill (contain, cover, fill)", value));
    const std::string key = CanonicalKey(path);   // PAL, outside the lock
    std::string json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string folder;
        size_t index = 0;
        if (!FindItemLocked(key, folder, index))
            return Error::Make(Err::NotFound, kModule, "that file is not in the library");
        const std::string held = fits_.count(key) != 0 ? fits_[key] : std::string();
        if (held == value) return {};   // unchanged — nothing to write
        if (value.empty())
            fits_.erase(key);
        else
            fits_[key] = value;
        // The item's own copy too, so a caller holding an item it just changed
        // (and every Items() result) says the same thing as the store.
        if (auto it = items_.find(folder); it != items_.end())
            it->second[index].fit = value;
        json = BuildFitsJsonLocked();
    }
    SaveFits(json);   // PAL, outside the lock
    return {};
}

std::string MediaLibrary::ItemFit(std::string_view path) const {
    const std::string key = CanonicalKey(path);   // PAL, outside the lock
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fits_.find(key);
    return it == fits_.end() ? std::string() : it->second;
}

std::vector<LibraryItem> MediaLibrary::Items(std::string_view folder) const {
    // The PAL path resolution runs BEFORE the library lock: a filesystem call
    // must never sit under mutex_ (the lock-scope guard flags it, and a slow
    // volume would otherwise stall every other library reader).
    std::string absoluteFolder;
    if (!folder.empty()) {
        auto absolute = platform::PlatformAccessor::Get().Filesystem().Absolute(folder);
        absoluteFolder = absolute.ok() ? absolute.value() : std::string(folder);
    }
    const std::string folderKey = folder.empty() ? std::string() : Norm(absoluteFolder);

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryItem> out;
    // THE FILE'S OWN FILL rides every item handed out (see LibraryItem::fit):
    // the store keeps it keyed by path because a scan rebuilds the items from
    // the disk, so it is merged here, once, for every caller.
    auto withFits = [this](std::vector<LibraryItem>& list) -> std::vector<LibraryItem> {
        for (LibraryItem& item : list) {
            const auto it = fits_.find(Norm(item.path));
            item.fit = it == fits_.end() ? std::string() : it->second;
        }
        return std::move(list);
    };
    if (!folder.empty()) {
        if (const std::string root = MatchKey(folderKey); !root.empty()) {
            if (auto it = items_.find(root); it != items_.end()) out = it->second;
            return withFits(out);
        }
        // A folder inside a library folder: everything under it (its own files and its subfolders').
        std::string prefix = Norm(absoluteFolder);
        prefix.push_back('/');
        for (const std::string& f : order_)
            if (auto it = items_.find(f); it != items_.end())
                for (const LibraryItem& item : it->second)
                    if (Norm(item.path).compare(0, prefix.size(), prefix) == 0) out.push_back(item);
        return withFits(out);
    }
    for (const std::string& f : order_)
        if (auto it = items_.find(f); it != items_.end())
            out.insert(out.end(), it->second.begin(), it->second.end());
    return withFits(out);
}

std::vector<LibraryItem> MediaLibrary::Search(std::string_view text, std::string_view folder) const {
    std::vector<std::string> words = textmatch::TokenizeWords(text);
    std::vector<LibraryItem> pool = Items(folder);
    if (words.empty()) return pool;

    // Every item name is a vocabulary word: "gideons_battle.mp4" carries
    // "gideons", "battle". Typos correct ("batle" -> "battle") against it —
    // the same rules The Table, songs and Bible search get from the shared
    // engine. A raw word stays primary: an incomplete word ("gide") already
    // substring-matches everything its completion would, so completions never
    // NARROW results.
    std::vector<std::string> vocabulary;
    vocabulary.reserve(pool.size() * 2);
    for (const LibraryItem& it : pool)
        for (std::string& w : textmatch::TokenizeWords(it.name))
            vocabulary.push_back(std::move(w));
    std::vector<std::vector<std::string>> lookFor;   // per word: [raw, correction?]
    lookFor.reserve(words.size());
    for (const std::string& w : words) {
        std::vector<std::string> alts{w};
        const textmatch::ResolvedWord r = textmatch::ResolveWordInList(w, vocabulary);
        if (!r.word.empty() && r.weight < 1.0) alts.push_back(r.word);
        lookFor.push_back(std::move(alts));
    }

    struct Hit { int rank; size_t order; LibraryItem item; };
    std::vector<Hit> hits;
    for (size_t i = 0; i < pool.size(); ++i) {
        const std::string name = Lower(pool[i].name);
        bool all = true;
        for (const std::vector<std::string>& alts : lookFor) {
            bool any = false;
            for (const std::string& w : alts)
                if (name.find(w) != std::string::npos) { any = true; break; }
            if (!any) { all = false; break; }
        }
        if (!all) continue;
        // WHERE the first word was found, among the alternatives that matched. The
        // raw word is not necessarily one of them: a query that only landed through
        // a typo CORRECTION ("img10" -> "img2") is in no name at all, and asking
        // for its position gave npos — which then read the name at npos - 1 and
        // aborted the app. A corrected-only match is rank 2 by definition (a fuzzy
        // hit, not the word the operator typed).
        size_t at = std::string::npos;
        for (const std::string& w : lookFor.front()) {
            const size_t found = name.find(w);
            if (found != std::string::npos && (at == std::string::npos || found < at)) at = found;
        }
        int rank = 2;
        if (at == 0) rank = 0;
        else if (at != std::string::npos
                 && (name[at - 1] == ' ' || name[at - 1] == '_' || name[at - 1] == '-')) rank = 1;
        hits.push_back({rank, i, std::move(pool[i])});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rank < b.rank; });
    std::vector<LibraryItem> out;
    out.reserve(hits.size());
    for (Hit& hit : hits) out.push_back(std::move(hit.item));
    return out;
}

size_t MediaLibrary::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [folder, list] : items_) n += list.size();
    return n;
}

} // namespace bps::media
