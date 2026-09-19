#include "modules/songs/ISongProvider.hpp"

#include "core/config/Json.hpp"
#include "modules/songs/Chord.hpp"
#include "modules/xml/Xml.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <format>

namespace bps::song {

namespace {

using XmlNode = xml::Node;

std::string Trim(std::string_view s) { return xml::Trim(s); }
std::string Collapse(std::string_view s) { return xml::Collapse(s); }

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string Lower(std::string_view s) {
    std::string out;
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Generates a stable-ish section id from a section name + index.
std::string SectionId(std::string_view name, size_t index) {
    std::string base;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) base.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!base.empty() && base.back() != '-') base.push_back('-');
    }
    if (base.empty()) base = "section";
    return std::format("{}-{}", base, index + 1);
}

// Normalizes a section name into an uppercase label ("verse 1" -> "VERSE 1").
std::string SectionLabel(std::string_view name) {
    std::string out;
    for (char c : name)
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

// Parses a line that consists only of chord tokens. Returns false when any
// token is not a chord (e.g. it is a lyric line).
bool TryChordLine(std::string_view line, std::vector<ChordRef>& out) {
    std::vector<ChordRef> refs;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        std::string token(line.substr(start, i - start));
        if (!ChordSystem::IsChordToken(token)) return false;
        ChordRef cr;
        cr.column = start;
        if (auto r = ChordSystem::Parse(token); r.ok()) cr.chord = r.value();
        refs.push_back(std::move(cr));
    }
    out = std::move(refs);
    return true;
}

// Extracts inline [C] bracket chords from a lyric line into chord refs.
void ExtractInlineChords(std::string& lyrics, std::vector<ChordRef>& refs) {
    size_t i = 0;
    std::string clean;
    while (i < lyrics.size()) {
        if (lyrics[i] == '[') {
            size_t close = lyrics.find(']', i);
            if (close != std::string::npos) {
                std::string token = lyrics.substr(i + 1, close - i - 1);
                if (auto r = ChordSystem::Parse(token); r.ok()) {
                    ChordRef cr;
                    cr.column = clean.size();
                    cr.chord = r.value();
                    refs.push_back(std::move(cr));
                    i = close + 1;
                    continue;
                }
            }
        }
        clean.push_back(lyrics[i++]);
    }
    lyrics = std::move(clean);
}

// Parses ChordPro-style directive lines: {title: X}, {key: C}, ...
bool TryDirective(std::string_view line, std::string& key, std::string& value) {
    std::string t = Trim(line);
    if (t.size() < 4 || t.front() != '{' || t.back() != '}') return false;
    size_t colon = t.find(':');
    if (colon == std::string::npos) return false;
    key = Lower(Trim(t.substr(1, colon - 1)));
    value = Trim(t.substr(colon + 1, t.size() - colon - 2));
    return true;
}

// Pending chord lines shared by the XML lyric walkers (a line of chords above
// a lyric line is remembered and merged into the next lyric line).
struct PendingChords {
    std::vector<std::vector<ChordRef>> stack;
    void Clear() { stack.clear(); }
};

void ParseXmlLyricsWithState(const XmlNode& song, Song& out, PendingChords& pc) {
    // Verses live under <lyrics> in OpenSong/OpenLP/ProPresenter; tolerate a
    // direct placement as well.
    std::vector<const XmlNode*> verses = song.Children("verse");
    if (verses.empty()) {
        if (const auto* lyrics = song.FindDescendant("lyrics"))
            verses = lyrics->Children("verse");
    }
    size_t idx = 0;
    for (const auto* verse : verses) {
        SongSection sec;
        std::string name = verse->Attr("name");
        if (name.empty()) name = "verse";
        sec.id = SectionId(name, idx);
        sec.name = SectionLabel(name);
        for (const auto* lines : verse->Children("lines")) {
            std::istringstream ls{lines->text};
            std::string line;
            while (std::getline(ls, line)) {
                std::string text = Trim(line);
                if (text.empty()) continue;
                SongLine sl;
                std::vector<ChordRef> chordLine;
                if (TryChordLine(text, chordLine)) {
                    pc.stack.push_back(chordLine);
                    continue;
                }
                ExtractInlineChords(text, sl.chords);
                if (!pc.stack.empty()) {
                    for (const auto& ch : pc.stack.front()) sl.chords.push_back(ch);
                    pc.stack.clear();
                }
                sl.lyrics = std::move(text);
                sec.lines.push_back(std::move(sl));
            }
        }
        if (!sec.lines.empty()) {
            out.sections.push_back(std::move(sec));
            ++idx;
        }
    }
}

