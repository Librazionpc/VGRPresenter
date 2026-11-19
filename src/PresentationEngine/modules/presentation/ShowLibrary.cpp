#include "modules/presentation/ShowLibrary.hpp"

#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/search/TextMatching.hpp"
#include "modules/vgr/VgrFile.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cstdlib>
#include <format>
#include <utility>

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
    return Scan();
}

// ---- the persisted index ------------------------------------------------------------------------------
// A line per record, tab separated; strings escape \ \t \n \r. First line is the format tag, then the library
// root (an index of another folder is never used), the categories, then the entries.
namespace {
constexpr const char* kIndexTag = "VGRSHOWIDX 1";

std::string Esc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string Unesc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> parts;
    size_t from = 0;
    for (;;) {
        const size_t at = line.find('\t', from);
        if (at == std::string_view::npos) { parts.push_back(line.substr(from)); break; }
        parts.push_back(line.substr(from, at - from));
        from = at + 1;
    }
    return parts;
}
} // namespace

std::string ShowLibrary::IndexPath() const {
    auto& platform = platform::PlatformAccessor::Get();
    const std::string dir = platform.Paths().CacheDir();
    if (dir.empty()) return {};
    return platform.Filesystem().Join(dir, "show-library-index.tsv");
}

size_t ShowLibrary::LoadCachedIndex() {
    const std::string path = IndexPath();
    if (path.empty()) return 0;
    auto text = platform::PlatformAccessor::Get().Filesystem().ReadText(path);   // no lock held
    if (!text.ok()) return 0;

    std::vector<ShowEntry> entries;
    std::vector<std::string> categories;
    IndexMap index;
    bool rootMatches = false;
    size_t pos = 0;
    const std::string& all = text.value();
    bool first = true;
    while (pos < all.size()) {
        size_t end = all.find('\n', pos);
        if (end == std::string::npos) end = all.size();
        std::string_view line(all.data() + pos, end - pos);
        pos = end + 1;
        if (first) {
            first = false;
            if (line != kIndexTag) return 0;   // another format: ignore it, the scan rebuilds it
            continue;
        }
        const auto f = SplitTabs(line);
        if (f.empty()) continue;
        if (f[0] == "R" && f.size() >= 2) {
            rootMatches = Unesc(f[1]) == root_;
            if (!rootMatches) return 0;
        } else if (f[0] == "C" && f.size() >= 2) {
            categories.push_back(Unesc(f[1]));
        } else if (f[0] == "E" && f.size() >= 9 && rootMatches) {
            IndexRec rec;
            rec.entry.path = Unesc(f[1]);
            rec.size = std::strtoull(std::string(f[2]).c_str(), nullptr, 10);
            rec.mtimeNs = std::strtoll(std::string(f[3]).c_str(), nullptr, 10);
            rec.entry.id = Unesc(f[4]);
            rec.entry.name = Unesc(f[5]);
            rec.entry.category = Unesc(f[6]);
            rec.entry.modifiedMs = std::strtoll(std::string(f[7]).c_str(), nullptr, 10);
            rec.entry.slideCount = std::atoi(std::string(f[8]).c_str());
            entries.push_back(rec.entry);
            index.emplace(rec.entry.path, std::move(rec));
        }
    }
    if (!rootMatches || entries.empty()) return 0;
    std::sort(entries.begin(), entries.end(),
              [](const ShowEntry& a, const ShowEntry& b) { return a.modifiedMs > b.modifiedMs; });
    const size_t count = entries.size();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.empty()) return 0;   // a real scan already landed: the cache is older than what is held
    entries_ = std::move(entries);
    categories_ = std::move(categories);
    index_ = std::move(index);
    return count;
}

