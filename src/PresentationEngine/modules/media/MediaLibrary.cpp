#include "modules/media/MediaLibrary.hpp"

#include "core/config/Json.hpp"
#include "modules/search/TextMatching.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
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
    indexFile_ = (cut == std::string::npos ? std::string() : storageFile_.substr(0, cut + 1)) + "media-index.json";
}

std::string MediaLibrary::Canonical(std::string_view path) const {
    // An empty query matches nothing. Without this guard the fallback below
    // would compare Norm("") against every stored folder — and any folder
    // whose normalised form is also empty would be returned as a match.
    if (path.empty()) return {};
    auto absolute = platform::PlatformAccessor::Get().Filesystem().Absolute(path);
    const std::string key = Norm(absolute.ok() ? absolute.value() : std::string(path));
    for (const std::string& f : order_)
        if (Norm(f) == key) return f;
    return {};
}

Result<void> MediaLibrary::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.Exists(storageFile_)) return {};
    auto text = fs.ReadText(storageFile_);
    if (!text.ok()) return text.error();
    auto parsed = json::Parse(text.value());
    if (!parsed.ok())
        return Error::Make(Err::ParseError, kModule, "media folder list is damaged: " + parsed.error().message);

    std::lock_guard<std::mutex> lock(mutex_);
    order_.clear();
    items_.clear();
    scanned_.clear();
    if (const auto* list = parsed.value().Find("folders"))
        if (const auto* arr = list->asArray())
            for (const auto& v : *arr) {
                const std::string path(v.asString());
                if (path.empty()) continue;
                const std::string key = Norm(path);
                const bool known = std::any_of(order_.begin(), order_.end(),
                                               [&](const std::string& f) { return Norm(f) == key; });
                if (!known) order_.push_back(path);
            }

    // The remembered index (what was in each folder last time). Anything wrong with it is ignored:
    // it is only a head start, and the first Scan() of each folder replaces it.
    if (!fs.Exists(indexFile_)) return {};
    auto indexText = fs.ReadText(indexFile_);
    if (!indexText.ok()) return {};
    auto index = json::Parse(indexText.value());
    if (!index.ok()) return {};
    const auto* byFolder = index.value().Find("folders");
    const auto* object = byFolder ? byFolder->asObject() : nullptr;
    if (!object) return {};
    for (const std::string& folder : order_) {
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
        items_[folder] = std::move(restored);
    }
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

void MediaLibrary::SaveIndex(const std::string& text) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const size_t cut = indexFile_.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) (void)fs.CreateDirectories(std::string_view(indexFile_).substr(0, cut));
    (void)fs.Write(indexFile_, text);
}

Result<void> MediaLibrary::Save() const {
    json::Value::Array folders;
    for (const std::string& f : order_) folders.push_back(json::Value::String(f));
    json::Value::Object root;
    root["folders"] = json::Value(std::move(folders));
    const std::string text = json::Value(std::move(root)).ToString();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const size_t cut = storageFile_.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        auto made = fs.CreateDirectories(std::string_view(storageFile_).substr(0, cut));
        if (!made.ok()) return made.error();
    }
    return fs.Write(storageFile_, text);
}

Result<void> MediaLibrary::AddFolder(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (path.empty() || !fs.IsDirectory(path))
        return Error::Make(Err::InvalidArgument, kModule, std::format("'{}' is not a folder", path));
    auto absolute = fs.Absolute(path);
    std::string folder = absolute.ok() ? absolute.value() : std::string(path);
    while (folder.size() > 3 && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();

    std::lock_guard<std::mutex> lock(mutex_);
    if (!Canonical(folder).empty())
        return Error::Make(Err::AlreadyExists, kModule, "that folder is already in the library");
    order_.push_back(folder);
    if (auto saved = Save(); !saved.ok()) {
        order_.pop_back();
        return saved.error();
    }
    return {};
}

Result<void> MediaLibrary::RemoveFolder(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string folder = Canonical(path);
    if (folder.empty())
        return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
    order_.erase(std::find(order_.begin(), order_.end(), folder));
    items_.erase(folder);
    scanned_.erase(folder);
    const std::string index = BuildIndexLocked();
    auto saved = Save();
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
    // Empty path: Canonical() answers {} for it, which the check below
    // reports as "not in the library". Stated explicitly so the intent is
    // readable — an empty folder name is not a library folder.
    if (path.empty())
        return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
    std::string folder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folder = Canonical(path);
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
        std::sort(found.begin(), found.end(), [](const LibraryItem& a, const LibraryItem& b) {
            if (NaturalLess(a.name, b.name)) return true;
            if (NaturalLess(b.name, a.name)) return false;
            return a.path < b.path;
        });
    }   // a folder that has gone missing is scanned as empty

    const size_t count = found.size();
    std::unique_lock<std::mutex> lock(mutex_);
    if (Canonical(folder).empty())   // removed while it was being scanned
        return Error::Make(Err::NotFound, kModule, "that folder is not in the library");
    items_[folder] = std::move(found);
    scanned_[folder] = true;
    const std::string index = BuildIndexLocked();
    lock.unlock();
    SaveIndex(index);   // written outside the lock: it can be a few MB for a big library
    return count;
}

std::vector<LibrarySubfolder> MediaLibrary::Subfolders(std::string_view libraryFolder) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string root = Canonical(libraryFolder);
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

std::vector<LibraryItem> MediaLibrary::Items(std::string_view folder) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryItem> out;
    if (!folder.empty()) {
        if (const std::string root = Canonical(folder); !root.empty()) {
            if (auto it = items_.find(root); it != items_.end()) out = it->second;
            return out;
        }
        // A folder inside a library folder: everything under it (its own files and its subfolders').
        auto absolute = platform::PlatformAccessor::Get().Filesystem().Absolute(folder);
        std::string prefix = Norm(absolute.ok() ? absolute.value() : std::string(folder));
        prefix.push_back('/');
        for (const std::string& f : order_)
            if (auto it = items_.find(f); it != items_.end())
                for (const LibraryItem& item : it->second)
                    if (Norm(item.path).compare(0, prefix.size(), prefix) == 0) out.push_back(item);
        return out;
    }
    for (const std::string& f : order_)
        if (auto it = items_.find(f); it != items_.end())
            out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
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
        const size_t at = name.find(lookFor.front().front());
        const int rank = at == 0 ? 0 : (name[at - 1] == ' ' || name[at - 1] == '_' || name[at - 1] == '-' ? 1 : 2);
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