// ---------------------------------------------------------------------------
// ChordPro
// ---------------------------------------------------------------------------
class ChordProProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "chordpro"; }
    const char* Format() const noexcept override { return "chordpro"; }
    std::vector<std::string> SupportedExtensions() const override {
        return {".cho", ".chopro", ".chordpro", ".crd", ".txt"};
    }

    Result<Song> Parse(std::string_view source, std::string_view) const override {
        Song song;
        song.id = "chordpro-import";
        std::istringstream lines{std::string(source)};
        std::string line;
        SongSection* cur = nullptr;
        std::vector<ChordRef> pendingChordLine;
        size_t sectionIdx = 0;

        auto ensureSection = [&]() {
            if (cur) return;
            song.sections.push_back(SongSection{});
            song.sections.back().id = SectionId("verse", sectionIdx);
            song.sections.back().name = "VERSE";
            cur = &song.sections.back();
            ++sectionIdx;
        };

        while (std::getline(lines, line)) {
            std::string text = Trim(line);
            if (text.empty()) {
                pendingChordLine.clear();
                continue;
            }
            std::string key, value;
            if (TryDirective(text, key, value)) {
                if (key == "title" || key == "t") {
                    if (song.metadata.title.empty()) song.metadata.title = value;
                } else if (key == "key") {
                    song.metadata.originalKey = value;
                    if (song.metadata.performanceKey.empty()) song.metadata.performanceKey = value;
                } else if (key == "author") {
                    song.metadata.authors.push_back(value);
                } else if (key == "ccli") {
                    song.metadata.ccli = value;
                    song.metadata.licensing.ccli = value;
                } else if (key == "copyright") {
                    song.metadata.copyright = value;
                    song.metadata.licensing.copyrightHolder = value;
                } else if (key == "tempo") {
                    song.metadata.tempo = value;
                } else if (key == "time") {
                    song.metadata.timeSignature = value;
                } else if (key == "language") {
                    song.metadata.language = value;
                } else if (key == "comment" || key == "notes") {
                    if (!song.metadata.notes.empty()) song.metadata.notes += " ";
                    song.metadata.notes += value;
                }
                continue;
            }
            // Section marker: "[Verse 1]", "[Chorus]" (not a bracket chord).
            if (text.size() >= 2 && text.front() == '[' && text.back() == ']' &&
                text.find(']') == text.size() - 1) {
                std::string name = Trim(text.substr(1, text.size() - 2));
                if (!ChordSystem::IsChordToken(name)) {
                    song.sections.push_back(SongSection{});
                    song.sections.back().id = SectionId(name, sectionIdx);
                    song.sections.back().name = SectionLabel(name);
                    cur = &song.sections.back();
                    ++sectionIdx;
                    pendingChordLine.clear();
                    continue;
                }
            }
            ensureSection();
            std::vector<ChordRef> chordLine;
            if (TryChordLine(text, chordLine)) {
                pendingChordLine = chordLine;
                continue;
            }
            SongLine sl;
            ExtractInlineChords(text, sl.chords);
            if (!pendingChordLine.empty()) {
                for (const auto& ch : pendingChordLine) sl.chords.push_back(ch);
                pendingChordLine.clear();
            }
            sl.lyrics = std::move(text);
            cur->lines.push_back(std::move(sl));
        }
        if (song.sections.empty())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "ChordPro contains no sections");
        song.metadata.licensing.source = "chordpro";
        song.createdMs = song.modifiedMs = NowMs();
        return song;
    }
};