void ShowLibrary::SaveIndex(const Scanned& scanned) const {
    const std::string path = IndexPath();
    if (path.empty()) return;
    std::string out;
    out.reserve(scanned.index.size() * 160 + 256);
    out += kIndexTag;
    out += "\nR\t" + Esc(root_) + "\n";
    for (const std::string& c : scanned.categories) out += "C\t" + Esc(c) + "\n";
    for (const auto& [p, rec] : scanned.index) {
        out += "E\t" + Esc(rec.entry.path) + "\t" + std::to_string(rec.size) + "\t" + std::to_string(rec.mtimeNs) +
               "\t" + Esc(rec.entry.id) + "\t" + Esc(rec.entry.name) + "\t" + Esc(rec.entry.category) + "\t" +
               std::to_string(rec.entry.modifiedMs) + "\t" + std::to_string(rec.entry.slideCount) + "\n";
        (void)p;
    }
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    (void)fs.CreateDirectories(platform::PlatformAccessor::Get().Paths().CacheDir());
    (void)fs.WriteAtomic(path, out);   // best effort: a failed write only costs the next start its head start
}

Result<void> ShowLibrary::ScanUnlocked(Scanned& out, const IndexMap& previous) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.Exists(root_)) {
        if (auto c = fs.CreateDirectories(root_); !c.ok()) return c.error();
    }
    auto top = fs.Enumerate(root_);
    if (!top.ok()) return top.error();

    auto readShows = [&](const std::string& dir, const std::string& category) {
        auto files = fs.Enumerate(dir);
        if (!files.ok()) {
            out.problems.push_back(dir + ": " + files.error().message);
            return;
        }
        for (const std::string& path : files.value()) {
            if (fs.IsDirectory(path) || !IsShowFile(path)) continue;
            // UNCHANGED SINCE THE LAST SCAN (same size, same modification time): reuse what its header said
            // — a stat instead of reading and parsing the whole file.
            const auto stat = fs.Metadata(path);
            const uint64_t size = stat.ok() ? stat.value().sizeBytes : 0;
            const int64_t mtimeNs = stat.ok() ? stat.value().modifiedEpochNs : 0;
            if (stat.ok()) {
                const auto known = previous.find(path);
                if (known != previous.end() && known->second.size == size && known->second.mtimeNs == mtimeNs) {
                    IndexRec rec = known->second;
                    rec.entry.category = category;
                    out.entries.push_back(rec.entry);
                    out.index.emplace(path, std::move(rec));
                    continue;
                }
            }
            auto doc = vgr::VgrFile::Read(path);
            if (!doc.ok()) {
                out.problems.push_back(path + ": " + doc.error().message);
                continue;
            }
            const auto& h = doc.value().header;
            if (h.type != vgr::DocumentType::Show && h.type != vgr::DocumentType::Presentation) {
                out.problems.push_back(path + ": not a show (" + vgr::ToString(h.type) + ")");
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
            if (stat.ok()) out.index.emplace(path, IndexRec{size, mtimeNs, e});
            out.entries.push_back(std::move(e));
        }
    };

    readShows(root_, {});
    for (const std::string& path : top.value()) {
        if (!fs.IsDirectory(path)) continue;
        const std::string name = LeafName(path);
        if (!name.empty() && name.front() == '.') continue;   // .deleted etc. — not a category
        out.categories.push_back(name);
        readShows(path, name);
    }

    auto ci = [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); };
    std::sort(out.categories.begin(), out.categories.end(), ci);
    std::sort(out.entries.begin(), out.entries.end(),
              [](const ShowEntry& a, const ShowEntry& b) { return a.modifiedMs > b.modifiedMs; });
    return Ok();
}

