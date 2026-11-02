#include "modules/import/ImportEngine.hpp"

#include "core/config/Json.hpp"
#include "modules/bible/BibleEngine.hpp"
#include "modules/import/Archive.hpp"
#include "modules/import/SongText.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/media/MediaLibrary.hpp"
#include "modules/songs/ISongProvider.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>

namespace bps::import {
namespace {
constexpr const char* kModule = "ImportEngine";
using J = json::Value;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

ImportFormat Fmt(std::string id, std::string name, std::vector<std::string> ext, ImportKind kind, std::string section, bool available,
                 std::string icon = {}, bool primary = false, std::string tutorial = {}, std::string description = {}) {
    ImportFormat f;
    f.id = std::move(id); f.name = std::move(name); f.extensions = std::move(ext); f.kind = kind; f.section = std::move(section);
    f.available = available; f.icon = std::move(icon); f.primary = primary; f.tutorial = std::move(tutorial); f.description = std::move(description);
    return f;
}

std::vector<ImportFormat> BuildFormats() {
    using K = ImportKind;
    std::vector<ImportFormat> f;

    // ---- FreeShow's own files ----
    f.push_back(Fmt("freeshow", "FreeShow Song/Presentation File", { "show", "json" }, K::Show, "freeshow", true, "presentation", true));
    f.push_back(Fmt("freeshow_project", "FreeShow Project File", { "project", "shows", "json", "zip" }, K::Project, "freeshow", true, "folder", true));
    f.push_back(Fmt("freeshow_template", "FreeShow Template File", { "fstemplate", "fst", "template", "json", "zip" }, K::Template, "freeshow", false, "layoutTemplate"));
    f.push_back(Fmt("freeshow_action", "FreeShow Action File", { "fsaction", "action", "json" }, K::Other, "freeshow", false, "wrench"));
    f.push_back(Fmt("freeshow_stage", "FreeShow Stage Layout File", { "fsstage", "stage", "json" }, K::Other, "freeshow", false, "presentation"));
    f.push_back(Fmt("freeshow_theme", "FreeShow Theme File", { "fstheme", "theme", "json" }, K::Other, "freeshow", false, "layers"));

    // ---- media ----
    f.push_back(Fmt("lessons", "Lessons.church", { "json", "olp", "olf" }, K::Show, "media", false, "book", false, {}, "ChurchApps - https://lessons.church"));
    f.push_back(Fmt("pdf", "PDF", { "pdf" }, K::Show, "media", false, "fileText", false, {}, "Added to your project"));
    f.push_back(Fmt("powerpoint", "PowerPoint", { "ppt", "pptx" }, K::Show, "media", false, "presentation"));

    // ---- text: songs and lyrics ----
    f.push_back(Fmt("txt", "Text", { "txt" }, K::Show, "text", true, "fileText", true));
    f.push_back(Fmt("csv", "CSV", { "csv" }, K::Show, "text", true, "layoutDashboard"));
    f.push_back(Fmt("chordpro", "ChordPro", { "cho", "crd", "chopro", "chordpro", "chord", "pro", "txt", "onsong" }, K::Show, "text", true, "music", true));
    f.push_back(Fmt("word", "Word", { "doc", "docx" }, K::Show, "text", false, "fileText"));
    f.push_back(Fmt("propresenter", "ProPresenter", { "pro4", "pro5", "pro6", "pro", "json", "probundle" }, K::Show, "text", true, "presentation"));
    f.push_back(Fmt("easyworship", "EasyWorship", { "db" }, K::Show, "text", false, "music", false,
                    "Import the SongsWords.db / SongWords.db file from the Data folder. Optionally select Songs.db at the same time to also import the title and metadata.\n\nOften located in the Documents folder: Documents/Softouch/EasyWorship/Default/v6.1/Databases/Data/"));
    f.push_back(Fmt("videopsalm", "VideoPsalm", { "json", "vpc" }, K::Show, "text", false, "music", false,
                    "Find the .vpc or .json file(s), often located in Documents\\VideoPsalm\\Songbooks"));
    f.push_back(Fmt("openlp", "OpenLP/OpenLyrics", { "xml", "sqlite" }, K::Show, "text", true, "music"));
    f.push_back(Fmt("opensong", "OpenSong", {}, K::Show, "text", true, "music"));
    f.push_back(Fmt("mediashout", "MediaShout", { "ssc", "xml", "mdb" }, K::Show, "text", false, "music"));
    f.push_back(Fmt("quelea", "Quelea", { "xml", "qsp" }, K::Show, "text", false, "music"));
    f.push_back(Fmt("softprojector", "SoftProjector", { "sps" }, K::Show, "text", false, "music"));
    f.push_back(Fmt("songbeamer", "Songbeamer", {}, K::Show, "text", false, "music"));
    f.push_back(Fmt("easyslides", "Easyslides", { "xml" }, K::Show, "text", false, "music"));
    f.push_back(Fmt("verseview", "VerseVIEW", { "xml" }, K::Show, "text", false, "music"));

    // ---- scripture ----
    f.push_back(Fmt("bible_xml", "Bible - Zefania XML", { "xml" }, K::Bible, "bible", true, "bookOpen"));
    f.push_back(Fmt("bible_osis", "Bible - OSIS", { "osis", "xml" }, K::Bible, "bible", true, "bookOpen"));
    f.push_back(Fmt("bible_usfm", "Bible - USFM", { "usfm", "sfm" }, K::Bible, "bible", true, "bookOpen"));
    f.push_back(Fmt("bible_json", "Bible - JSON", { "json" }, K::Bible, "bible", true, "bookOpen"));
    f.push_back(Fmt("bible_txt", "Bible - text", { "txt" }, K::Bible, "bible", true, "bookOpen"));

    // ---- calendar ----
    f.push_back(Fmt("calendar", "Calendar (ICS)", { "ics" }, K::Calendar, "calendar", false, "calendar"));
    return f;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSV: FreeShow makes one slide of every line, one text box of every field
// ---------------------------------------------------------------------------------------------------------------------

// Fields of a line: "quoted, with commas", ""\"three quotes\"" (kept as one quoted word) or plain.
std::vector<std::string> CsvFields(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        if (line.compare(i, 3, "\"\"\"") == 0) {   // three quotes: a word that keeps one pair of quotes
            const size_t end = line.find("\"\"\"", i + 3);
            if (end != std::string::npos && end > i + 3) { out.push_back("\"" + line.substr(i + 3, end - i - 3) + "\""); i = end + 3; continue; }
        }
        if (line[i] == '"') {
            const size_t end = line.find('"', i + 1);
            if (end != std::string::npos) { out.push_back(line.substr(i + 1, end - i - 1)); i = end + 1; continue; }
        }
        if (line[i] == ',') { ++i; continue; }
        const size_t end = line.find(',', i);
        out.push_back(line.substr(i, end == std::string::npos ? std::string::npos : end - i));
        i = end == std::string::npos ? line.size() : end;
    }
    return out;
}

ImportedShow ParseCsv(const ImportFile& file) {
    ImportedShow show;
    show.name = file.name;
    show.category = "song";
    show.origin = "csv";
    std::string text;
    for (char c : file.content) if (c != '\r') text += c; else if (!text.empty() && text.back() != '\n') text += '\n';
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        std::string body;
        for (const std::string& field : CsvFields(line)) { if (!body.empty()) body += "\n"; body += field; }
        if (body.empty()) continue;   // empty slides are dropped
        ImportedSection section;
        section.group = "verse";
        section.label = "Verse";
        section.slides.push_back({ body, {}, {}, {} });
        show.sections.push_back(std::move(section));
    }
    return show;
}

// ---------------------------------------------------------------------------------------------------------------------
// the engine's song providers -> imported shows
// ---------------------------------------------------------------------------------------------------------------------

std::string ChordsJsonOf(const song::SongSection& section) {
    bool any = false;
    std::string out = "[";
    for (size_t l = 0; l < section.lines.size(); ++l) {
        if (l) out += ",";
        out += "[";
        for (size_t c = 0; c < section.lines[l].chords.size(); ++c) {
            if (c) out += ",";
            const auto& ref = section.lines[l].chords[c];
            std::string key = ref.chord.Display();
            std::string escaped;
            for (char ch : key) { if (ch == '"' || ch == '\\') escaped += '\\'; escaped += ch; }
            out += std::format("{{\"pos\":{},\"key\":\"{}\"}}", ref.column, escaped);
            any = true;
        }
        out += "]";
    }
    out += "]";
    return any ? out : std::string();
}

std::string ParentFolderName(std::string_view path) {
    if (const size_t member = path.find("::"); member != std::string_view::npos) path = path.substr(0, member);
    const size_t leaf = path.find_last_of("/\\");
    if (leaf == std::string_view::npos) return {};
    const std::string_view parent = path.substr(0, leaf);
    const size_t slash = parent.find_last_of("/\\");
    std::string name(parent.substr(slash == std::string_view::npos ? 0 : slash + 1));
    // A category name may not carry path characters or end in a space or dot.
    for (char& c : name)
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ||
            static_cast<unsigned char>(c) < 0x20)
            c = '-';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name == "." || name == "..") return {};
    return name;
}

