#include "modules/library/DesignLibrary.hpp"

#include "core/config/Json.hpp"
#include "modules/presentation/BlockValidator.hpp"
#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/presentation/ShowEditor.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <type_traits>

namespace bps::library {

namespace {

constexpr const char* kModule = "DesignLibrary";

// The one id every design's blocks live under when ShowEditor's block rules are applied to them.
constexpr const char* kScratchSlide = "content";

using J = json::Value;
namespace pres = presentation;

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// "img2" < "img10": digit runs compare as numbers, everything else case-insensitively.
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            size_t zi = i, zj = j;   // leading zeros do not count
            while (zi + 1 < ei && a[zi] == '0') ++zi;
            while (zj + 1 < ej && b[zj] == '0') ++zj;
            const size_t li = ei - zi, lj = ej - zj;
            if (li != lj) return li < lj;
            const int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return (a.size() - i) < (b.size() - j);
}

std::vector<std::string> Words(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(Lower(cur)), cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(Lower(cur));
    return out;
}

// ---------------------------------------------------------------------------
// JSON. A design's content is written as a presentation SlideTemplate body - the engine's one schema for
// blocks - with the library's own fields (colour, category, flags) beside it.
// ---------------------------------------------------------------------------

std::string Str(const J& v, std::string_view key, std::string_view dflt = {}) {
    const J* f = v.Find(key);
    return f ? std::string(f->asString(dflt)) : std::string(dflt);
}
double Num(const J& v, std::string_view key, double dflt = 0) {
    const J* f = v.Find(key);
    return f ? f->asNumber(dflt) : dflt;
}
bool Flag(const J& v, std::string_view key, bool dflt = false) {
    const J* f = v.Find(key);
    return f ? f->asBool(dflt) : dflt;
}

pres::SlideTemplate ToTemplate(const Design& d) {
    pres::SlideTemplate t;
    t.id = d.id;
    t.name = d.name;
    t.contentType = d.contentType;
    t.background = d.background;
    t.blocks = d.blocks;
    t.metaJson = d.metaJson;
    return t;
}

J ToJson(const Design& d) {
    J::Object o;
    o["id"] = J::String(d.id);
    o["name"] = J::String(d.name);
    o["color"] = J::String(d.color);
    o["category"] = J::String(d.category);
    o["default"] = J::Bool(d.isDefault);
    o["locked"] = J::Bool(d.locked);
    o["under"] = J::Bool(d.placeUnderSlide);
    o["duration"] = J::Number(d.displayDuration);
    if (auto body = pres::PresentationSerializer().SerializeTemplate(ToTemplate(d)); body.ok())
        if (auto parsed = json::Parse(body.value()); parsed.ok()) o["content"] = std::move(parsed.value());
    return J(std::move(o));
}

J ToJson(const DesignCategory& c) {
    J::Object o;
    o["id"] = J::String(c.id);
    o["name"] = J::String(c.name);
    o["icon"] = J::String(c.icon);
    o["default"] = J::Bool(c.isDefault);
    return J(std::move(o));
}

bool FromJson(const J& o, Design& out) {
    out.id = Str(o, "id");
    out.name = Str(o, "name");
    if (out.id.empty() || out.name.empty()) return false;
    out.color = Str(o, "color");
    out.category = Str(o, "category");
    out.isDefault = Flag(o, "default");
    out.locked = Flag(o, "locked");
    out.placeUnderSlide = Flag(o, "under");
    out.displayDuration = Num(o, "duration");
    if (const J* content = o.Find("content")) {
        if (auto t = pres::PresentationSerializer().DeserializeTemplate(content->ToString()); t.ok()) {
            out.contentType = t.value().contentType;
            out.background = t.value().background;
            out.blocks = t.value().blocks;
            out.metaJson = t.value().metaJson;
        }
    }
    return true;
}

// ShowEditor's block rules (ids, stacking, validation), applied to a design's blocks by lending them to a
// scratch presentation with one slide.
template <typename Op>
auto WithBlocks(Design& d, Op&& op) {
    pres::Presentation scratch;
    pres::Slide slide;
    slide.id = kScratchSlide;
    slide.blocks = std::move(d.blocks);
    scratch.slides.push_back(std::move(slide));
    auto result = op(scratch);
    d.blocks = std::move(scratch.slides.front().blocks);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction, load, save
// ---------------------------------------------------------------------------

DesignLibrary::DesignLibrary(std::string storageFile, DesignLibraryConfig config)
    : storageFile_(std::move(storageFile)), config_(std::move(config)) {}

Error DesignLibrary::Missing() const {
    return Error::Make(Err::NotFound, kModule, std::format("no such {}", config_.noun));
}

void DesignLibrary::EnsureDefaultsLocked() {
    for (const DesignCategory& def : config_.defaultCategories) {
        auto found = std::find_if(categories_.begin(), categories_.end(),
                                  [&](const DesignCategory& c) { return c.id == def.id; });
        if (found != categories_.end()) {
            found->isDefault = true;   // the app's own, whatever the file said
            continue;
        }
        const auto shipped = std::count_if(categories_.begin(), categories_.end(),
                                           [](const DesignCategory& c) { return c.isDefault; });
        categories_.insert(categories_.begin() + shipped, def);
    }
    for (const Design& def : config_.defaultDesigns) {
        if (deletedDefaults_.contains(def.id)) continue;
        const bool present = std::any_of(designs_.begin(), designs_.end(),
                                         [&](const Design& d) { return d.id == def.id; });
        if (!present) designs_.push_back(def);
    }
}

Result<void> DesignLibrary::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::lock_guard<std::mutex> lock(mutex_);
    categories_.clear();
    designs_.clear();
    deletedDefaults_.clear();

    if (fs.Exists(storageFile_)) {
        auto text = fs.ReadText(storageFile_);
        if (!text.ok()) {
            EnsureDefaultsLocked();
            return text.error();
        }
        auto parsed = json::Parse(text.value());
        if (!parsed.ok()) {
            EnsureDefaultsLocked();
            return Error::Make(Err::ParseError, kModule,
                               std::format("the {} library is damaged: {}", config_.noun, parsed.error().message));
        }
        const J& root = parsed.value();
        if (const J* cats = root.Find("categories"); cats && cats->asArray())
            for (const J& c : *cats->asArray()) {
                DesignCategory cat{ Str(c, "id"), Str(c, "name"), Str(c, "icon", "folder"), Flag(c, "default") };
                if (!cat.id.empty() && !cat.name.empty()) categories_.push_back(std::move(cat));
            }
        if (const J* list = root.Find("designs"); list && list->asArray())
            for (const J& o : *list->asArray()) {
                Design d;
                if (FromJson(o, d)) designs_.push_back(std::move(d));
            }
        if (const J* gone = root.Find("deletedDefaults"); gone && gone->asArray())
            for (const J& id : *gone->asArray()) deletedDefaults_.insert(std::string(id.asString()));
    }

    EnsureDefaultsLocked();
    // A design filed in a category that no longer exists is just unlabeled.
    for (Design& d : designs_)
        if (!d.category.empty() && !CategoryLocked(d.category)) d.category.clear();
    return {};
}

Result<void> DesignLibrary::SaveLocked() const {
    J::Array cats;
    for (const DesignCategory& c : categories_) cats.push_back(ToJson(c));
    J::Array designs;
    for (const Design& d : designs_) designs.push_back(ToJson(d));
    J::Array gone;
    for (const std::string& id : deletedDefaults_) gone.push_back(J::String(id));
    J::Object root;
    root["version"] = J::Number(1);
    root["categories"] = J(std::move(cats));
    root["designs"] = J(std::move(designs));
    root["deletedDefaults"] = J(std::move(gone));
    const std::string text = J(std::move(root)).ToString();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const size_t cut = storageFile_.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        auto made = fs.CreateDirectories(std::string_view(storageFile_).substr(0, cut));
        if (!made.ok()) return made.error();
    }
    return fs.Write(storageFile_, text);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Design* DesignLibrary::FindLocked(std::string_view id) {
    for (Design& d : designs_)
        if (d.id == id) return &d;
    return nullptr;
}

const DesignCategory* DesignLibrary::CategoryLocked(std::string_view id) const {
    for (const DesignCategory& c : categories_)
        if (c.id == id) return &c;
    return nullptr;
}

// "<prefix>-<n>" with n one past the largest already used, so an id is never a live one.
std::string DesignLibrary::NextIdLocked(std::string_view prefix) const {
    const std::string lead = std::string(prefix) + "-";
    long long highest = 0;
    auto scan = [&](const std::string& id) {
        if (id.rfind(lead, 0) != 0) return;
        const std::string tail = id.substr(lead.size());
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); }))
            highest = std::max(highest, std::stoll(tail));
    };
    for (const Design& d : designs_) scan(d.id);
    for (const DesignCategory& c : categories_) scan(c.id);
    return lead + std::to_string(highest + 1);
}

