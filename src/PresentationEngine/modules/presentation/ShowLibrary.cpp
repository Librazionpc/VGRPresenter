#include "modules/presentation/ShowLibrary.hpp"

#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/search/TextMatching.hpp"
#include "modules/vgr/VgrFile.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>

namespace bps::presentation {

namespace {
namespace textmatch = bps::search::textmatch;

constexpr const char* kModule = "ShowLibrary";

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Last path component of a full path (either separator).
std::string LeafName(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// Folder part of a path (either separator); "" when there is none.
std::string ParentOf(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// Path comparison key: one separator style, no trailing slash, case-folded (the
// platform layer may hand back backslashes for a root given with slashes).
std::string Norm(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr const char* kDeletedFolder = ".deleted";

bool IsShowFile(const std::string& path) {
    return Lower(std::string_view(path)).ends_with(".vgr");
}

bool ValidFolderName(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') return false;
    return name.find_first_of("/\\:*?\"<>|") == std::string_view::npos;
}

} // namespace

ShowLibrary::ShowLibrary(std::string rootDir) : root_(std::move(rootDir)) {}

std::string ShowLibrary::SafeName(std::string_view text) {
    std::string out;
    for (char c : text) {
        const bool bad = std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ||
                         static_cast<unsigned char>(c) < 0x20;
        out += bad ? '-' : c;
    }
    // Trim spaces/dots at the ends (Windows rejects trailing ones).
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Untitled") : out;
}

std::string ShowLibrary::CategoryDir(std::string_view category) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return category.empty() ? root_ : fs.Join(root_, category);
}

Result<void> ShowLibrary::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ScanLocked();
}

Result<void> ShowLibrary::ScanLocked() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.Exists(root_)) {
        if (auto c = fs.CreateDirectories(root_); !c.ok()) return c.error();
    }
    auto top = fs.Enumerate(root_);
    if (!top.ok()) return top.error();

    entries_.clear();
    categories_.clear();
    problems_.clear();

    auto readShows = [&](const std::string& dir, const std::string& category) {
        auto files = fs.Enumerate(dir);
        if (!files.ok()) {
            problems_.push_back(dir + ": " + files.error().message);
            return;
        }
        for (const std::string& path : files.value()) {
            if (fs.IsDirectory(path) || !IsShowFile(path)) continue;
            auto doc = vgr::VgrFile::Read(path);
            if (!doc.ok()) {
                problems_.push_back(path + ": " + doc.error().message);
                continue;
            }
            const auto& h = doc.value().header;
            if (h.type != vgr::DocumentType::Show && h.type != vgr::DocumentType::Presentation) {
                problems_.push_back(path + ": not a show (" + vgr::ToString(h.type) + ")");
                continue;
            }
            ShowEntry e;
            e.path = path;
            e.id = h.uuid;
            e.name = h.name.empty() ? LeafName(path) : h.name;
            e.category = category;
            e.modifiedMs = h.modifiedMs;
            if (auto it = h.searchMetadata.find("slides"); it != h.searchMetadata.end())
                e.slideCount = std::atoi(it->second.c_str());
            entries_.push_back(std::move(e));
        }
    };

    readShows(root_, {});
    for (const std::string& path : top.value()) {
        if (!fs.IsDirectory(path)) continue;
        const std::string name = LeafName(path);
        if (!name.empty() && name.front() == '.') continue;   // .deleted etc. — not a category
        categories_.push_back(name);
        readShows(path, name);
    }

    auto ci = [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); };
    std::sort(categories_.begin(), categories_.end(), ci);
    std::sort(entries_.begin(), entries_.end(),
              [](const ShowEntry& a, const ShowEntry& b) { return a.modifiedMs > b.modifiedMs; });
    return Ok();
}

std::vector<std::string> ShowLibrary::Categories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return categories_;
}

std::vector<ShowEntry> ShowLibrary::Shows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::vector<ShowEntry> ShowLibrary::ShowsIn(std::string_view category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShowEntry> out;
    for (const auto& e : entries_)
        if (e.category == category) out.push_back(e);
    return out;
}