// The name part of a path, whatever separator the reference used.
std::string LeafName(std::string_view path) {
    const size_t cut = path.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

bool EndsWithNoCase(std::string_view text, std::string_view tail) {
    if (tail.empty() || tail.size() > text.size()) return false;
    const size_t offset = text.size() - tail.size();
    for (size_t i = 0; i < tail.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
            std::tolower(static_cast<unsigned char>(tail[i]))) return false;
    return true;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::mutex& FolderCacheMutex() {
    static std::mutex mutex;
    return mutex;
}
std::map<std::string, std::vector<std::string>>& FolderCache() {
    static std::map<std::string, std::vector<std::string>> cache;
    return cache;
}

const std::vector<std::string>& FolderEntries(const std::string& folder) {
    {
        std::lock_guard<std::mutex> lock(FolderCacheMutex());
        const auto it = FolderCache().find(folder);
        if (it != FolderCache().end()) return it->second;
    }
    std::vector<std::string> entries;
    // Outside the lock: a directory read is a PAL call and must never be held
    // under a lock this process can re-enter (lock-scope rule).
    if (auto listed = platform::PlatformAccessor::Get().Filesystem().Enumerate(folder); listed.ok())
        entries = std::move(listed.value());
    std::lock_guard<std::mutex> lock(FolderCacheMutex());
    return FolderCache().emplace(folder, std::move(entries)).first->second;
}

void ResetFolderCache() {
    std::lock_guard<std::mutex> lock(FolderCacheMutex());
    FolderCache().clear();
}

std::string ResolveMediaPath(const std::string& reference, const std::string& filePath) {
    if (reference.empty()) return {};
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string name = LeafName(reference);
    const bool absolute = reference.size() > 2 &&
                          (reference[1] == ':' || reference[0] == '/' || reference[0] == '\\');

    // The folder the file (or the reference) names, above the document.
    std::vector<std::string> ancestors;
    {
        std::string dir = filePath;
        for (int level = 0; level < 6; ++level) {
            const size_t cut = dir.find_last_of("/\\");
            if (cut == std::string::npos) break;
            dir.resize(cut);
            if (dir.empty()) break;
            ancestors.push_back(dir);
        }
    }

    // 1. Exactly where it says, for a path that still holds here.
    if (absolute && fs.IsRegularFile(reference)) return reference;
    if (!absolute) {
        for (const std::string& dir : ancestors) {
            const std::string candidate = fs.Join(dir, reference);
            if (fs.IsRegularFile(candidate)) return candidate;
        }
    }

    // 2. The library's media folders, by the file's own name.
    if (!name.empty()) {
        for (const std::string& dir : ancestors)
            for (const char* sub : { "/Media/Assets/", "/Media/", "/media/" }) {
                const std::string candidate = dir + sub + name;
                if (fs.IsRegularFile(candidate)) return candidate;
            }
        // 3. Under the media id prefixed to that name (ProPresenter's own
        //    storage), or — for a reference that is just an id — a file whose
        //    stem IS the id.
        const size_t dot = name.find_last_of('.');
        const bool nameHasExtension = dot != std::string::npos && dot + 1 < name.size();
        for (const std::string& dir : ancestors)
            for (const char* sub : { "/Media/Assets", "/Media", "/media" }) {
                for (const std::string& entry : FolderEntries(dir + sub)) {
                    const std::string leaf = LeafName(entry);
                    bool named = false;
                    if (nameHasExtension) {
                        named = EndsWithNoCase(leaf, name);
                    } else {
                        const size_t entryDot = leaf.find_last_of('.');
                        const std::string entryStem = entryDot == std::string::npos ? leaf : leaf.substr(0, entryDot);
                        named = EqualsNoCase(entryStem, name);
                    }
                    if (!named) continue;
                    if (fs.IsRegularFile(entry)) return entry;
                }
            }
    }
    return {};
}

ImportedShow SongToImported(const song::Song& s, const ImportFile& file, const std::string& origin) {
    ImportedShow show;
    show.origin = origin;
    show.category = "song";
    show.sourceFolder = ParentFolderName(file.path);
    for (const song::MediaRef& ref : s.metadata.mediaRefs) {
        const std::string path = ResolveMediaPath(ref.mediaId, file.path);
        if (path.empty() || !media::MediaKindOf(path)) continue;   // only files the library can show
        if (std::find(show.mediaRefs.begin(), show.mediaRefs.end(), path) == show.mediaRefs.end())
            show.mediaRefs.push_back(path);
    }
    const song::SongMetadata& m = s.metadata;
    show.name = m.title.empty() ? file.name : m.title;
    show.meta["title"] = show.name;
    std::string authors;
    for (const std::string& a : m.authors) { if (!authors.empty()) authors += ", "; authors += a; }
    if (!authors.empty()) show.meta["author"] = authors;
    if (!m.copyright.empty()) show.meta["copyright"] = m.copyright;
    if (!m.ccli.empty()) show.meta["CCLI"] = m.ccli;
    if (!m.originalKey.empty()) show.meta["key"] = m.originalKey;
    show.notes = m.notes;

    std::vector<const song::SongSection*> order;
    if (!s.arrangements.empty()) {
        for (const std::string& id : s.arrangements.front().sectionIds)
            for (const song::SongSection& sec : s.sections)
                if (sec.id == id) { order.push_back(&sec); break; }
    }
    if (order.empty()) for (const song::SongSection& sec : s.sections) order.push_back(&sec);

    for (const song::SongSection* sec : order) {
        ImportedSection section;
        const std::string match = FindGroupMatch(sec->name);
        section.group = match.empty() ? Lower(sec->name) : match;
        section.label = match.empty() ? sec->name : GroupLabel(match);
        std::string text;
        for (size_t i = 0; i < sec->lines.size(); ++i) { if (i) text += "\n"; text += sec->lines[i].lyrics; }
        section.slides.push_back({ text, {}, ChordsJsonOf(*sec), {} });
        show.sections.push_back(std::move(section));
    }
    return show;
}

std::shared_ptr<song::ISongProvider> SongProviderFor(std::string_view formatId) {
    if (formatId == "chordpro") return song::CreateChordProProvider();
    if (formatId == "opensong") return song::CreateOpenSongProvider();
    if (formatId == "openlp") return song::CreateOpenLpProvider();
    if (formatId == "propresenter") return song::CreateProPresenterProvider();
    if (formatId == "easyworship") return song::CreateEasyWorshipProvider();
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// FreeShow's own show file: { name, category, meta, slides: { id: { group, notes, items, children } }, layouts, settings }
// ---------------------------------------------------------------------------------------------------------------------

std::string ItemsText(const J& items) {
    std::string out;
    if (!items.asArray()) return out;
    for (const J& item : *items.asArray()) {
        const J* lines = item.Find("lines");
        if (!lines || !lines->asArray()) continue;   // not a text box (media, timer...)
        for (const J& line : *lines->asArray()) {
            std::string text;
            if (const J* runs = line.Find("text"); runs && runs->asArray())
                for (const J& run : *runs->asArray())
                    if (const J* v = run.Find("value")) text += std::string(v->asString());
            if (!out.empty()) out += "\n";
            out += text;
        }
    }
    return out;
}

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

std::map<std::string, std::string> FreeShowCss(std::string_view style) {
    std::map<std::string, std::string> properties;
    size_t start = 0;
    while (start < style.size()) {
        const size_t end = style.find(';', start);
        const std::string_view declaration = style.substr(
            start, end == std::string_view::npos ? style.size() - start : end - start);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            const auto key = Trim(declaration.substr(0, colon));
            const auto value = Trim(declaration.substr(colon + 1));
            if (!key.empty() && !value.empty())
                properties[std::string(key)] = std::string(value);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return properties;
}

double FreeShowNumber(const std::map<std::string, std::string>& properties,
                      std::string_view key, double fallback) {
    const auto it = properties.find(std::string(key));
    if (it == properties.end()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : value;
}

std::string FreeShowText(const J& item) {
    const J* lines = item.Find("lines");
    if (!lines || !lines->asArray()) return {};
    std::string out;
    for (const J& line : *lines->asArray()) {
        if (!out.empty()) out += '\n';
        if (const J* runs = line.Find("text"); runs && runs->asArray())
            for (const J& run : *runs->asArray())
                if (const J* value = run.Find("value")) out += std::string(value->asString());
    }
    return out;
}

std::vector<presentation::ContentBlock> FreeShowBlocks(const J& items, const ImportFile& file) {
    using namespace presentation;
    std::vector<ContentBlock> blocks;
    if (!items.asArray()) return blocks;

    // FreeShow stores positions in a 1920x1080 canvas; the app's stage uses
    // the same proportions on its 754x428 logical grid.
    constexpr double kFreeShowWidth = 1920.0;
    constexpr double kFreeShowHeight = 1080.0;
    const double scaleX = library::kStageWidth / kFreeShowWidth;
    const double scaleY = library::kStageHeight / kFreeShowHeight;

    int itemNumber = 0;
    for (const J& item : *items.asArray()) {
        const J* lines = item.Find("lines");
        ContentBlock block;
        block.id = std::format("item-{}", ++itemNumber);
        const J* styleValue = item.Find("style");
        const auto geometry = FreeShowCss(styleValue ? styleValue->asString() : std::string_view{});
        block.x = FreeShowNumber(geometry, "left", 0.0) * scaleX;
        block.y = FreeShowNumber(geometry, "top", 0.0) * scaleY;
        block.width = FreeShowNumber(geometry, "width", kFreeShowWidth) * scaleX;
        block.height = FreeShowNumber(geometry, "height", kFreeShowHeight) * scaleY;

        if (!lines || !lines->asArray()) {
            const J* type = item.Find("type");
            const J* src = item.Find("src");
            if (!type || !src || src->type() != J::Type::String
                    || (type->asString() != "media" && type->asString() != "image"
                        && type->asString() != "video"))
                continue;
            const std::string path = ResolveMediaPath(std::string(src->asString()), file.path);
            if (path.empty()) continue;
            const auto mediaKind = media::MediaKindOf(path);
            if (!mediaKind) continue;

            block.kind = type->asString() == "video" || *mediaKind == media::MediaKind::Video
                ? "video" : "image";
            J::Object meta;
            meta["src"] = J::String(path);
            J::Object mediaOptions;
            if (const J* fit = item.Find("fit"); fit && !fit->asString().empty())
                mediaOptions["fit"] = J::String(std::string(fit->asString()));
            if (const J* filter = item.Find("filter"); filter && filter->type() == J::Type::String) {
                const std::string value(filter->asString());
                constexpr std::string_view opacityPrefix = "opacity(";
                const size_t at = value.find(opacityPrefix);
                if (at != std::string::npos) {
                    const char* number = value.c_str() + at + opacityPrefix.size();
                    char* end = nullptr;
                    const double opacity = std::strtod(number, &end);
                    if (end != number)
                        mediaOptions["opacity"] = J::Number(std::clamp(opacity, 0.0, 1.0));
                }
            }
            if (!mediaOptions.empty()) meta["media"] = J(std::move(mediaOptions));
            block.metaJson = J(std::move(meta)).ToString();
            blocks.push_back(std::move(block));
            continue;
        }

        block.kind = "text";
        block.text = FreeShowText(item);

        J::Object meta;
        const J* autoFontSize = item.Find("autoFontSize");
        const double sourceFontSize = autoFontSize && autoFontSize->type() == J::Type::Number
            ? autoFontSize->asNumber() : 54.0;
        meta["fontSize"] = J::Number(sourceFontSize * scaleY);
        meta["color"] = J::String("#f2f4fa");
        meta["align"] = J::String("center");
        meta["autoSize"] = J::String("shrinkToFit");

        // The first run describes the text item's common font in FreeShow's
        // usual case. Preserve its supported font attributes at block level.
        std::string runStyle;
        for (const J& line : *lines->asArray()) {
            const J* runs = line.Find("text");
            if (!runs || !runs->asArray() || runs->asArray()->empty()) continue;
            const J* value = runs->asArray()->front().Find("style");
            if (value) runStyle = std::string(value->asString());
            if (!runStyle.empty()) break;
        }
        const auto textStyle = FreeShowCss(runStyle);
        if (const auto family = textStyle.find("font-family"); family != textStyle.end()) {
            std::string name = family->second;
            if (name.size() >= 2 && ((name.front() == '\'' && name.back() == '\'')
                                     || (name.front() == '"' && name.back() == '"')))
                name = name.substr(1, name.size() - 2);
            meta["fontFamily"] = J::String(std::move(name));
        }
        if (const auto color = textStyle.find("color"); color != textStyle.end())
            meta["color"] = J::String(color->second);
        if (const auto weight = textStyle.find("font-weight"); weight != textStyle.end())
            meta["bold"] = J::Bool(weight->second == "bold" || weight->second == "700");
        if (const auto fontStyle = textStyle.find("font-style"); fontStyle != textStyle.end())
            meta["italic"] = J::Bool(fontStyle->second == "italic");
        if (const auto decoration = textStyle.find("text-decoration"); decoration != textStyle.end())
            meta["underline"] = J::Bool(decoration->second.find("underline") != std::string::npos);

        if (const J* align = item.Find("align"); align && !align->asString().empty())
            meta["align"] = J::String(std::string(align->asString()));
        else {
            for (const J& line : *lines->asArray()) {
                const J* align = line.Find("align");
                if (align && !align->asString().empty()) {
                    meta["align"] = J::String(std::string(align->asString()));
                    break;
                }
            }
        }
        block.metaJson = J(std::move(meta)).ToString();
        blocks.push_back(std::move(block));
    }
    return blocks;
}

void CollectFreeShowMedia(const J& showJson, const ImportFile& file, ImportedShow& out) {
    const J* slides = showJson.Find("slides");
    if (!slides || !slides->asObject()) return;
    auto add = [&](const J& slide) {
        const J* items = slide.Find("items");
        if (!items || !items->asArray()) return;
        for (const J& item : *items->asArray()) {
            const J* type = item.Find("type");
            if (!type || type->type() != J::Type::String) continue;
            const std::string kind = std::string(type->asString());
            if (kind != "media" && kind != "image" && kind != "video") continue;
            const J* src = item.Find("src");
            if (!src || src->type() != J::Type::String) continue;
            const std::string path = ResolveMediaPath(std::string(src->asString()), file.path);
            if (path.empty() || !media::MediaKindOf(path)) continue;
            if (std::find(out.mediaRefs.begin(), out.mediaRefs.end(), path) == out.mediaRefs.end())
                out.mediaRefs.push_back(path);
        }
    };
    for (const auto& [id, slide] : *slides->asObject()) {
        if (!slide.asObject()) continue;
        add(slide);
        // A group's children are their own slides, with their own items.
        if (const J* kids = slide.Find("children"); kids && kids->asArray())
            for (const J& child : *kids->asArray())
                if (const J* cs = slides->Find(std::string(child.asString()))) add(*cs);
    }
}

Result<ImportedShow> ParseFreeShowFile(const ImportFile& file) {
    auto parsed = json::Parse(file.content);
    if (!parsed.ok())
        return Error::Make(Err::ParseError, kModule, std::format("'{}' is not a FreeShow file: {}", file.name, parsed.error().message));
    const J* root = &parsed.value();
    // a show file is either the show itself, or [id, show]
    if (root->asArray() && root->asArray()->size() >= 2) root = &(*root->asArray())[1];
    if (!root->asObject() || !root->Find("slides")) {
        bool onlyReference = root->asObject() != nullptr;
        if (onlyReference)
            for (const auto& [key, value] : *root->asObject())
                if (key != "category" && key != "name" && key != "timestamps") onlyReference = false;
        return Error::Make(Err::ParseError, kModule,
                           onlyReference
                               ? std::format("'{}' holds no show data (an empty reference, nothing to import)", file.name)
                               : std::format("'{}' has no slides", file.name));
    }

    ImportedShow show;
    show.origin = "freeshow";
    show.category = "song";
    if (const J* n = root->Find("name")) show.name = std::string(n->asString());
    if (show.name.empty()) show.name = file.name;
    if (const J* meta = root->Find("meta"); meta && meta->asObject())
        for (const auto& [k, v] : *meta->asObject())
            if (v.type() == J::Type::String && !v.asString().empty()) show.meta[k] = std::string(v.asString());

    const J* slides = root->Find("slides");
    const J* layouts = root->Find("layouts");
    std::string layoutId;
    if (const J* active = root->Find("settings.activeLayout")) layoutId = std::string(active->asString());
    const J* layout = layouts && layouts->asObject() ? (layouts->Find(layoutId) ? layouts->Find(layoutId) : nullptr) : nullptr;
    if (!layout && layouts && layouts->asObject() && !layouts->asObject()->empty()) layout = &layouts->asObject()->begin()->second;
    if (layout)
        if (const J* n = layout->Find("notes")) show.notes = std::string(n->asString());

    auto isMeaninglessGroup = [](const std::string& g) {
        if (g.empty()) return true;
        return std::all_of(g.begin(), g.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };

    auto slideOf = [&](const std::string& id) -> const J* { return slides->asObject() ? slides->Find(id) : nullptr; };
    auto textOf = [&](const J& s) {
        ImportedSlide out;
        if (const J* items = s.Find("items")) {
            out.text = ItemsText(*items);
            out.blocks = FreeShowBlocks(*items, file);
        }
        if (const J* notes = s.Find("notes")) out.notes = std::string(notes->asString());
        return out;
    };

    struct RawSection { std::string id; std::string group; std::vector<ImportedSlide> slides; bool needsAutoGroup = false; };
    std::vector<RawSection> raw;
    auto collect = [&](const std::string& id) {
        const J* slide = slideOf(id);
        if (!slide || !slide->asObject()) return;
        RawSection r;
        r.id = id;
        if (const J* g = slide->Find("group"); g && g->type() == J::Type::String) r.group = std::string(g->asString());
        r.needsAutoGroup = isMeaninglessGroup(r.group);
        r.slides.push_back(textOf(*slide));
        if (const J* kids = slide->Find("children"); kids && kids->asArray())
            for (const J& child : *kids->asArray())
                if (const J* cs = slideOf(std::string(child.asString()))) r.slides.push_back(textOf(*cs));
        raw.push_back(std::move(r));
    };

    if (layout && layout->Find("slides") && layout->Find("slides")->asArray()) {
        for (const J& entry : *layout->Find("slides")->asArray()) {
            const J* disabled = entry.Find("disabled");
            if (disabled && disabled->asBool()) continue;
            if (const J* id = entry.Find("id")) collect(std::string(id->asString()));
        }
    } else if (slides->asObject()) {
        for (const auto& [id, s] : *slides->asObject()) {
            const J* g = s.Find("group");
            if (g && g->type() == J::Type::String) collect(id);   // children (group null) hang from their parents
        }
    }

    std::vector<std::string> guessInput;
    std::vector<size_t> guessTargets;
    for (size_t i = 0; i < raw.size(); ++i)
        if (raw[i].needsAutoGroup) {
            guessTargets.push_back(i);
            guessInput.push_back(raw[i].slides.empty() ? std::string() : raw[i].slides.front().text);
        }
    const std::vector<std::string> guessed = guessInput.empty() ? std::vector<std::string>() : AutoGroupSections(guessInput);
    for (size_t k = 0; k < guessTargets.size(); ++k)
        raw[guessTargets[k]].group = k < guessed.size() ? guessed[k] : "verse";

    // Pass 3: build the real sections with their final group + display label.
    for (RawSection& r : raw) {
        ImportedSection section;
        const std::string match = FindGroupMatch(r.group);
        section.group = match.empty() ? (r.group.empty() ? "verse" : Lower(r.group)) : match;
        section.label = match.empty() ? (r.group.empty() ? "Verse" : r.group) : GroupLabel(match);
        section.slides = std::move(r.slides);
        show.sections.push_back(std::move(section));
    }
    CollectFreeShowMedia(*root, file, show);
    return show;
}

bool IsEmptyFreeShowReference(const Error& error) {
    return error.message.find("holds no show data (an empty reference, nothing to import)")
        != std::string::npos;
}

std::string BibleFormatOf(std::string_view formatId) {
    if (formatId == "bible_xml") return "xml";
    if (formatId == "bible_osis") return "osis";
    if (formatId == "bible_usfm") return "usfm";
    if (formatId == "bible_json") return "json";
    return "txt";
}
} // namespace

const char* ToString(ImportKind kind) {
    switch (kind) {
        case ImportKind::Show: return "show";
        case ImportKind::Bible: return "bible";
        case ImportKind::Project: return "project";
        case ImportKind::Template: return "template";
        case ImportKind::Calendar: return "calendar";
        case ImportKind::Other: return "other";
    }
    return "show";
}

const std::vector<ImportFormat>& ImportFormats() {
    static const std::vector<ImportFormat> formats = BuildFormats();
    return formats;
}

const ImportFormat* FindImportFormat(std::string_view id) {
    for (const ImportFormat& f : ImportFormats())
        if (f.id == id) return &f;
    return nullptr;
}

std::vector<const ImportFormat*> FormatsForExtension(std::string_view extension) {
    std::vector<const ImportFormat*> out;
    const std::string ext = Lower(std::string(extension));
    for (const ImportFormat& f : ImportFormats())
        if (std::find(f.extensions.begin(), f.extensions.end(), ext) != f.extensions.end()) out.push_back(&f);
    return out;
}

Result<ImportResult> ImportFiles(std::string_view formatId, const std::vector<ImportFile>& files, const ImportOptions& options) {
    const ImportFormat* format = FindImportFormat(formatId);
    if (!format) return Error::Make(Err::NotFound, kModule, std::format("there is no import format '{}'", formatId));
    if (!format->available)
        return Error::Make(Err::Unsupported, kModule, std::format("importing {} files is not available yet", format->name));

    ImportResult result;
    result.files = files.size();
    ResetFolderCache();
    for (const ImportFile& file : files) {
        auto fail = [&](const std::string& why) { result.warnings.push_back(std::format("{}: {}", file.name, why)); };

        if (format->kind == ImportKind::Bible) {
            auto id = bible::BibleEngine::Instance().Import(file.content, BibleFormatOf(formatId), {});
            if (id.ok()) result.bibles.push_back(id.value()); else fail(id.error().message);
        } else if (formatId == "txt") {
            SongTextOptions o;
            o.name = file.name;
            o.noFormatting = options.noFormatting;
            o.splitLines = options.splitLines;
            ImportedShow show = ParseSongText(file.content, o);
            if (show.sections.empty()) fail("no lyrics found"); else result.shows.push_back(std::move(show));
        } else if (formatId == "csv") {
            ImportedShow show = ParseCsv(file);
            if (show.sections.empty()) fail("no lines found"); else result.shows.push_back(std::move(show));
        } else if (formatId == "freeshow") {
            auto show = ParseFreeShowFile(file);
            if (show.ok()) result.shows.push_back(std::move(show.value()));
            else if (!IsEmptyFreeShowReference(show.error())) fail(show.error().message);
        } else if (formatId == "freeshow_project") {
            if (LooksLikeZip(file.content)) {
                auto zip = ReadZip(file.content);
                if (!zip.ok()) { fail(zip.error().message); continue; }
                size_t members = 0;
                for (ArchiveEntry& entry : zip.value()) {
                    if (entry.name.ends_with("/")) continue;
                    ++members;
                    ImportFile member;
                    member.name = entry.name.substr(entry.name.find_last_of("/") + 1);
                    const size_t dot = member.name.find_last_of('.');
                    member.extension = dot == std::string::npos ? std::string() : Lower(member.name.substr(dot + 1));
                    member.path = file.path + "::" + entry.name;
                    member.content = std::move(entry.data);
                    auto show = ParseFreeShowFile(member);
                    if (show.ok()) result.shows.push_back(std::move(show.value()));
                    else result.warnings.push_back(std::format("{}: {}", entry.name, show.error().message));
                }
                if (members == 0) fail("the project archive is empty");
            } else {
                // A project saved as one JSON document: an ARRAY of [id, show].
                auto parsed = json::Parse(file.content);
                if (!parsed.ok()) { fail(std::format("'{}' is not a FreeShow project: {}", file.name, parsed.error().message)); continue; }
                const J* root = &parsed.value();
                if (!root->asArray()) { fail("the project is not an array of shows"); continue; }
                size_t members = 0;
                for (const J& entry : *root->asArray()) {
                    const J* showNode = entry.asArray() && entry.asArray()->size() >= 2 ? &(*entry.asArray())[1] : &entry;
                    ImportFile member;
                    member.name = file.name + "-" + std::to_string(members + 1);
                    member.extension = "show";
                    member.content = showNode->ToString();
                    auto show = ParseFreeShowFile(member);
                    if (show.ok()) { ++members; result.shows.push_back(std::move(show.value())); }
                    else result.warnings.push_back(std::format("{}[{}]: {}", file.name, members + 1, show.error().message));
                }
                if (members == 0 && result.warnings.empty()) fail("the project holds no shows");
            }
        } else if (auto provider = SongProviderFor(formatId)) {
            if (file.extension == "probundle") {
                auto zip = ReadZip(file.content);
                if (!zip.ok()) { fail(zip.error().message); continue; }
                size_t members = 0;
                for (ArchiveEntry& entry : zip.value()) {
                    if (entry.name.ends_with("/")) continue;
                    if (!entry.data.starts_with("<")) continue;   // metadata/assets live in there too
                    ++members;
                    ImportFile member;
                    member.name = entry.name.substr(entry.name.find_last_of("/") + 1);
                    const size_t dot = member.name.find_last_of('.');
                    member.extension = dot == std::string::npos ? std::string() : Lower(member.name.substr(dot + 1));
                    member.path = file.path + "::" + entry.name;
                    auto song = provider->Parse(entry.data, provider->Format());
                    if (song.ok()) result.shows.push_back(SongToImported(song.value(), member, "propresenter"));
                    else result.warnings.push_back(std::format("{}: {}", entry.name, song.error().message));
                }
                if (members == 0) fail("the bundle holds no readable shows");
                continue;
            }
            auto song = provider->Parse(file.content, provider->Format());
            if (song.ok()) result.shows.push_back(SongToImported(song.value(), file, std::string(formatId)));
            else fail(song.error().message);
        } else {
            fail("no converter");
        }
    }
    return result;
}

ImportedShow ShowFromClipboardText(const std::string& text, const ImportOptions& options) {
    SongTextOptions o;
    o.noFormatting = options.noFormatting;
    o.splitLines = options.splitLines;
    ImportedShow show = ParseSongText(text, o);
    show.origin = "clipboard";
    return show;
}

// ---------------------------------------------------------------------------------------------------------------------
// ImportedShow -> Presentation
// ---------------------------------------------------------------------------------------------------------------------

presentation::Presentation ShowFromImported(const ImportedShow& imported, const ShowBuildOptions& options) {
    using namespace presentation;
    Presentation p;
    p.name = imported.name;

    // the show's own details
    J::Object meta;
    for (const auto& [k, v] : imported.meta) meta[k] = J::String(v);
    if (!imported.notes.empty()) meta["notes"] = J::String(imported.notes);
    if (!imported.origin.empty()) meta["origin"] = J::String(imported.origin);
    p.metaJson = J(std::move(meta)).ToString();

    Category category;
    category.id = "cat-" + imported.category;
    category.name = options.categoryName;
    category.contentType = imported.category;
    p.categories.push_back(category);

    // "Verse 1", "Verse 2": every different text of a group is the next number; the same text again is the same slide
    std::map<std::string, int> perGroup;
    std::map<std::string, int> numbered;   // group|text -> its number
    auto labelFor = [&](const ImportedSection& s) {
        const std::string key = s.group + "|" + (s.slides.empty() ? std::string() : s.slides.front().text);
        auto it = numbered.find(key);
        if (it == numbered.end()) it = numbered.emplace(key, ++perGroup[s.group]).first;
        return it->second;
    };
    std::map<std::string, int> groupTotals;
    {
        std::map<std::string, std::string> firstSeen;
        for (const ImportedSection& s : imported.sections) {
            const std::string key = s.group + "|" + (s.slides.empty() ? std::string() : s.slides.front().text);
            if (firstSeen.emplace(key, key).second) ++groupTotals[s.group];
        }
    }

    int slideNumber = 0;
    for (const ImportedSection& section : imported.sections) {
        const int number = labelFor(section);
        const std::string label = groupTotals[section.group] > 1 ? std::format("{} {}", section.label, number) : section.label;
        for (size_t si = 0; si < section.slides.size(); ++si) {
            const ImportedSlide& src = section.slides[si];
            Slide slide;
            slide.id = std::format("slide-{}", ++slideNumber);
            slide.title = si == 0 ? label : std::format("{} ({})", label, si + 1);
            slide.text = src.text;
            slide.notes = src.notes;
            slide.tags = { section.group };
            slide.background = options.background;
            slide.categoryId = category.id;

            J::Object slideMeta;
            slideMeta["group"] = J::String(section.group);
            if (!src.chordsJson.empty())
                if (auto chords = json::Parse(src.chordsJson); chords.ok()) slideMeta["chords"] = chords.value();
            slide.metaJson = J(std::move(slideMeta)).ToString();

            int blockNumber = 0;
            if (!src.blocks.empty()) {
                slide.blocks = src.blocks;
            } else if (options.templateBlocks.empty()) {
                ContentBlock b;
                b.id = "item-1";
                b.kind = "text";
                b.text = src.text;
                b.x = 20; b.y = 30; b.width = 714; b.height = 368;
                b.bind = "text";
                b.metaJson = R"({"fontSize":40,"align":"center","color":"#ffffff","autoSize":"shrinkToFit"})";
                slide.blocks.push_back(std::move(b));
            } else {
                for (ContentBlock b : options.templateBlocks) {
                    b.id = std::format("item-{}", ++blockNumber);
                    if (b.kind == "text") {
                        if (b.bind == "text") b.text = src.text;
                        else if (b.bind == "title") b.text = label;
                        else if (b.bind == "notes") b.text = src.notes;
                    }
                    slide.blocks.push_back(std::move(b));
                }
            }
            p.slides.push_back(std::move(slide));
        }
    }
    return p;
}
} // namespace bps::import