template <typename Change>
Result<void> DesignLibrary::Edit(std::string_view id, Change&& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    Design* d = FindLocked(id);
    if (!d) return Missing();
    const Design before = *d;
    if constexpr (std::is_void_v<decltype(change(*d))>) {
        change(*d);
    } else if (auto applied = change(*d); !applied.ok()) {
        *d = before;
        return applied.error();
    }
    if (auto saved = SaveLocked(); !saved.ok()) {
        *d = before;
        return saved.error();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

std::vector<DesignCategory> DesignLibrary::Categories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return categories_;
}

Result<DesignCategory> DesignLibrary::CreateCategory(std::string_view name, std::string_view icon) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, "a category needs a name");
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = Lower(clean);
    for (const DesignCategory& c : categories_)
        if (Lower(c.name) == key) return Error::Make(Err::AlreadyExists, kModule, "a category with that name already exists");
    DesignCategory made{ NextIdLocked("cat"), clean, icon.empty() ? "folder" : std::string(icon), false };
    categories_.push_back(made);
    if (auto saved = SaveLocked(); !saved.ok()) {
        categories_.pop_back();
        return saved.error();
    }
    return made;
}

Result<void> DesignLibrary::RenameCategory(std::string_view id, std::string_view name) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, "a category needs a name");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const DesignCategory& c) { return c.id == id; });
    if (it == categories_.end()) return Error::Make(Err::NotFound, kModule, "no such category");
    if (it->isDefault) return Error::Make(Err::InvalidState, kModule, "a default category cannot be renamed");
    const std::string key = Lower(clean);
    for (const DesignCategory& c : categories_)
        if (&c != &*it && Lower(c.name) == key)
            return Error::Make(Err::AlreadyExists, kModule, "a category with that name already exists");
    const std::string before = it->name;
    it->name = clean;
    if (auto saved = SaveLocked(); !saved.ok()) {
        it->name = before;
        return saved.error();
    }
    return {};
}

