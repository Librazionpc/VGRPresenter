#include "modules/overlays/OverlayLibrary.hpp"

#include "core/config/Json.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace bps::overlays {

namespace {

constexpr const char* kModule = "OverlayLibrary";

constexpr std::string_view kVisualsId = "visuals";

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
// The defaults (FreeShow's "Visuals" category and the overlays filed in it, laid out on the 1920 x 1080 canvas)
// ---------------------------------------------------------------------------

OverlayElement Box(double x, double y, double w, double h, std::string background) {
    OverlayElement e;
    e.kind = OverlayElement::Kind::Box;
    e.x = x; e.y = y; e.width = w; e.height = h;
    e.background = std::move(background);
    return e;
}

OverlayElement TextBox(double x, double y, double w, double h, std::string text, double fontSize,
                       std::string background = {}, bool bold = false, bool upper = false,
                       std::string align = "left") {
    OverlayElement e;
    e.kind = OverlayElement::Kind::Text;
    e.x = x; e.y = y; e.width = w; e.height = h;
    e.text = std::move(text);
    e.fontSize = fontSize;
    e.background = std::move(background);
    e.bold = bold;
    e.uppercase = upper;
    e.align = std::move(align);
    return e;
}

Overlay Make(std::string id, std::string name, std::string color, std::vector<OverlayElement> elements) {
    Overlay o;
    o.id = std::move(id);
    o.name = std::move(name);
    o.color = std::move(color);
    o.category = std::string(kVisualsId);
    o.isDefault = true;
    o.elements = std::move(elements);
    return o;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

using J = json::Value;

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

J ToJson(const OverlayElement& e) {
    J::Object o;
    o["kind"] = J::Number(static_cast<double>(e.kind));
    o["x"] = J::Number(e.x);
    o["y"] = J::Number(e.y);
    o["w"] = J::Number(e.width);
    o["h"] = J::Number(e.height);
    o["bg"] = J::String(e.background);
    o["radius"] = J::Number(e.radius);
    o["bw"] = J::Number(e.borderWidth);
    o["bc"] = J::String(e.borderColor);
    o["text"] = J::String(e.text);
    o["tc"] = J::String(e.textColor);
    o["fs"] = J::Number(e.fontSize);
    o["bold"] = J::Bool(e.bold);
    o["upper"] = J::Bool(e.uppercase);
    o["align"] = J::String(e.align);
    o["analog"] = J::Bool(e.analog);
    o["inset"] = J::Number(e.inset);
    return J(std::move(o));
}

OverlayElement ElementFromJson(const J& v) {
    OverlayElement e;
    const int kind = static_cast<int>(Num(v, "kind"));
    e.kind = kind >= 0 && kind <= static_cast<int>(OverlayElement::Kind::Corners)
                 ? static_cast<OverlayElement::Kind>(kind) : OverlayElement::Kind::Box;
    e.x = Num(v, "x");
    e.y = Num(v, "y");
    e.width = Num(v, "w");
    e.height = Num(v, "h");
    e.background = Str(v, "bg");
    e.radius = Num(v, "radius");
    e.borderWidth = Num(v, "bw");
    e.borderColor = Str(v, "bc");
    e.text = Str(v, "text");
    e.textColor = Str(v, "tc", "#ffffff");
    e.fontSize = Num(v, "fs", 48);
    e.bold = Flag(v, "bold");
    e.uppercase = Flag(v, "upper");
    e.align = Str(v, "align", "left");
    e.analog = Flag(v, "analog");
    e.inset = Num(v, "inset");
    return e;
}

J ToJson(const Overlay& o) {
    J::Object j;
    j["id"] = J::String(o.id);
    j["name"] = J::String(o.name);
    j["color"] = J::String(o.color);
    j["category"] = J::String(o.category);
    j["default"] = J::Bool(o.isDefault);
    j["locked"] = J::Bool(o.locked);
    j["duration"] = J::Number(o.displayDuration);
    J::Array items;
    for (const OverlayElement& e : o.elements) items.push_back(ToJson(e));
    j["items"] = J(std::move(items));
    return J(std::move(j));
}

J ToJson(const OverlayCategory& c) {
    J::Object j;
    j["id"] = J::String(c.id);
    j["name"] = J::String(c.name);
    j["icon"] = J::String(c.icon);
    j["default"] = J::Bool(c.isDefault);
    return J(std::move(j));
}

} // namespace

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

std::vector<OverlayCategory> OverlayLibrary::DefaultCategories() {
    return { OverlayCategory{ std::string(kVisualsId), "Visuals", "star", true } };
}

std::vector<Overlay> OverlayLibrary::DefaultOverlays() {
    std::vector<Overlay> out;

    // A white frame around the picture, a red dot and "REC".
    {
        OverlayElement frame = Box(36.5, 35, 1847.62, 1008.21, {});
        frame.borderWidth = 4;
        frame.borderColor = "#ffffff";
        OverlayElement dot = Box(80, 80, 40, 40, "red");
        dot.radius = 20;
        out.push_back(Make("recording", "Recording", "red",
                           { frame, dot, TextBox(140, 80, 100, 40, "REC", 40) }));
    }

    // The time, top right.
    {
        OverlayElement clock;
        clock.kind = OverlayElement::Kind::Clock;
        clock.x = 1450; clock.y = 70; clock.width = 470; clock.height = 150;
        clock.fontSize = 100;
        clock.align = "right";
        out.push_back(Make("clock", "Clock", "dodgerblue", { clock }));
    }

    // A round analog clock in the middle.
    {
        OverlayElement face;
        face.kind = OverlayElement::Kind::Clock;
        face.analog = true;
        face.x = 492; face.y = 72.5; face.width = 936.4; face.height = 936.4;
        face.background = "rgba(0,0,0,0.5)";
        face.radius = 500;
        face.borderWidth = 2;
        face.borderColor = "#ffffff";
        out.push_back(Make("clock_analog", "Clock (Analog)", "dodgerblue", { face }));
    }

    // A name lower third: an accent stripe, the name, and a title above it. Leaves by itself after 4 s.
    {
        Overlay name = Make("name", "Name", "#0b57a2",
                            { Box(80, 875, 750, 135, "#0b57a2"),
                              Box(80, 875, 50, 135, "#74cbfb"),
                              TextBox(130, 935, 700, 75, "Name Surname", 70, "#0b57a2"),
                              TextBox(130, 875, 700, 60, "Title", 40, "#006fcf", true, true) });
        name.displayDuration = 4;
        out.push_back(std::move(name));
    }

    // The screen's four corners rounded off.
    {
        OverlayElement corners;
        corners.kind = OverlayElement::Kind::Corners;
        corners.x = 0; corners.y = 0; corners.width = kCanvasWidth; corners.height = kCanvasHeight;
        corners.background = "#000000";
        corners.inset = 50;
        Overlay rounded = Make("rounded", "Rounded", {}, { corners });
        rounded.locked = true;
        out.push_back(std::move(rounded));
    }

    // The screen's edges softened.
    {
        OverlayElement vignette;
        vignette.kind = OverlayElement::Kind::Vignette;
        vignette.x = 0; vignette.y = 0; vignette.width = kCanvasWidth; vignette.height = kCanvasHeight;
        vignette.background = "#ffffff";
        vignette.inset = 248;
        Overlay v = Make("vignette", "Vignette", "#dddddd", { vignette });
        v.locked = true;
        out.push_back(std::move(v));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Construction, load, save
// ---------------------------------------------------------------------------

OverlayLibrary::OverlayLibrary(std::string storageFile) : storageFile_(std::move(storageFile)) {}

void OverlayLibrary::EnsureDefaultsLocked() {
    for (OverlayCategory& def : DefaultCategories()) {
        auto found = std::find_if(categories_.begin(), categories_.end(),
                                  [&](const OverlayCategory& c) { return c.id == def.id; });
        if (found != categories_.end()) {
            found->isDefault = true;   // the app's own, whatever the file said
            continue;
        }
        categories_.insert(categories_.begin() + static_cast<std::ptrdiff_t>(std::count_if(
                               categories_.begin(), categories_.end(), [](const OverlayCategory& c) { return c.isDefault; })),
                           std::move(def));
    }
    for (Overlay& def : DefaultOverlays()) {
        if (deletedDefaults_.contains(def.id)) continue;
        const bool present = std::any_of(overlays_.begin(), overlays_.end(),
                                         [&](const Overlay& o) { return o.id == def.id; });
        if (!present) overlays_.push_back(std::move(def));
    }
}

Result<void> OverlayLibrary::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::lock_guard<std::mutex> lock(mutex_);
    categories_.clear();
    overlays_.clear();
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
            return Error::Make(Err::ParseError, kModule, "the overlay library is damaged: " + parsed.error().message);
        }
        const J& root = parsed.value();
        if (const J* cats = root.Find("categories"); cats && cats->asArray())
            for (const J& c : *cats->asArray()) {
                OverlayCategory cat{ Str(c, "id"), Str(c, "name"), Str(c, "icon", "folder"), Flag(c, "default") };
                if (!cat.id.empty() && !cat.name.empty()) categories_.push_back(std::move(cat));
            }
        if (const J* list = root.Find("overlays"); list && list->asArray())
            for (const J& o : *list->asArray()) {
                Overlay ov;
                ov.id = Str(o, "id");
                ov.name = Str(o, "name");
                if (ov.id.empty() || ov.name.empty()) continue;
                ov.color = Str(o, "color");
                ov.category = Str(o, "category");
                ov.isDefault = Flag(o, "default");
                ov.locked = Flag(o, "locked");
                ov.displayDuration = Num(o, "duration");
                if (const J* items = o.Find("items"); items && items->asArray())
                    for (const J& e : *items->asArray()) ov.elements.push_back(ElementFromJson(e));
                overlays_.push_back(std::move(ov));
            }
        if (const J* gone = root.Find("deletedDefaults"); gone && gone->asArray())
            for (const J& id : *gone->asArray()) deletedDefaults_.insert(std::string(id.asString()));
    }