std::vector<ShowEntry> ShowLibrary::Search(std::string_view text) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShowEntry> out;
    const auto words = textmatch::TokenizeWords(text);
    if (words.empty()) {
        out = entries_;
        return out;
    }
    // Show names + categories are the vocabulary: "victory_lifestream.show"
    // carries "victory", "lifestream". Typos correct ("victoy" -> "victory")
    // before matching — the shared rules of every other search surface. A raw
    // word stays primary: an incomplete word ("vic") already substring-matches
    // everything its completion would, so completions never NARROW results.
    std::vector<std::string> vocabulary;
    vocabulary.reserve(entries_.size() * 2);
    for (const auto& e : entries_) {
        for (std::string& w : textmatch::TokenizeWords(e.name)) vocabulary.push_back(std::move(w));
        for (std::string& w : textmatch::TokenizeWords(e.category)) vocabulary.push_back(std::move(w));
    }
    std::vector<std::vector<std::string>> lookFor;   // per word: [raw, correction?]
    lookFor.reserve(words.size());
    for (const std::string& w : words) {
        std::vector<std::string> alts{w};
        const textmatch::ResolvedWord r = textmatch::ResolveWordInList(w, vocabulary);
        if (!r.word.empty() && r.weight < 1.0) alts.push_back(r.word);
        lookFor.push_back(std::move(alts));
    }
    for (const auto& e : entries_) {
        const std::string name = Lower(e.name);
        const std::string cat = Lower(e.category);
        bool all = true;
        for (const std::vector<std::string>& alts : lookFor) {
            bool any = false;
            for (const std::string& w : alts)
                if (name.find(w) != std::string::npos || cat.find(w) != std::string::npos) { any = true; break; }
            if (!any) { all = false; break; }
        }
        if (all) out.push_back(e);
    }
    return out;
}

std::vector<std::string> ShowLibrary::Problems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return problems_;
}

Result<void> ShowLibrary::CreateCategory(std::string_view name) {
    if (!ValidFolderName(name))
        return Error::Make(Err::InvalidArgument, kModule,
                           "a category name can't be empty, end in a space or dot, or contain / \\ : * ? \" < > |");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = CategoryDir(name);
    if (fs.Exists(dir))
        return Error::Make(Err::AlreadyExists, kModule, std::format("category '{}' already exists", name));
    if (auto c = fs.CreateDirectories(dir); !c.ok()) return c.error();
    std::lock_guard<std::mutex> lock(mutex_);
    return ScanLocked();
}

Result<void> ShowLibrary::RenameCategory(std::string_view from, std::string_view to) {
    if (!ValidFolderName(from) || !ValidFolderName(to))
        return Error::Make(Err::InvalidArgument, kModule, "invalid category name");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string src = CategoryDir(from), dst = CategoryDir(to);
    if (!fs.IsDirectory(src))
        return Error::Make(Err::NotFound, kModule, std::format("no category '{}'", from));
    if (fs.Exists(dst))
        return Error::Make(Err::AlreadyExists, kModule, std::format("category '{}' already exists", to));
    if (auto m = fs.Move(src, dst); !m.ok()) return m.error();
    std::lock_guard<std::mutex> lock(mutex_);
    return ScanLocked();
}

Result<void> ShowLibrary::RemoveCategory(std::string_view name) {
    if (!ValidFolderName(name))
        return Error::Make(Err::InvalidArgument, kModule, "invalid category name");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = CategoryDir(name);
    if (!fs.IsDirectory(dir))
        return Error::Make(Err::NotFound, kModule, std::format("no category '{}'", name));
    auto contents = fs.Enumerate(dir);
    if (!contents.ok()) return contents.error();
    if (!contents.value().empty())
        return Error::Make(Err::InvalidState, kModule,
                           std::format("category '{}' still has shows in it — move or delete them first", name));
    if (auto r = fs.Remove(dir); !r.ok()) return r.error();
    std::lock_guard<std::mutex> lock(mutex_);
    return ScanLocked();
}

Result<void> ShowLibrary::ClearCategory(std::string_view name) {
    if (!ValidFolderName(name))
        return Error::Make(Err::InvalidArgument, kModule, "invalid category name");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = CategoryDir(name);
    if (!fs.IsDirectory(dir)) return Ok();   // nothing to clear
    auto contents = fs.Enumerate(dir);
    if (!contents.ok()) return contents.error();
    const std::string bin = fs.Join(root_, kDeletedFolder);
    if (!contents.value().empty() && !fs.Exists(bin)) {
        if (auto c = fs.CreateDirectories(bin); !c.ok()) return c.error();
    }
    for (const std::string& path : contents.value()) {
        if (fs.IsDirectory(path) || !IsShowFile(path)) continue;
        std::string target = fs.Join(bin, LeafName(path));
        if (fs.Exists(target))
            target = fs.Join(bin, std::format("{}-{}", NowMs(), LeafName(path)));
        if (auto m = fs.Move(path, target); !m.ok()) return m.error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return ScanLocked();
}

Result<std::string> ShowLibrary::MoveShow(std::string_view path, std::string_view category) {
    if (!category.empty() && !ValidFolderName(category))
        return Error::Make(Err::InvalidArgument, kModule, "invalid category name");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string from(path);
    if (!fs.Exists(from))
        return Error::Make(Err::NotFound, kModule, "show file not found: " + from);
    if (!category.empty() && !fs.IsDirectory(CategoryDir(category)))
        return Error::Make(Err::NotFound, kModule, std::format("no category '{}'", category));
    const std::string dest = fs.Join(CategoryDir(category), LeafName(from));
    if (Norm(dest) == Norm(from)) return dest;   // already there
    if (fs.Exists(dest))
        return Error::Make(Err::AlreadyExists, kModule,
                           "a show with that file name already exists in the destination");
    if (auto m = fs.Move(from, dest); !m.ok()) return m.error();
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto s = ScanLocked(); !s.ok()) return s.error();
    return dest;
}