// ---------------------------------------------------------------------------
// Shared XML metadata reader (OpenSong / OpenLP / ProPresenter / EasyWorship).
// ---------------------------------------------------------------------------
void ReadXmlMetadata(const XmlNode& root, SongMetadata& meta) {
    auto set = [&](const char* tag, std::string& out) {
        const XmlNode* n = root.FindDescendant(tag);
        if (n && out.empty()) out = Collapse(Trim(n->text));
    };
    set("title", meta.title);
    set("ccli", meta.ccli);
    set("key", meta.originalKey);
    set("copyright", meta.copyright);
    set("tempo", meta.tempo);
    set("timeSignature", meta.timeSignature);
    set("language", meta.language);
    set("notes", meta.notes);
    for (const auto* a : root.FindDescendant("authors")
                             ? root.FindDescendant("authors")->Children("author")
                             : std::vector<const XmlNode*>{})
        meta.authors.push_back(Collapse(Trim(a->text)));
    if (meta.authors.empty())
        for (const auto* a : root.Children("author"))
            meta.authors.push_back(Collapse(Trim(a->text)));
    if (meta.performanceKey.empty()) meta.performanceKey = meta.originalKey;
}

Result<Song> ParseOpenSongLike(std::string_view source) {
    auto parsed = xml::Parse(source);
    if (!parsed.ok()) return parsed.error();
    const XmlNode& root = parsed.value();
    if (root.tag != "song" && root.tag != "rvb")
        return Error::Make(Err::Song_UnsupportedFormat, "SongProvider",
                           "root element is not <song>/<rvb>");
    const XmlNode* songNode = &root;
    if (const auto* song = root.FindDescendant("song")) songNode = song;

    Song song;
    ReadXmlMetadata(*songNode, song.metadata);
    PendingChords pc;
    ParseXmlLyricsWithState(*songNode, song, pc);
    if (song.sections.empty())
        return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                           "no lyrics found");
    song.metadata.licensing.source = "xml";
    song.createdMs = song.modifiedMs = NowMs();
    return song;
}

class OpenSongProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "opensong"; }
    const char* Format() const noexcept override { return "opensong"; }
    std::vector<std::string> SupportedExtensions() const override { return {".opn", ".xml"}; }
    Result<Song> Parse(std::string_view source, std::string_view) const override {
        return ParseOpenSongLike(source);
    }
};

class OpenLpProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "openlp"; }
    const char* Format() const noexcept override { return "openlp"; }
    std::vector<std::string> SupportedExtensions() const override { return {".xml"}; }
    Result<Song> Parse(std::string_view source, std::string_view) const override {
        return ParseOpenSongLike(source);
    }
};

class ProPresenterProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "propresenter"; }
    const char* Format() const noexcept override { return "propresenter"; }
    std::vector<std::string> SupportedExtensions() const override { return {".pro", ".xml", ".json"}; }
    Result<Song> Parse(std::string_view source, std::string_view) const override {
        // ProPresenter 6/7 XML is OpenSong-shaped at the <song> level.
        if (Lower(source.substr(0, 64)).contains("<rvb") ||
            Lower(source.substr(0, 64)).contains("<song"))
            return ParseOpenSongLike(source);
        return Error::Make(Err::Song_UnsupportedFormat, "SongProvider",
                           "unsupported ProPresenter payload");
    }
};

// ---------------------------------------------------------------------------
// EasyWorship — <song><title> + repeated <stanza>/<verse> blocks of <line>s.
// ---------------------------------------------------------------------------
class EasyWorshipProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "easyworship"; }
    const char* Format() const noexcept override { return "easyworship"; }
    std::vector<std::string> SupportedExtensions() const override { return {".xml", ".sng"}; }

    Result<Song> Parse(std::string_view source, std::string_view) const override {
        auto parsed = xml::Parse(source);
        if (!parsed.ok()) return parsed.error();
        const XmlNode& root = parsed.value();
        const XmlNode* songNode = &root;
        if (const auto* s = root.FindDescendant("song")) songNode = s;

        Song song;
        ReadXmlMetadata(*songNode, song.metadata);
        size_t idx = 0;
        PendingChords pc;
        for (const auto* block : songNode->Children("stanza")) {
            SongSection sec;
            std::string name = block->Attr("name");
            if (name.empty()) name = "verse";
            sec.id = SectionId(name, idx);
            sec.name = SectionLabel(name);
            for (const auto& child : block->children) {
                if (child.tag != "line" && child.tag != "row") continue;
                std::string text = Trim(child.text);
                if (text.empty()) continue;
                SongLine sl;
                std::vector<ChordRef> chordLine;
                if (TryChordLine(text, chordLine)) {
                    pc.stack.push_back(chordLine);
                    continue;
                }
                ExtractInlineChords(text, sl.chords);
                if (!pc.stack.empty()) {
                    for (const auto& ch : pc.stack.front()) sl.chords.push_back(ch);
                    pc.stack.clear();
                }
                sl.lyrics = std::move(text);
                sec.lines.push_back(std::move(sl));
            }
            if (!sec.lines.empty()) {
                song.sections.push_back(std::move(sec));
                ++idx;
            }
        }
        // Fall back: any <verse> blocks (some EasyWorship exports differ).
        if (song.sections.empty()) {
            PendingChords pc2;
            ParseXmlLyricsWithState(*songNode, song, pc2);
        }
        if (song.sections.empty())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "no lyrics found");
        song.metadata.licensing.source = "easyworship";
        song.createdMs = song.modifiedMs = NowMs();
        return song;
    }
};