Result<void> DesignLibrary::SetCategoryIcon(std::string_view id, std::string_view icon) {
    if (icon.empty()) return Error::Make(Err::InvalidArgument, kModule, "an icon name is needed");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const DesignCategory& c) { return c.id == id; });
    if (it == categories_.end()) return Error::Make(Err::NotFound, kModule, "no such category");
    if (it->isDefault) return Error::Make(Err::InvalidState, kModule, "a default category keeps its icon");
    const std::string before = it->icon;
    it->icon = std::string(icon);
    if (auto saved = SaveLocked(); !saved.ok()) {
        it->icon = before;
        return saved.error();
    }
    return {};
}

Result<void> DesignLibrary::DeleteCategory(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const DesignCategory& c) { return c.id == id; });
    if (it == categories_.end()) return Error::Make(Err::NotFound, kModule, "no such category");
    if (it->isDefault) return Error::Make(Err::InvalidState, kModule, "a default category cannot be removed");
    const DesignCategory removed = *it;
    const size_t position = static_cast<size_t>(it - categories_.begin());
    categories_.erase(it);
    std::vector<std::string> unfiled;   // the designs that were in it
    for (Design& d : designs_)
        if (d.category == removed.id) {
            unfiled.push_back(d.id);
            d.category.clear();
        }
    if (auto saved = SaveLocked(); !saved.ok()) {
        categories_.insert(categories_.begin() + static_cast<std::ptrdiff_t>(position), removed);
        for (Design& d : designs_)
            if (std::find(unfiled.begin(), unfiled.end(), d.id) != unfiled.end()) d.category = removed.id;
        return saved.error();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Designs
// ---------------------------------------------------------------------------

std::vector<Design> DesignLibrary::Designs(std::string_view filter, std::string_view query) const {
    std::vector<Design> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Design& d : designs_)
            if (filter.empty() || (filter == kUnlabeled ? d.category.empty() : d.category == filter))
                out.push_back(d);
    }
    std::stable_sort(out.begin(), out.end(), [](const Design& a, const Design& b) { return NaturalLess(a.name, b.name); });

    const std::vector<std::string> words = Words(query);
    if (words.empty()) return out;
    struct Hit { Design design; int rank; };
    std::vector<Hit> hits;
    for (Design& d : out) {
        const std::string name = Lower(d.name);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return name.find(w) != std::string::npos; }))
            continue;
        hits.push_back({ std::move(d), name.rfind(words.front(), 0) == 0 ? 0 : 1 });
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rank < b.rank; });
    std::vector<Design> ranked;
    ranked.reserve(hits.size());
    for (Hit& h : hits) ranked.push_back(std::move(h.design));
    return ranked;
}