// Loads a show file for rewriting: the container (sections/assets preserved) and
// its parsed model.
namespace {
struct LoadedShow {
    vgr::VgrDocument doc;
    Presentation model;
};
Result<LoadedShow> LoadShow(const std::string& path) {
    auto doc = vgr::VgrFile::Read(path);
    if (!doc.ok()) return doc.error();
    const auto type = doc.value().header.type;
    if (type != vgr::DocumentType::Show && type != vgr::DocumentType::Presentation)
        return Error::Make(Err::InvalidArgument, kModule, "not a show file: " + path);
    PresentationSerializer ser;
    auto model = ser.Deserialize(doc.value().documentJson);
    if (!model.ok()) return model.error();
    return LoadedShow{std::move(doc.value()), std::move(model.value())};
}
Result<void> StoreShow(const std::string& path, LoadedShow& s) {
    PresentationSerializer ser;
    auto body = ser.Serialize(s.model);
    if (!body.ok()) return body.error();
    s.doc.documentJson = std::move(body.value());
    s.doc.header.name = s.model.name;
    s.doc.header.uuid = s.model.id;
    s.doc.header.modifiedMs = NowMs();
    return vgr::VgrFile::Write(path, s.doc);
}
} // namespace

Result<std::string> ShowLibrary::RenameShow(std::string_view path, std::string_view newName) {
    std::string name(newName);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) return Error::Make(Err::InvalidArgument, kModule, "a show needs a name");
    const std::string from(path);
    auto show = LoadShow(from);
    if (!show.ok()) return show.error();
    show.value().model.name = name;

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = ParentOf(from);
    const std::string base = SafeName(name);
    std::string target = fs.Join(dir, base + ".vgr");
    for (int n = 2; fs.Exists(target) && target != from; ++n)
        target = fs.Join(dir, std::format("{} ({}).vgr", base, n));

    // Write the renamed file first; only then drop the old one — an interruption
    // leaves both, never neither.
    if (auto w = StoreShow(target, show.value()); !w.ok()) return w.error();
    if (target != from) (void)fs.Remove(from);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto s = ScanLocked(); !s.ok()) return s.error();
    return target;
}

Result<std::string> ShowLibrary::DuplicateShow(std::string_view path, std::string_view newName) {
    const std::string from(path);
    auto show = LoadShow(from);
    if (!show.ok()) return show.error();
    Presentation& m = show.value().model;
    m.name = newName.empty() ? m.name + " copy" : std::string(newName);
    m.id = std::format("show-{}", NowMs());          // a copy is a different show
    m.createdAt = m.modifiedAt = std::chrono::system_clock::now();

    const std::string dir = ParentOf(from);
    const std::string category = Norm(dir) == Norm(root_) ? std::string() : LeafName(dir);
    auto target = NewShowPath(category, m.name);
    if (!target.ok()) return target.error();
    show.value().doc.header.createdMs = NowMs();
    if (auto w = StoreShow(target.value(), show.value()); !w.ok()) return w.error();
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto s = ScanLocked(); !s.ok()) return s.error();
    return target.value();
}

Result<std::string> ShowLibrary::DeleteShow(std::string_view path) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string from(path);
    if (!fs.Exists(from))
        return Error::Make(Err::NotFound, kModule, "show file not found: " + from);
    const std::string bin = fs.Join(root_, kDeletedFolder);
    if (!fs.Exists(bin)) {
        if (auto c = fs.CreateDirectories(bin); !c.ok()) return c.error();
    }
    std::string target = fs.Join(bin, LeafName(from));
    if (fs.Exists(target))
        target = fs.Join(bin, std::format("{}-{}", NowMs(), LeafName(from)));
    if (auto m = fs.Move(from, target); !m.ok()) return m.error();
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto s = ScanLocked(); !s.ok()) return s.error();
    return target;
}

Result<void> ShowLibrary::EmptyDeleted() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string bin = fs.Join(root_, kDeletedFolder);
    if (fs.Exists(bin)) return fs.RemoveAll(bin);
    return Ok();
}

Result<std::string> ShowLibrary::NewShowPath(std::string_view category, std::string_view showName) const {
    if (!category.empty() && !ValidFolderName(category))
        return Error::Make(Err::InvalidArgument, kModule, "invalid category name");
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = CategoryDir(category);
    if (!fs.Exists(dir)) {
        if (auto c = fs.CreateDirectories(dir); !c.ok()) return c.error();
    }
    const std::string base = SafeName(showName);
    std::string candidate = fs.Join(dir, base + ".vgr");
    for (int n = 2; fs.Exists(candidate); ++n)
        candidate = fs.Join(dir, std::format("{} ({}).vgr", base, n));
    return candidate;
}

} // namespace bps::presentation