// ---------------------------------------------------------------------------
// Plain text — "[Verse 1]" / "VERSE 1:" headers, blank-line section breaks.
// ---------------------------------------------------------------------------
class PlainTextSongProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "plain-text"; }
    const char* Format() const noexcept override { return "txt"; }
    std::vector<std::string> SupportedExtensions() const override { return {".txt"}; }

    Result<Song> Parse(std::string_view source, std::string_view) const override {
        Song song;
        std::istringstream lines{std::string(source)};
        std::string line;
        std::vector<ChordRef> pendingChordLine;
        size_t sectionIdx = 0;
    std::string pendingName = "VERSE";

    auto flushSection = [&](SongSection& sec) {
            if (!sec.lines.empty()) {
                song.sections.push_back(std::move(sec));
                ++sectionIdx;
            }
        };

        SongSection cur;
        cur.id = SectionId("verse", 0);
        cur.name = "VERSE";
        bool sawHeader = false;   // title = first lyric line before any header
        while (std::getline(lines, line)) {
            std::string text = Trim(line);
            if (text.empty()) {
                pendingChordLine.clear();
                continue;
            }
            // Section header: "[Verse 1]", "VERSE 1:", "CHORUS:"
            std::string upper = SectionLabel(text);
            bool isHeader = false;
            if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
                std::string inner = Trim(text.substr(1, text.size() - 2));
                if (!ChordSystem::IsChordToken(inner)) {
                    pendingName = SectionLabel(inner);
                    isHeader = true;
                }
            } else if (upper.size() >= 4 && upper.back() == ':') {
                const std::string label = upper.substr(0, upper.size() - 1);
                const bool known = label.contains("VERSE") || label.contains("CHORUS") ||
                                   label.contains("BRIDGE") || label.contains("INTRO") ||
                                   label.contains("OUTRO") || label.contains("TAG") ||
                                   label.contains("PRECHORUS");
                if (known) {
                    pendingName = label;
                    isHeader = true;
                }
            }
            if (isHeader) {
                sawHeader = true;
                flushSection(cur);
                cur = SongSection{};
                cur.id = SectionId(pendingName, sectionIdx);
                cur.name = pendingName;
                continue;
            }
            // The first lyric line of a plain-text file is conventionally the
            // title (when the file does not start with a section header).
            if (!sawHeader && song.metadata.title.empty()) {
                song.metadata.title = text;
                continue;
            }
            std::vector<ChordRef> chordLine;
            if (TryChordLine(text, chordLine)) {
                pendingChordLine = chordLine;
                continue;
            }
            SongLine sl;
            ExtractInlineChords(text, sl.chords);
            if (!pendingChordLine.empty()) {
                for (const auto& ch : pendingChordLine) sl.chords.push_back(ch);
                pendingChordLine.clear();
            }
            sl.lyrics = std::move(text);
            cur.lines.push_back(std::move(sl));
        }
        flushSection(cur);
        if (song.sections.empty())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "no lyrics found");
        song.metadata.licensing.source = "text";
        song.createdMs = song.modifiedMs = NowMs();
        return song;
    }
};

// ---------------------------------------------------------------------------
// JSON — the canonical Song shape.
// ---------------------------------------------------------------------------
class JsonSongProvider final : public ISongProvider {
public:
    const char* Name() const noexcept override { return "song-json"; }
    const char* Format() const noexcept override { return "json"; }
    std::vector<std::string> SupportedExtensions() const override { return {".json"}; }