Result<void> ShowLibrary::Scan() {
    Scanned scanned;   // filled with no lock held
    IndexMap previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = index_;   // what the last scan (or the persisted index) knew
    }
    if (auto s = ScanUnlocked(scanned, previous); !s.ok()) return s.error();
    SaveIndex(scanned);   // file I/O: no lock held
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(scanned.entries);
    categories_ = std::move(scanned.categories);
    problems_ = std::move(scanned.problems);
    index_ = std::move(scanned.index);
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
    return Scan();
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
    return Scan();
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

    std::vector<std::pair<std::string, std::string>> moved;
    for (const std::string& source : contents.value()) {
        if (fs.IsDirectory(source) || !IsShowFile(source)) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                (void)fs.Move(it->second, it->first);
            return Error::Make(Err::InvalidState, kModule,
                               std::format("category '{}' contains unsupported files", name));
        }
        const std::string leaf = LeafName(source);
        std::string target = fs.Join(root_, leaf);
        const size_t dot = leaf.find_last_of('.');
        const std::string stem = dot == std::string::npos ? leaf : leaf.substr(0, dot);
        const std::string ext = dot == std::string::npos ? std::string() : leaf.substr(dot);
        for (int suffix = 2; fs.Exists(target); ++suffix)
            target = fs.Join(root_, std::format("{} ({}){}", stem, suffix, ext));
        if (auto move = fs.Move(source, target); !move.ok()) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                (void)fs.Move(it->second, it->first);
            return move.error();
        }
        moved.emplace_back(source, target);
    }
    if (auto r = fs.Remove(dir); !r.ok()) {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
            (void)fs.Move(it->second, it->first);
        return r.error();
    }
    return Scan();
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
    return Scan();
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
    if (auto s = Scan(); !s.ok()) return s.error();
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
    if (auto s = Scan(); !s.ok()) return s.error();
    return target;
}

