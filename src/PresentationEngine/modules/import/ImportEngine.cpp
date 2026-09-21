#include "modules/import/ImportEngine.hpp"

#include "core/config/Json.hpp"
#include "modules/bible/BibleEngine.hpp"
#include "modules/import/SongText.hpp"
#include "modules/songs/ISongProvider.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

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
    f.push_back(Fmt("freeshow_project", "FreeShow Project File", { "project", "shows", "json", "zip" }, K::Project, "freeshow", false, "folder", true));
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
        section.slides.push_back({ body, {}, {} });
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

ImportedShow SongToImported(const song::Song& s, const ImportFile& file, const std::string& origin) {
    ImportedShow show;
    show.origin = origin;
    show.category = "song";
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
        section.slides.push_back({ text, {}, ChordsJsonOf(*sec) });
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

Result<ImportedShow> ParseFreeShowFile(const ImportFile& file) {
    auto parsed = json::Parse(file.content);
    if (!parsed.ok())
        return Error::Make(Err::ParseError, kModule, std::format("'{}' is not a FreeShow file: {}", file.name, parsed.error().message));
    const J* root = &parsed.value();
    // a show file is either the show itself, or [id, show]
    if (root->asArray() && root->asArray()->size() >= 2) root = &(*root->asArray())[1];
    if (!root->asObject() || !root->Find("slides"))
        return Error::Make(Err::ParseError, kModule, std::format("'{}' has no slides", file.name));

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

    auto slideOf = [&](const std::string& id) -> const J* { return slides->asObject() ? slides->Find(id) : nullptr; };
    auto addSection = [&](const std::string& id) {
        const J* slide = slideOf(id);
        if (!slide || !slide->asObject()) return;
        ImportedSection section;
        std::string group;
        if (const J* g = slide->Find("group"); g && g->type() == J::Type::String) group = std::string(g->asString());
        const std::string match = FindGroupMatch(group);
        section.group = match.empty() ? (group.empty() ? "verse" : Lower(group)) : match;
        section.label = match.empty() ? (group.empty() ? "Verse" : group) : GroupLabel(match);
        auto text = [&](const J& s) {
            ImportedSlide out;
            if (const J* items = s.Find("items")) out.text = ItemsText(*items);
            if (const J* notes = s.Find("notes")) out.notes = std::string(notes->asString());
            return out;
        };
        section.slides.push_back(text(*slide));
        if (const J* kids = slide->Find("children"); kids && kids->asArray())
            for (const J& child : *kids->asArray())
                if (const J* cs = slideOf(std::string(child.asString()))) section.slides.push_back(text(*cs));
        show.sections.push_back(std::move(section));
    };

    if (layout && layout->Find("slides") && layout->Find("slides")->asArray()) {
        for (const J& entry : *layout->Find("slides")->asArray()) {
            const J* disabled = entry.Find("disabled");
            if (disabled && disabled->asBool()) continue;
            if (const J* id = entry.Find("id")) addSection(std::string(id->asString()));
        }
    } else if (slides->asObject()) {
        for (const auto& [id, s] : *slides->asObject()) {
            const J* g = s.Find("group");
            if (g && g->type() == J::Type::String) addSection(id);   // children (group null) hang from their parents
        }
    }
    return show;
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
            if (show.ok()) result.shows.push_back(std::move(show.value())); else fail(show.error().message);
        } else if (auto provider = SongProviderFor(formatId)) {
            if (file.extension == "sqlite" || file.extension == "probundle") { fail(std::format("'.{}' files are not readable yet", file.extension)); continue; }
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

            // the words on the slide: the template's blocks with the text filled in
            int blockNumber = 0;
            if (options.templateBlocks.empty()) {
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