Result<Design> DesignLibrary::Get(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Design& d : designs_)
        if (d.id == id) return d;
    return Missing();
}

DesignCounts DesignLibrary::Counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DesignCounts counts;
    for (const DesignCategory& c : categories_) counts.byCategory[c.id] = 0;
    for (const Design& d : designs_) {
        ++counts.all;
        if (d.category.empty()) ++counts.unlabeled;
        else ++counts.byCategory[d.category];
    }
    return counts;
}

Result<Design> DesignLibrary::Create(std::string_view name, std::string_view categoryId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!categoryId.empty() && !CategoryLocked(categoryId))
        return Error::Make(Err::InvalidArgument, kModule, "no such category");

    std::string clean = Trim(name);
    if (clean.empty()) {
        // "Overlay", "Overlay 2", ...: the first one not taken.
        auto taken = [&](const std::string& candidate) {
            const std::string key = Lower(candidate);
            return std::any_of(designs_.begin(), designs_.end(), [&](const Design& d) { return Lower(d.name) == key; });
        };
        clean = config_.defaultName;
        for (int n = 2; taken(clean); ++n) clean = std::format("{} {}", config_.defaultName, n);
    }

    Design d;
    d.id = NextIdLocked(config_.idPrefix);
    d.name = clean;
    d.category = std::string(categoryId);
    d.color = "#0b57a2";
    if (config_.starterBlocks) {
        // Ids come from the same rules as every other block: the engine claims them.
        WithBlocks(d, [&](pres::Presentation& scratch) {
            for (pres::ContentBlock block : config_.starterBlocks())
                (void)pres::ShowEditor::AddBlock(scratch, kScratchSlide, std::move(block));
            return 0;
        });
    }
    designs_.push_back(d);
    if (auto saved = SaveLocked(); !saved.ok()) {
        designs_.pop_back();
        return saved.error();
    }
    return d;
}

Result<Design> DesignLibrary::Duplicate(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Design* source = FindLocked(id);
    if (!source) return Missing();
    Design copy = *source;
    copy.id = NextIdLocked(config_.idPrefix);
    copy.name = source->name + " copy";
    copy.isDefault = false;
    copy.locked = false;
    designs_.push_back(copy);
    if (auto saved = SaveLocked(); !saved.ok()) {
        designs_.pop_back();
        return saved.error();
    }
    return copy;
}

Result<void> DesignLibrary::Rename(std::string_view id, std::string_view name) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, std::format("a {} needs a name", config_.noun));
    return Edit(id, [&](Design& d) { d.name = clean; });
}

Result<void> DesignLibrary::SetColor(std::string_view id, std::string_view color) {
    return Edit(id, [&](Design& d) { d.color = std::string(color); });
}

Result<void> DesignLibrary::SetCategory(std::string_view id, std::string_view categoryId) {
    return Edit(id, [&](Design& d) -> Result<void> {
        // (Edit holds the lock, so the category table is read directly.)
        if (!categoryId.empty() && !CategoryLocked(categoryId))
            return Error::Make(Err::InvalidArgument, kModule, "no such category");
        d.category = std::string(categoryId);
        return {};
    });
}

Result<void> DesignLibrary::SetContentType(std::string_view id, std::string_view contentType) {
    return Edit(id, [&](Design& d) { d.contentType = std::string(contentType); });
}

Result<void> DesignLibrary::SetLocked(std::string_view id, bool locked) {
    return Edit(id, [&](Design& d) { d.locked = locked; });
}

Result<void> DesignLibrary::SetPlaceUnderSlide(std::string_view id, bool under) {
    return Edit(id, [&](Design& d) { d.placeUnderSlide = under; });
}

Result<void> DesignLibrary::SetDisplayDuration(std::string_view id, double seconds) {
    if (seconds < 0) return Error::Make(Err::InvalidArgument, kModule, "a duration cannot be negative");
    return Edit(id, [&](Design& d) { d.displayDuration = seconds; });
}