    EnsureDefaultsLocked();
    // An overlay filed in a category that no longer exists is just unlabeled.
    for (Overlay& o : overlays_)
        if (!o.category.empty() && !CategoryLocked(o.category)) o.category.clear();
    return {};
}

Result<void> OverlayLibrary::SaveLocked() const {
    J::Array cats;
    for (const OverlayCategory& c : categories_) cats.push_back(ToJson(c));
    J::Array overlays;
    for (const Overlay& o : overlays_) overlays.push_back(ToJson(o));
    J::Array gone;
    for (const std::string& id : deletedDefaults_) gone.push_back(J::String(id));
    J::Object root;
    root["version"] = J::Number(1);
    root["categories"] = J(std::move(cats));
    root["overlays"] = J(std::move(overlays));
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

Overlay* OverlayLibrary::FindLocked(std::string_view id) {
    for (Overlay& o : overlays_)
        if (o.id == id) return &o;
    return nullptr;
}

const OverlayCategory* OverlayLibrary::CategoryLocked(std::string_view id) const {
    for (const OverlayCategory& c : categories_)
        if (c.id == id) return &c;
    return nullptr;
}

// "<prefix>-<n>" with n one past the largest already used, so an id is never a live one.
std::string OverlayLibrary::NextIdLocked(std::string_view prefix) const {
    const std::string lead = std::string(prefix) + "-";
    long long highest = 0;
    auto scan = [&](const std::string& id) {
        if (id.rfind(lead, 0) != 0) return;
        const std::string tail = id.substr(lead.size());
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); }))
            highest = std::max(highest, std::stoll(tail));
    };
    for (const Overlay& o : overlays_) scan(o.id);
    for (const OverlayCategory& c : categories_) scan(c.id);
    return lead + std::to_string(highest + 1);
}

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