Result<std::string> ShowLibrary::DuplicateShow(std::string_view path, std::string_view newName) {
    const std::string from(path);
    auto show = LoadShow(from);
    if (!show.ok()) return show.error();
    Presentation& m = show.value().model;
    m.id = std::format("show-{}", NowMs());          // a copy is a different show
    m.createdAt = m.modifiedAt = std::chrono::system_clock::now();

    const std::string dir = ParentOf(from);
    const std::string category = Norm(dir) == Norm(root_) ? std::string() : LeafName(dir);
    // A copy is "Name (2)", then "Name (3)"... like a file system numbers it (an explicit name is numbered only if taken).
    m.name = NumberedShowName(category, newName.empty() ? std::string_view(m.name) : newName);
    auto target = NewShowPath(category, m.name);
    if (!target.ok()) return target.error();
    show.value().doc.header.createdMs = NowMs();
    if (auto w = StoreShow(target.value(), show.value()); !w.ok()) return w.error();
    if (auto s = Scan(); !s.ok()) return s.error();
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
    if (auto s = Scan(); !s.ok()) return s.error();
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

std::string ShowLibrary::NumberedShowName(std::string_view category, std::string_view showName) const {
    std::string name(showName);
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = CategoryDir(category);
    const std::string base0 = SafeName(name);
    if (!fs.Exists(fs.Join(dir, base0 + ".vgr")))
        return name;
    // "Name (3)" is numbered from "Name": strip one trailing " (digits)".
    std::string stem = name;
    if (stem.size() > 4 && stem.back() == ')') {
        const size_t open = stem.rfind(" (");
        if (open != std::string::npos && open + 3 < stem.size()) {
            const std::string digits = stem.substr(open + 2, stem.size() - open - 3);
            if (digits.find_first_not_of("0123456789") == std::string::npos)
                stem.resize(open);
        }
    }
    for (int n = 2;; ++n) {
        std::string candidate = std::format("{} ({})", stem, n);
        if (!fs.Exists(fs.Join(dir, SafeName(candidate) + ".vgr")))
            return candidate;
    }
}

namespace {
// What a slide SAYS — the words, not the ids/timestamps a save stamps on every copy.
std::string SlideWords(const Slide& s) {
    std::string w = s.title;
    w += '\x1f';
    w += s.text;
    for (const auto& b : s.blocks) {
        w += '\x1e';
        w += b.text;
    }
    return w;
}

// "Name (3).vgr" -> 3; "Name.vgr" (the original) -> 0.
int CopyNumber(const std::string& path) {
    const size_t dot = path.rfind(".vgr");
    if (dot == std::string::npos || dot < 4 || path[dot - 1] != ')')
        return 0;
    const size_t open = path.rfind(" (", dot - 1);
    if (open == std::string::npos)
        return 0;
    const std::string digits = path.substr(open + 2, dot - 1 - (open + 2));
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    return std::atoi(digits.c_str());
}
} // namespace

Result<ShowLibrary::DuplicateReport> ShowLibrary::RemoveDuplicates() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    DuplicateReport report;
    const std::vector<ShowEntry> all = Shows();

    // Only shows that share a name AND a slide count can be the same show: both are in the cached index, so
    // the files of every other song are never opened.
    std::map<std::pair<std::string, int>, std::vector<const ShowEntry*>> candidates;
    for (const ShowEntry& e : all)
        candidates[{e.name, e.slideCount}].push_back(&e);

    std::vector<std::string> doomed;
    for (auto& [key, group] : candidates) {
        if (group.size() < 2)
            continue;
        std::map<std::string, std::vector<const ShowEntry*>> sameWords;   // words -> the files that say them
        for (const ShowEntry* e : group) {
            auto loaded = LoadShow(e->path);
            if (!loaded.ok()) {
                ++report.unreadable;
                continue;
            }
            std::string words;
            for (const Slide& s : loaded.value().model.slides)
                words += SlideWords(s) + '\x1d';
            sameWords[std::move(words)].push_back(e);
        }
        for (auto& [words, files] : sameWords) {
            if (files.size() < 2)
                continue;
            ++report.groups;
            // The one to keep, per folder: the original, else the lowest copy number, else the oldest.
            const auto better = [](const ShowEntry* a, const ShowEntry* b) {
                const int na = CopyNumber(a->path), nb = CopyNumber(b->path);
                if (na != nb) return na < nb;
                if (a->modifiedMs != b->modifiedMs) return a->modifiedMs < b->modifiedMs;
                return a->path < b->path;
            };
            std::map<std::string, std::vector<const ShowEntry*>> byFolder;
            bool anyNamedFolder = false;
            for (const ShowEntry* e : files) {
                byFolder[e->category].push_back(e);
                if (!e->category.empty()) anyNamedFolder = true;
            }
            for (auto& [folder, inFolder] : byFolder) {
                std::sort(inFolder.begin(), inFolder.end(), better);
                // An uncategorised copy of a show that also lives in a named folder is the stray one.
                const size_t firstToRemove = (folder.empty() && anyNamedFolder) ? 0 : 1;
                for (size_t i = firstToRemove; i < inFolder.size(); ++i)
                    doomed.push_back(inFolder[i]->path);
            }
        }
    }
    if (doomed.empty())
        return report;

    const std::string bin = fs.Join(root_, kDeletedFolder);
    if (!fs.Exists(bin)) {
        if (auto c = fs.CreateDirectories(bin); !c.ok()) return c.error();
    }
    for (const std::string& from : doomed) {
        std::string target = fs.Join(bin, LeafName(from));
        if (fs.Exists(target))
            target = fs.Join(bin, std::format("{}-{}", NowMs(), LeafName(from)));
        if (fs.Move(from, target).ok())
            ++report.removed;
    }
    if (auto s = Scan(); !s.ok()) return s.error();
    return report;
}

std::string ShowLibrary::FindSameShow(std::string_view category, const Presentation& show) const {
    // Same name and same slide count first (both are in the cached index, so this opens nothing for the
    // hundreds of other songs); only those few candidates are read and compared word for word.
    for (const ShowEntry& e : ShowsIn(category)) {
        if (e.name != show.name || e.slideCount != static_cast<int>(show.slides.size()))
            continue;
        auto loaded = LoadShow(e.path);
        if (!loaded.ok())
            continue;
        const auto& have = loaded.value().model.slides;
        if (have.size() != show.slides.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < have.size() && same; ++i)
            same = SlideWords(have[i]) == SlideWords(show.slides[i]);
        if (same)
            return e.path;
    }
    return {};
}
} // namespace bps::presentation