Result<void> DesignLibrary::Delete(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(designs_.begin(), designs_.end(), [&](const Design& d) { return d.id == id; });
    if (it == designs_.end()) return Missing();
    const Design removed = *it;
    const size_t position = static_cast<size_t>(it - designs_.begin());
    // Only a shipped design the app itself would put back needs remembering.
    const bool remember = removed.isDefault;
    designs_.erase(it);
    if (remember) deletedDefaults_.insert(removed.id);
    if (auto saved = SaveLocked(); !saved.ok()) {
        designs_.insert(designs_.begin() + static_cast<std::ptrdiff_t>(position), removed);
        if (remember) deletedDefaults_.erase(removed.id);
        return saved.error();
    }
    return {};
}

Result<size_t> DesignLibrary::RestoreDefaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = designs_.size();
    deletedDefaults_.clear();
    EnsureDefaultsLocked();
    const size_t restored = designs_.size() - before;
    if (restored == 0) return size_t{ 0 };
    if (auto saved = SaveLocked(); !saved.ok()) return saved.error();
    return restored;
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

Result<void> DesignLibrary::SetContent(std::string_view id, std::string background,
                                       std::vector<pres::ContentBlock> blocks) {
    // What the canvas sends is checked by the engine's own block rules before anything is changed.
    if (auto ok = pres::CheckBlocks(blocks); !ok.ok()) return ok;
    if (!background.empty() && !pres::IsColorText(background))
        return Error::Make(Err::InvalidArgument, kModule, std::format("'{}' is not a colour (background)", background));
    return Edit(id, [&](Design& d) {
        d.background = background.empty() ? std::string("transparent") : std::move(background);
        d.blocks = std::move(blocks);
    });
}

Result<std::string> DesignLibrary::AddBlock(std::string_view id, pres::ContentBlock block, std::string_view above) {
    if (auto ok = pres::CheckBlock(block, /*requireId=*/false); !ok.ok()) return ok.error();
    std::string claimed;
    auto done = Edit(id, [&](Design& d) -> Result<void> {
        auto added = WithBlocks(d, [&](pres::Presentation& scratch) -> Result<std::string> {
            std::optional<size_t> index;
            if (!above.empty()) {
                const auto& blocks = scratch.slides.front().blocks;
                auto at = std::find_if(blocks.begin(), blocks.end(), [&](const pres::ContentBlock& b) { return b.id == above; });
                if (at == blocks.end()) return Error::Make(Err::NotFound, kModule, "no such block");
                index = static_cast<size_t>(at - blocks.begin()) + 1;
            }
            return pres::ShowEditor::AddBlock(scratch, kScratchSlide, std::move(block), index);
        });
        if (!added.ok()) return added.error();
        claimed = added.value();
        return {};
    });
    if (!done.ok()) return done.error();
    return claimed;
}

Result<std::string> DesignLibrary::DuplicateBlock(std::string_view id, std::string_view blockId) {
    std::string claimed;
    auto done = Edit(id, [&](Design& d) -> Result<void> {
        auto copy = WithBlocks(d, [&](pres::Presentation& scratch) {
            return pres::ShowEditor::DuplicateBlock(scratch, kScratchSlide, blockId);
        });
        if (!copy.ok()) return copy.error();
        claimed = copy.value();
        return {};
    });
    if (!done.ok()) return done.error();
    return claimed;
}

Result<void> DesignLibrary::RemoveBlock(std::string_view id, std::string_view blockId) {
    return Edit(id, [&](Design& d) -> Result<void> {
        return WithBlocks(d, [&](pres::Presentation& scratch) {
            return pres::ShowEditor::RemoveBlock(scratch, kScratchSlide, blockId);
        });
    });
}

Result<pres::ContentBlock> DesignLibrary::Block(std::string_view id, std::string_view blockId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Design& d : designs_)
        if (d.id == id) {
            for (const pres::ContentBlock& b : d.blocks)
                if (b.id == blockId) return b;
            return Error::Make(Err::NotFound, kModule, "no such block");
        }
    return Missing();
}

Result<pres::SlideTemplate> DesignLibrary::AsTemplate(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Design& d : designs_)
        if (d.id == id) return ToTemplate(d);
    return Missing();
}

} // namespace bps::library