std::vector<OverlayCategory> OverlayLibrary::Categories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return categories_;
}

Result<OverlayCategory> OverlayLibrary::CreateCategory(std::string_view name, std::string_view icon) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, "a category needs a name");
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = Lower(clean);
    for (const OverlayCategory& c : categories_)
        if (Lower(c.name) == key) return Error::Make(Err::AlreadyExists, kModule, "a category with that name already exists");
    OverlayCategory made{ NextIdLocked("cat"), clean, icon.empty() ? "folder" : std::string(icon), false };
    categories_.push_back(made);
    if (auto saved = SaveLocked(); !saved.ok()) {
        categories_.pop_back();
        return saved.error();
    }
    return made;
}

Result<void> OverlayLibrary::RenameCategory(std::string_view id, std::string_view name) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, "a category needs a name");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const OverlayCategory& c) { return c.id == id; });
    if (it == categories_.end()) return Error::Make(Err::NotFound, kModule, "no such category");
    if (it->isDefault) return Error::Make(Err::InvalidState, kModule, "a default category cannot be renamed");
    const std::string key = Lower(clean);
    for (const OverlayCategory& c : categories_)
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

Result<void> OverlayLibrary::SetCategoryIcon(std::string_view id, std::string_view icon) {
    if (icon.empty()) return Error::Make(Err::InvalidArgument, kModule, "an icon name is needed");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const OverlayCategory& c) { return c.id == id; });
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