    Result<Song> Parse(std::string_view source, std::string_view) const override {
        auto parsed = json::Parse(std::string(source));
        if (!parsed.ok())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "invalid JSON: " + parsed.error().message);
        const json::Value& root = parsed.value();
        Song song;
        if (const auto* meta = root.Find("metadata")) {
            song.metadata.title =
                std::string(meta->Find("title") ? meta->Find("title")->asString() : "");
            song.metadata.ccli =
                std::string(meta->Find("ccli") ? meta->Find("ccli")->asString() : "");
            song.metadata.language =
                std::string(meta->Find("language") ? meta->Find("language")->asString() : "");
            song.metadata.originalKey =
                std::string(meta->Find("originalKey") ? meta->Find("originalKey")->asString() : "");
            song.metadata.preferredKey =
                std::string(meta->Find("preferredKey") ? meta->Find("preferredKey")->asString() : "");
            song.metadata.performanceKey =
                std::string(meta->Find("performanceKey") ? meta->Find("performanceKey")->asString() : "");
            song.metadata.tempo =
                std::string(meta->Find("tempo") ? meta->Find("tempo")->asString() : "");
            song.metadata.timeSignature =
                std::string(meta->Find("timeSignature") ? meta->Find("timeSignature")->asString() : "");
            song.metadata.copyright =
                std::string(meta->Find("copyright") ? meta->Find("copyright")->asString() : "");
            song.metadata.notes =
                std::string(meta->Find("notes") ? meta->Find("notes")->asString() : "");
            if (const auto* a = meta->Find("authors"))
                if (const auto* arr = a->asArray())
                    for (const auto& au : *arr)
                        song.metadata.authors.push_back(std::string(au.asString()));
            if (const auto* t = meta->Find("tags"))
                if (const auto* arr = t->asArray())
                    for (const auto& tg : *arr)
                        song.metadata.tags.push_back(std::string(tg.asString()));
        }
        const auto* sections = root.Find("sections");
        if (!sections || !sections->asArray())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "JSON has no \"sections\" array");
        for (const auto& sv : *sections->asArray()) {
            SongSection sec;
            sec.id = std::string(sv.Find("id") ? sv.Find("id")->asString() : "");
            sec.name = std::string(sv.Find("name") ? sv.Find("name")->asString() : "VERSE");
            if (const auto* ls = sv.Find("lines")) {
                if (const auto* arr = ls->asArray())
                    for (const auto& lv : *arr) {
                        SongLine sl;
                        sl.lyrics = std::string(lv.Find("lyrics") ? lv.Find("lyrics")->asString() : "");
                        if (const auto* cs = lv.Find("chords")) {
                            if (const auto* carr = cs->asArray())
                                for (const auto& cv : *carr) {
                                    ChordRef cr;
                                    cr.column = static_cast<size_t>(
                                        cv.Find("column") ? cv.Find("column")->asInt() : 0);
                                    std::string token = std::string(
                                        cv.Find("chord") ? cv.Find("chord")->asString() : "");
                                    if (auto r = ChordSystem::Parse(token); r.ok())
                                        cr.chord = r.value();
                                    sl.chords.push_back(std::move(cr));
                                }
                        }
                        if (!sl.lyrics.empty()) sec.lines.push_back(std::move(sl));
                    }
            }
            if (!sec.lines.empty()) song.sections.push_back(std::move(sec));
        }
        if (song.sections.empty())
            return Error::Make(Err::Song_ValidationFailed, "SongProvider",
                               "no sections found");
        song.metadata.licensing.source = "json";
        song.createdMs = song.modifiedMs = NowMs();
        return song;
    }
};

} // namespace

std::shared_ptr<ISongProvider> CreateChordProProvider() {
    return std::make_shared<ChordProProvider>();
}
std::shared_ptr<ISongProvider> CreateOpenSongProvider() {
    return std::make_shared<OpenSongProvider>();
}
std::shared_ptr<ISongProvider> CreateOpenLpProvider() {
    return std::make_shared<OpenLpProvider>();
}
std::shared_ptr<ISongProvider> CreateProPresenterProvider() {
    return std::make_shared<ProPresenterProvider>();
}
std::shared_ptr<ISongProvider> CreateEasyWorshipProvider() {
    return std::make_shared<EasyWorshipProvider>();
}
std::shared_ptr<ISongProvider> CreatePlainTextSongProvider() {
    return std::make_shared<PlainTextSongProvider>();
}
std::shared_ptr<ISongProvider> CreateJsonSongProvider() {
    return std::make_shared<JsonSongProvider>();
}

} // namespace bps::song