Result<void> OverlayLibrary::DeleteCategory(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(categories_.begin(), categories_.end(), [&](const OverlayCategory& c) { return c.id == id; });
    if (it == categories_.end()) return Error::Make(Err::NotFound, kModule, "no such category");
    if (it->isDefault) return Error::Make(Err::InvalidState, kModule, "a default category cannot be removed");
    const OverlayCategory removed = *it;
    categories_.erase(it);
    std::vector<std::string> unfiled;   // the overlays that were in it
    for (Overlay& o : overlays_)
        if (o.category == removed.id) {
            unfiled.push_back(o.id);
            o.category.clear();
        }
    if (auto saved = SaveLocked(); !saved.ok()) {
        categories_.push_back(removed);
        for (Overlay& o : overlays_)
            if (std::find(unfiled.begin(), unfiled.end(), o.id) != unfiled.end()) o.category = removed.id;
        return saved.error();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------

std::vector<Overlay> OverlayLibrary::Overlays(std::string_view filter, std::string_view query) const {
    std::vector<Overlay> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Overlay& o : overlays_) {
            if (filter.empty() || (filter == kUnlabeled ? o.category.empty() : o.category == filter))
                out.push_back(o);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Overlay& a, const Overlay& b) { return NaturalLess(a.name, b.name); });

    const std::vector<std::string> words = Words(query);
    if (words.empty()) return out;
    struct Hit { Overlay overlay; int rank; };
    std::vector<Hit> hits;
    for (Overlay& o : out) {
        const std::string name = Lower(o.name);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return name.find(w) != std::string::npos; }))
            continue;
        hits.push_back({ std::move(o), name.rfind(words.front(), 0) == 0 ? 0 : 1 });
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rank < b.rank; });
    std::vector<Overlay> ranked;
    ranked.reserve(hits.size());
    for (Hit& h : hits) ranked.push_back(std::move(h.overlay));
    return ranked;
}

Result<Overlay> OverlayLibrary::Get(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Overlay& o : overlays_)
        if (o.id == id) return o;
    return Error::Make(Err::NotFound, kModule, "no such overlay");
}

OverlayCounts OverlayLibrary::Counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    OverlayCounts counts;
    for (const OverlayCategory& c : categories_) counts.byCategory[c.id] = 0;
    for (const Overlay& o : overlays_) {
        ++counts.all;
        if (o.category.empty()) ++counts.unlabeled;
        else ++counts.byCategory[o.category];
    }
    return counts;
}

Result<Overlay> OverlayLibrary::CreateOverlay(std::string_view name, std::string_view categoryId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!categoryId.empty() && !CategoryLocked(categoryId))
        return Error::Make(Err::InvalidArgument, kModule, "no such category");

    std::string clean = Trim(name);
    if (clean.empty()) {
        // "Overlay", "Overlay 2", ...: the first one not taken.
        auto taken = [&](const std::string& candidate) {
            const std::string key = Lower(candidate);
            return std::any_of(overlays_.begin(), overlays_.end(), [&](const Overlay& o) { return Lower(o.name) == key; });
        };
        clean = "Overlay";
        for (int n = 2; taken(clean); ++n) clean = std::format("Overlay {}", n);
    }

    Overlay o;
    o.id = NextIdLocked("ov");
    o.name = clean;
    o.category = std::string(categoryId);
    o.color = "#0b57a2";
    // A starter lower third the user can restyle: a bar with a line of text.
    o.elements = { Box(80, 900, 12, 100, "#74cbfb"), TextBox(92, 900, 760, 100, "Text", 64, "#0b57a2") };
    overlays_.push_back(o);
    if (auto saved = SaveLocked(); !saved.ok()) {
        overlays_.pop_back();
        return saved.error();
    }
    return o;
}

Result<Overlay> OverlayLibrary::Duplicate(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Overlay* source = FindLocked(id);
    if (!source) return Error::Make(Err::NotFound, kModule, "no such overlay");
    Overlay copy = *source;
    copy.id = NextIdLocked("ov");
    copy.name = source->name + " copy";
    copy.isDefault = false;
    copy.locked = false;
    overlays_.push_back(copy);
    if (auto saved = SaveLocked(); !saved.ok()) {
        overlays_.pop_back();
        return saved.error();
    }
    return copy;
}

// One edit of one overlay: find it, apply `change`, save, and put the old value back if the save fails.
#define BPS_OVERLAY_EDIT(id, change)                                                        \
    std::lock_guard<std::mutex> lock(mutex_);                                               \
    Overlay* o = FindLocked(id);                                                            \
    if (!o) return Error::Make(Err::NotFound, kModule, "no such overlay");                  \
    const Overlay before = *o;                                                              \
    change;                                                                                 \
    if (auto saved = SaveLocked(); !saved.ok()) {                                           \
        *o = before;                                                                        \
        return saved.error();                                                               \
    }                                                                                       \
    return {}

Result<void> OverlayLibrary::Rename(std::string_view id, std::string_view name) {
    const std::string clean = Trim(name);
    if (clean.empty()) return Error::Make(Err::InvalidArgument, kModule, "an overlay needs a name");
    BPS_OVERLAY_EDIT(id, o->name = clean);
}

Result<void> OverlayLibrary::SetColor(std::string_view id, std::string_view color) {
    BPS_OVERLAY_EDIT(id, o->color = std::string(color));
}

Result<void> OverlayLibrary::SetCategory(std::string_view id, std::string_view categoryId) {
    std::lock_guard<std::mutex> lock(mutex_);
    Overlay* o = FindLocked(id);
    if (!o) return Error::Make(Err::NotFound, kModule, "no such overlay");
    if (!categoryId.empty() && !CategoryLocked(categoryId))
        return Error::Make(Err::InvalidArgument, kModule, "no such category");
    const std::string before = o->category;
    o->category = std::string(categoryId);
    if (auto saved = SaveLocked(); !saved.ok()) {
        o->category = before;
        return saved.error();
    }
    return {};
}

Result<void> OverlayLibrary::SetLocked(std::string_view id, bool locked) {
    BPS_OVERLAY_EDIT(id, o->locked = locked);
}

Result<void> OverlayLibrary::SetDisplayDuration(std::string_view id, double seconds) {
    if (seconds < 0) return Error::Make(Err::InvalidArgument, kModule, "a duration cannot be negative");
    BPS_OVERLAY_EDIT(id, o->displayDuration = seconds);
}

Result<void> OverlayLibrary::SetElements(std::string_view id, std::vector<OverlayElement> elements) {
    BPS_OVERLAY_EDIT(id, o->elements = std::move(elements));
}

#undef BPS_OVERLAY_EDIT

Result<void> OverlayLibrary::Delete(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(overlays_.begin(), overlays_.end(), [&](const Overlay& o) { return o.id == id; });
    if (it == overlays_.end()) return Error::Make(Err::NotFound, kModule, "no such overlay");
    const Overlay removed = *it;
    const size_t position = static_cast<size_t>(it - overlays_.begin());
    // Only a default the app itself would put back needs remembering.
    const bool remember = removed.isDefault;
    overlays_.erase(it);
    if (remember) deletedDefaults_.insert(removed.id);
    if (auto saved = SaveLocked(); !saved.ok()) {
        overlays_.insert(overlays_.begin() + static_cast<std::ptrdiff_t>(position), removed);
        if (remember) deletedDefaults_.erase(removed.id);
        return saved.error();
    }
    return {};
}

Result<size_t> OverlayLibrary::RestoreDefaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = overlays_.size();
    deletedDefaults_.clear();
    EnsureDefaultsLocked();
    const size_t restored = overlays_.size() - before;
    if (restored == 0) return size_t{ 0 };
    if (auto saved = SaveLocked(); !saved.ok()) return saved.error();
    return restored;
}

} // namespace bps::overlays
