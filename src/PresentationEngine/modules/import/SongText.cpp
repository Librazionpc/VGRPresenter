#include "modules/import/SongText.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <regex>
#include <set>

namespace bps::import {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// small string helpers (JavaScript's split / trim / slice, on UTF-8 bytes)
// ---------------------------------------------------------------------------------------------------------------------

std::vector<std::string> Split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t at = s.find(sep, start);
        if (at == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, at - start));
        start = at + sep.size();
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += sep; out += parts[i]; }
    return out;
}

bool IsSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& c : s) if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) { s.replace(at, from.size(), to); at += to.size(); }
    return s;
}

// Removes every character of `chars` (the [\[\]'":]+ style clean-ups).
std::string Strip(const std::string& s, const std::string& chars) {
    std::string out;
    for (char c : s) if (chars.find(c) == std::string::npos) out += c;
    return out;
}

// "x2" / "x0-9" markers: an x followed by one digit.
std::string RemoveXDigit(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == 'x' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) { ++i; continue; }
        out += s[i];
    }
    return out;
}

std::string RemoveDigits(const std::string& s) {
    std::string out;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) out += c;
    return out;
}

const char* const kCyrillicX = "\xD1\x85";   // "х", which people type for "x2"

// ---------------------------------------------------------------------------------------------------------------------
// groups
// ---------------------------------------------------------------------------------------------------------------------

struct StandardGroup { const char* id; const char* label; };
constexpr StandardGroup kGroups[] = {
    { "intro", "Intro" }, { "verse", "Verse" }, { "pre_chorus", "Pre-Chorus" }, { "chorus", "Chorus" },
    { "break", "Break" }, { "tag", "Tag" }, { "bridge", "Bridge" }, { "outro", "Outro" },
};

// FreeShow's getLabelId: a label as an id ("Pre-Chorus 2" -> "pre_chorus").
std::string LabelId(const std::string& label) {
    if (label.empty()) return "";
    std::string s = RemoveXDigit(Lower(label));
    s = Strip(s, "[]'\":");
    s = Trim(s);
    s = ReplaceAll(s, " ", "_");
    s = ReplaceAll(s, "-", "_");
    s = RemoveDigits(s);
    if (!s.empty() && s.back() == '_') s.pop_back();
    return s;
}

} // namespace

std::string FindGroupMatch(const std::string& group) {
    if (group.empty()) return "";
    // brackets, colons, repeat markers (x2) and surrounding numbers do not count
    std::string label = Strip(group, "[]'\":");
    label = std::regex_replace(label, std::regex(std::string("[xX]\\d+|") + kCyrillicX + "\\d+"), "");
    label = Trim(label);
    while (!label.empty() && std::isdigit(static_cast<unsigned char>(label.front()))) label.erase(label.begin());
    while (!label.empty() && std::isdigit(static_cast<unsigned char>(label.back()))) label.pop_back();
    label = Lower(Trim(label));
    if (label.empty()) return "";
    const std::string whole = Lower(Trim(group));
    for (const StandardGroup& g : kGroups)
        if (label == g.id || label == Lower(g.label) || whole == Lower(g.label)) return g.id;
    return "";
}

std::string GroupLabel(const std::string& group) {
    for (const StandardGroup& g : kGroups)
        if (group == g.id) return g.label;
    return group;
}

double TextSimilarity(const std::string& a, const std::string& b) {
    const std::string& longer = a.size() < b.size() ? b : a;
    const std::string& shorter = a.size() < b.size() ? a : b;
    if (longer.empty()) return 1.0;
    const std::string s1 = Lower(longer), s2 = Lower(shorter);
    std::vector<size_t> costs(s2.size() + 1);
    for (size_t i = 0; i <= s1.size(); ++i) {
        size_t last = i;
        for (size_t j = 0; j <= s2.size(); ++j) {
            if (i == 0) { costs[j] = j; continue; }
            if (j > 0) {
                size_t next = costs[j - 1];
                if (s1[i - 1] != s2[j - 1]) next = std::min({ next, last, costs[j] }) + 1;
                costs[j - 1] = last;
                last = next;
            }
        }
        if (i > 0) costs[s2.size()] = last;
    }
    return (static_cast<double>(longer.size()) - static_cast<double>(costs[s2.size()])) / static_cast<double>(longer.size());
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// headers and chords
// ---------------------------------------------------------------------------------------------------------------------

// "[Verse]", "Verse 1:", "Chorus: x2"
bool IsHeaderLine(const std::string& line) {
    const std::string t = Trim(line);
    if (t.empty()) return false;
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') return true;

    std::string body = t;
    // an optional repeat count after the colon
    static const std::regex repeat(std::string("\\s*(?:[xX]|") + kCyrillicX + ")\\d+$");
    std::smatch m;
    std::string withoutRepeat = body;
    if (std::regex_search(body, m, repeat)) {
        const std::string before = body.substr(0, static_cast<size_t>(m.position(0)));
        if (!before.empty() && Trim(before).back() == ':') withoutRepeat = Trim(before);
    }
    if (withoutRepeat.empty() || withoutRepeat.back() != ':') return false;
    const std::string name = Trim(withoutRepeat.substr(0, withoutRepeat.size() - 1));
    if (name.empty()) return false;
    for (unsigned char c : name)
        if (!(std::isalnum(c) || c >= 0x80 || c == '_' || c == '-' || IsSpace(static_cast<char>(c)))) return false;
    return true;
}

const std::regex& ChordRegex() {
    static const std::regex r(R"(\b[A-G][#b]?(?:m|M|maj|min|dim|aug|sus|add)?(?:\d+)?(?:\/[A-G][#b]?)?\b)");
    return r;
}

bool IsChordLine(const std::string& line) {
    if (Trim(line).empty() || IsHeaderLine(line)) return false;
    std::string nonWhitespace;
    for (char c : line) if (!IsSpace(c)) nonWhitespace += c;
    if (nonWhitespace.empty()) return false;
    size_t chordChars = 0;
    size_t count = 0;
    for (auto it = std::sregex_iterator(line.begin(), line.end(), ChordRegex()); it != std::sregex_iterator(); ++it) {
        chordChars += static_cast<size_t>(it->length(0));
        ++count;
    }
    if (count == 0) return false;
    const size_t nonChord = nonWhitespace.size() > chordChars ? nonWhitespace.size() - chordChars : 0;
    return static_cast<double>(nonChord) / static_cast<double>(nonWhitespace.size()) <= 0.2;
}

std::string InsertChordsIntoLyrics(const std::string& chordLine, const std::string& lyric) {
    struct Placed { std::string chord; size_t pos; };
    std::vector<Placed> chords;
    for (auto it = std::sregex_iterator(chordLine.begin(), chordLine.end(), ChordRegex()); it != std::sregex_iterator(); ++it) {
        const std::string chord = it->str(0);
        if (chord.size() > 12) continue;
        size_t pos = static_cast<size_t>(it->position(0));
        // a chord over a space belongs to the next word
        if (pos < lyric.size() && lyric[pos] == ' ')
            while (pos < lyric.size() && lyric[pos] == ' ') ++pos;
        chords.push_back({ chord, pos });
    }
    if (chords.empty()) return lyric;

    std::string out;
    size_t next = 0;
    for (size_t pos = 0; pos < lyric.size(); ++pos) {
        while (next < chords.size() && chords[next].pos == pos) out += "[" + chords[next++].chord + "]";
        out += lyric[pos];
    }
    while (next < chords.size()) out += "[" + chords[next++].chord + "]";   // at or past the end of the line
    return out;
}

std::vector<std::string> PreprocessLines(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size()) {
        const std::string& current = lines[i];
        if (IsHeaderLine(current)) {   // brackets everywhere: "Verse 1:" -> "[Verse 1]"
            out.push_back("[" + Trim(Strip(current, "[]:")) + "]");
            ++i;
            continue;
        }
        if (IsChordLine(current)) {
            size_t j = i + 1;
            if (j < lines.size() && Trim(lines[j]).empty()) ++j;   // one blank line between chords and words is allowed
            if (j < lines.size() && !Trim(lines[j]).empty() && !IsChordLine(lines[j]) && !IsHeaderLine(lines[j])) {
                out.push_back(InsertChordsIntoLyrics(current, lines[j]));
                i = j + 1;
            } else {   // a chords-only line (intro, instrumental): every chord in brackets
                out.push_back(std::regex_replace(current, ChordRegex(), "[$&]"));
                ++i;
            }
        } else {
            out.push_back(current);
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// metadata
// ---------------------------------------------------------------------------------------------------------------------

const std::vector<std::string>& MetaKeys() {
    static const std::vector<std::string> keys = { "number", "title", "artist", "author", "composer", "publisher", "copyright", "CCLI", "year", "key" };
    return keys;
}

// ---------------------------------------------------------------------------------------------------------------------
// finding the group of each section
// ---------------------------------------------------------------------------------------------------------------------

struct Labeled { std::string type; std::string text; };

bool LinesSimilar(const std::string& text) {
    const std::vector<std::string> lines = Split(text, "\n");
    if (lines.size() < 3) return false;
    for (size_t i = 1; i < lines.size(); ++i)
        if (TextSimilarity(lines[i - 1], lines[i]) > 0.95) return true;
    return false;
}

constexpr double kSimilar = 0.7;

struct Pattern {
    std::vector<size_t> matches;
    int count = 0;
};

std::vector<std::string> FindPatterns(std::vector<std::string>& sections, bool autoGroups) {
    std::vector<std::shared_ptr<Pattern>> similar(sections.size());
    int totalMatches = 0;
    for (size_t i = 0; i < sections.size(); ++i) {
        similar[i] = std::make_shared<Pattern>();
        std::shared_ptr<Pattern> already;
        for (size_t k = 0; k < i && !already; ++k)   // an earlier section this one was found alike to shares its record
            if (similar[k] && std::find(similar[k]->matches.begin(), similar[k]->matches.end(), i) != similar[k]->matches.end()) already = similar[k];
        if (already) { similar[i] = already; continue; }
        for (size_t j = 0; j < sections.size(); ++j) {
            if (i == j || kSimilar > TextSimilarity(sections[i], sections[j])) continue;
            ++similar[i]->count;
            similar[i]->matches.push_back(j);
        }
        if (similar[i]->count > 0) ++totalMatches;
    }

    int matches = 0;
    std::vector<Labeled> stored;
    std::vector<std::string> indexes;
    static const std::vector<std::string> globalGroups = { "pre_chorus", "chorus", "bridge", "bridge", "bridge" };

    for (size_t i = 0; i < sections.size(); ++i) {
        std::vector<std::string> splitted;
        for (const std::string& l : Split(sections[i], "\n")) if (!l.empty()) splitted.push_back(l);
        if (splitted.empty()) { indexes.push_back("break"); continue; }

        const size_t length = ReplaceAll(sections[i], "\n", "").size();
        const std::string rawName = Trim(Strip(splitted[0], "[]'\":"));
        if (const std::string exact = FindGroupMatch(rawName); !exact.empty()) { indexes.push_back(exact); continue; }

        bool foundStored = false;
        for (const Labeled& s : stored)
            if (TextSimilarity(s.text, sections[i]) > kSimilar) { indexes.push_back(s.type); foundStored = true; break; }
        if (foundStored) continue;

        const std::string name = LabelId(splitted[0]);
        if (const std::string m = FindGroupMatch(name); !m.empty()) { indexes.push_back(m); continue; }

        // a first line that is only a [bracket] or ends with a colon is the section's own name
        std::smatch bracket;
        static const std::regex bracketRe(R"(\[[^\]]*\])");
        const size_t bracketLen = std::regex_search(splitted[0], bracket, bracketRe) ? static_cast<size_t>(bracket.length(0)) : 0;
        const std::string trimmedFirst = Trim(splitted[0]);
        const bool endsWithColon = splitted[0].size() - 1 < trimmedFirst.size() && trimmedFirst[splitted[0].size() - 1] == ':';
        if (bracketLen == splitted[0].size() || endsWithColon) {
            indexes.push_back(Trim(RemoveDigits(RemoveXDigit(Strip(splitted[0], "[]'\":")))));
            continue;
        }

        if (autoGroups) {
            if (length < 30 || LinesSimilar(sections[i])) { indexes.push_back("tag"); continue; }

            const std::string cleanGroup = Trim(RemoveDigits(splitted[0]));
            const std::string matchedGroup = FindGroupMatch(cleanGroup);
            const bool punctuated = splitted[0].find_first_of(",.!?-") != std::string::npos;
            if (splitted[0].size() < 8 && splitted.size() > 1 && splitted[1].size() > 20 && !punctuated && !matchedGroup.empty()) {
                sections[i] = Join(std::vector<std::string>(splitted.begin() + 1, splitted.end()), "\n");
                indexes.push_back(matchedGroup);
                continue;
            }

            if (similar[i]->count > 0) {
                ++matches;
                std::string group = matches < static_cast<int>(globalGroups.size()) ? globalGroups[static_cast<size_t>(matches)] : "";
                if (totalMatches > 2) group = matches - 1 < static_cast<int>(globalGroups.size()) ? globalGroups[static_cast<size_t>(matches - 1)] : "other";
                if (group.empty()) group = "other";
                stored.push_back({ group, sections[i] });
                indexes.push_back(group);
                continue;
            }
        }
        indexes.push_back("verse");
    }
    return indexes;
}

} // namespace

std::vector<std::string> AutoGroupSections(std::vector<std::string>& sections, bool autoGroups) {
    return FindPatterns(sections, autoGroups);
}

namespace {

// A section that ends with "x3" (or a header line "Chorus: x3") is played that many times.
std::vector<Labeled> CheckRepeats(const std::vector<Labeled>& labeled) {
    static const std::regex marker(std::string("(?:\\n|^[^\\n]+?)\\s*(?:x|X|") + kCyrillicX + ")([0-9]+)\\s*(?:\\n|$)");
    std::vector<Labeled> out;
    for (Labeled a : labeled) {
        std::smatch m;
        if (std::regex_search(a.text, m, marker) && m[1].matched) {
            const int times = std::stoi(m[1].str());
            if (times > 0 && times < 10) {
                // remove the marker: whitespace, x, digits, whitespace, at the end of a line (the first one)
                for (size_t i = 0; i < a.text.size(); ++i) {
                    const bool isX = a.text[i] == 'x' || a.text[i] == 'X' || a.text.compare(i, 2, kCyrillicX) == 0;
                    if (!isX) continue;
                    size_t d = i + (a.text[i] == 'x' || a.text[i] == 'X' ? 1 : 2);
                    const size_t digitsStart = d;
                    while (d < a.text.size() && std::isdigit(static_cast<unsigned char>(a.text[d]))) ++d;
                    if (d == digitsStart) continue;
                    size_t end = d;
                    while (end < a.text.size() && IsSpace(a.text[end])) ++end;
                    while (end > d && !(end == a.text.size() || a.text[end] == '\n')) --end;
                    if (!(end == a.text.size() || a.text[end] == '\n')) continue;
                    size_t begin = i;
                    while (begin > 0 && IsSpace(a.text[begin - 1])) --begin;
                    a.text.erase(begin, end - begin);
                    break;
                }
                a.text = Trim(a.text);
                for (int i = 0; i < times; ++i) out.push_back(a);
                continue;
            }
        }
        out.push_back(a);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// one section's text -> the slide's lines
// ---------------------------------------------------------------------------------------------------------------------

std::string FixText(std::string text, bool formatText) {
    if (formatText) text = std::regex_replace(text, std::regex(R"(\([^)]{1,2}\) )"), "");

    // the group name at the start is not part of the words
    if (!text.empty() && text[0] == '[' && Contains(text, "]")) text = text.substr(text.find(']') + 1);
    const std::vector<std::string> first = Split(text, "\n");
    const size_t colon = text.find(':');
    if (colon != std::string::npos && !first.empty() && colon + 1 == first[0].size()) {
        const size_t words = Split(first[0], " ").size();
        if (formatText || words < 3) text = text.substr(colon + 1);
    }

    if (formatText) {
        size_t a = text.find(":/:");
        size_t b = a == std::string::npos ? std::string::npos : text.find(":/:", a + 1);
        while (a != std::string::npos && b != std::string::npos) {
            const std::string repeated = text.substr(a + 3, b - a - 3);
            text = text.substr(0, a) + repeated + repeated + text.substr(b + 3);
            a = text.find(":/:");
            b = a == std::string::npos ? std::string::npos : text.find(":/:", a + 1);
        }
        std::string rebuilt;
        constexpr size_t kCommaBreak = 22;   // a comma only breaks a line when both halves are long enough
        for (const std::string& t : Split(text, "\n")) {
            std::vector<std::string> parts;
            for (const std::string& p : Split(t, ",")) if (!p.empty()) parts.push_back(p);
            for (size_t i = 0; i < parts.size(); ++i) {
                rebuilt += parts[i];
                if (i + 1 >= parts.size()) rebuilt += "\n";
                else if (parts[i].size() < kCommaBreak || parts[i + 1].size() < kCommaBreak) rebuilt += ",";
                else rebuilt += "\n";
            }
        }
        text = rebuilt;
    }

    std::vector<std::string> lines = Split(text, "\n");
    if (formatText)
        for (std::string& line : lines) {
            line = Trim(line);
            if (!line.empty() && static_cast<unsigned char>(line[0]) < 0x80) line[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
            while (!line.empty() && (line.back() == '.' || line.back() == ',' || line.back() == '!')) line.pop_back();
            line = Trim(line);
        }

    const std::string firstLine = lines.empty() ? "" : lines[0];
    if (!FindGroupMatch(LabelId(firstLine)).empty() || IsHeaderLine(firstLine)) lines.erase(lines.begin());

    std::vector<std::string> kept;
    for (const std::string& l : lines) if (!l.empty()) kept.push_back(l);
    return Join(kept, "\n");
}

std::string TrimNameFromString(const std::string& text) {
    if (text.empty()) return "";
    const std::vector<std::string> lines = Split(text, "\n");
    std::string name = lines[0];
    if (lines.size() > 1 && (Contains(name, "[") || Contains(name, ":"))) name = lines[1];
    std::string cleaned;
    bool inTag = false;
    for (char c : name) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>' && inTag) { inTag = false; continue; }
        if (inTag || c == ',' || c == '.' || c == '!') continue;
        cleaned += c;
    }
    name = Trim(cleaned);
    if (name.size() > 30) {
        const size_t space = name.find(' ', 30);
        name = space == std::string::npos ? name : name.substr(0, space);
    }
    if (name.size() > 38) name = name.substr(0, 30);
    return name;
}

// One line: its words with the [C] markers taken out, and where each chord sat.
struct ParsedLine { std::string text; std::string chords; /* JSON array of {pos,key} */ };

ParsedLine ParseChordedLine(const std::string& line) {
    ParsedLine out;
    // FreeShow leaves a line alone when it holds a colon before its last two characters (a "Note: [x]" line, not chords).
    const bool literal = line.size() > 2 && Contains(line.substr(0, line.size() - 2), ":");
    if (literal) { out.text = line; return out; }
    std::string chords;
    bool inChord = false;
    std::string key;
    size_t letters = 0;   // characters (not bytes) of the words so far
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '[') { inChord = true; key.clear(); continue; }
        if (c == ']' && inChord) {
            inChord = false;
            if (!chords.empty()) chords += ",";
            chords += std::format("{{\"pos\":{},\"key\":\"{}\"}}", letters, ReplaceAll(ReplaceAll(key, "\\", "\\\\"), "\"", "\\\""));
            continue;
        }
        if (inChord) { key += c; continue; }
        out.text += c;
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++letters;
    }
    if (inChord) out.text += "[" + key;   // an unclosed bracket was words after all
    out.chords = chords.empty() ? "" : "[" + chords + "]";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// the show
// ---------------------------------------------------------------------------------------------------------------------

struct SlideDraft {
    std::string group;                  // empty = a child slide
    std::string label;
    std::string text;
    std::string notes;
    std::string chordsJson;
    std::vector<size_t> children;       // indexes into the drafts
};

ImportedSlide ToSlide(const std::string& lines, const std::string& notes) {
    ImportedSlide slide;
    std::string text;
    std::string chords;
    bool anyChords = false;
    for (const std::string& raw : Split(lines, "\n")) {
        // ChordPro styling lines ({...}) are not words
        if (!raw.empty() && (raw.front() == '{' || raw.back() == '}')) continue;
        const ParsedLine parsed = ParseChordedLine(raw);
        if (!text.empty() || !chords.empty()) { text += "\n"; chords += ","; }
        text += parsed.text;
        chords += parsed.chords.empty() ? "[]" : parsed.chords;
        if (!parsed.chords.empty()) anyChords = true;
    }
    slide.text = text;
    slide.notes = notes;
    slide.chordsJson = anyChords ? "[" + chords + "]" : "";
    return slide;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// ParseSongText
// ---------------------------------------------------------------------------------------------------------------------

ImportedShow ParseSongText(const std::string& input, const SongTextOptions& options) {
    ImportedShow show;
    show.category = options.category;
    show.origin = "txt";

    std::string text = ReplaceAll(ReplaceAll(input, "\r", ""), "\n \n", "\n\n");

    // a trailing web address is where the song came from
    std::string source;
    {
        static const std::regex trailingUrl(R"(\n\s*(https?://\S+)\s*$)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(text, m, trailingUrl)) {
            source = Trim(m[1].str());
            text = text.substr(0, static_cast<size_t>(m.position(0)));
        }
    }

    const std::string processed = Join(PreprocessLines(Split(text, "\n")), "\n");
    std::vector<std::string> sections;
    for (const std::string& s : Split(processed, "\n\n")) if (!s.empty()) sections.push_back(s);

    // "Artist=Casting Crowns" lines are metadata, not words
    std::map<std::string, std::string> meta;
    std::string plainNotes;
    for (std::string& section : sections) {
        std::vector<std::string> kept;
        for (const std::string& line : Split(section, "\n")) {
            if (!Contains(line, "=")) { kept.push_back(line); continue; }
            const std::vector<std::string> pair = Split(line, "=");
            std::string key = Lower(ReplaceAll(pair[0], " ", ""));
            key = Trim(key);
            const std::string value = pair.size() > 1 ? pair[1] : "";
            if (key == "notes") { plainNotes = value; continue; }
            bool known = false;
            for (const std::string& k : MetaKeys())
                if (Lower(ReplaceAll(k, " ", "")) == key) { meta[k] = value; known = true; break; }
            if (!known) meta[pair[0]] = value;   // kept as it was written, for this show only
        }
        section = Join(kept, "\n");
    }
    if (!sections.empty() && sections[0].empty()) sections.erase(sections.begin());

    // SongSelect's footer
    std::string ccliBlock;
    if (meta.empty() && !sections.empty() && Contains(sections.back(), "www.ccli.com")) {
        ccliBlock = sections.back();
        sections.pop_back();
    }

    const std::vector<std::string> types = FindPatterns(sections, options.autoGroups);
    std::vector<Labeled> labeled;
    for (size_t i = 0; i < types.size(); ++i) labeled.push_back({ types[i], i < sections.size() ? sections[i] : "" });
    labeled = CheckRepeats(labeled);

    std::string name = options.name;
    if (name.empty()) name = meta.count("title") ? meta["title"] : TrimNameFromString(labeled.empty() ? "" : labeled[0].text);
    show.name = Trim(name);

    // ---- the slides ----
    // Each section becomes a parent slide (a group) and any child slides after it. A section with no header of its own hangs
    // from the group above it.
    constexpr size_t kNone = static_cast<size_t>(-1);
    std::vector<SlideDraft> drafts;
    std::vector<size_t> order;          // the parents in play order
    bool hasActive = false;
    size_t activeIndex = kNone;

    auto hasLines = [](const std::string& chunk) {
        for (const std::string& raw : Split(chunk, "\n"))
            if (raw.empty() || !(raw.front() == '{' || raw.back() == '}')) return true;
        return false;
    };

    for (const Labeled& a : labeled) {
        const std::string trimmed = Lower(Trim(a.text));
        if (trimmed == "[" + Lower(a.type) + "]" || trimmed == Lower(a.type) + ":") {   // a header with nothing under it: an empty slide of that group
            SlideDraft d;
            d.group = a.type;
            d.label = FindGroupMatch(a.type).empty() ? a.type : GroupLabel(a.type);
            drafts.push_back(d);
            order.push_back(drafts.size() - 1);
            hasActive = false;
            continue;
        }

        const std::string slideText = FixText(a.text, !options.noFormatting);
        const std::string tt = Trim(a.text);
        const bool hasTextGroup = (!tt.empty() && tt[0] == '[' && Contains(a.text, "]")) ||
                                  (!a.text.empty() && a.text.size() - 1 < tt.size() && tt[a.text.size() - 1] == ':');
        if (hasTextGroup) { hasActive = true; activeIndex = kNone; }

        std::string group = (hasActive && !hasTextGroup) ? "" : a.type;
        if (!options.autoGroups && !hasTextGroup && !group.empty()) {
            const std::string matched = FindGroupMatch(group);
            group = matched.empty() ? "verse" : matched;
        }

        // "---" starts the slide's notes
        std::vector<std::string> textAndNotes = Split(slideText, "---");
        auto onlyDashes = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c == '-'; }); };
        while (!textAndNotes.empty() && onlyDashes(textAndNotes[0])) textAndNotes.erase(textAndNotes.begin());
        std::string body = textAndNotes.empty() ? "" : textAndNotes[0];
        if (!textAndNotes.empty()) textAndNotes.erase(textAndNotes.begin());
        if (!body.empty() && body.back() == '\n') body.pop_back();
        while (!textAndNotes.empty() && textAndNotes[0].empty()) textAndNotes.erase(textAndNotes.begin());
        std::string notes = Join(textAndNotes, "\n");
        if (!notes.empty()) notes.erase(notes.begin());

        std::vector<std::string> slideLines;
        for (const std::string& l : Split(body, "\n")) if (!l.empty()) slideLines.push_back(l);

        std::vector<std::string> chunks;
        if (options.splitLines > 0 && static_cast<int>(slideLines.size()) > options.splitLines) {
            const size_t per = static_cast<size_t>(options.splitLines);
            for (size_t i = 0; i < slideLines.size(); i += per)
                chunks.push_back(Join(std::vector<std::string>(slideLines.begin() + static_cast<long>(i),
                                                              slideLines.begin() + static_cast<long>(std::min(slideLines.size(), i + per))), "\n"));
        } else {
            chunks.push_back(body);
        }

        size_t parent = kNone;
        std::vector<size_t> children;
        for (size_t c = 0; c < chunks.size(); ++c) {
            if (!hasLines(chunks[c])) continue;
            const ImportedSlide slide = ToSlide(chunks[c], c == 0 ? notes : std::string());
            if (c > 0 && Trim(slide.text).empty()) continue;   // no empty child slides
            SlideDraft d;
            d.text = slide.text; d.notes = slide.notes; d.chordsJson = slide.chordsJson;
            if (c == 0) {
                d.group = group;
                d.label = group.empty() ? "" : (FindGroupMatch(group).empty() ? group : GroupLabel(group));
            }
            drafts.push_back(d);
            (c == 0 ? parent : children.emplace_back()) = drafts.size() - 1;
        }

        if (group.empty()) {   // hangs from the group above it
            if (!hasActive || activeIndex == kNone) continue;
            if (parent != kNone) drafts[activeIndex].children.push_back(parent);
            for (size_t child : children) drafts[activeIndex].children.push_back(child);
        } else {
            if (parent == kNone) continue;
            drafts[parent].children = children;
            order.push_back(parent);
            if (hasTextGroup) activeIndex = parent;
        }
    }

    // ---- into sections ----
    for (size_t parent : order) {
        const SlideDraft& d = drafts[parent];
        ImportedSection section;
        section.group = d.group;
        section.label = d.label;
        section.slides.push_back({ d.text, d.notes, d.chordsJson });
        for (size_t child : d.children) section.slides.push_back({ drafts[child].text, drafts[child].notes, drafts[child].chordsJson });
        show.sections.push_back(std::move(section));
    }

    // ---- metadata ----
    if (!ccliBlock.empty()) {
        const std::vector<std::string> lines = Split(ccliBlock, "\n");
        auto at = [&](size_t i) { return i < lines.size() ? lines[i] : std::string(); };
        auto afterHash = [](const std::string& s, size_t skip) { const size_t p = s.find('#'); return p == std::string::npos ? std::string() : s.substr(std::min(s.size(), p + skip)); };
        if (Contains(at(4), "CCLI")) {   // SongSelect's own order
            meta = { { "title", show.name }, { "CCLI", afterHash(at(4), 1) }, { "author", at(0) }, { "copyright", at(2) } };
        } else {
            meta = { { "title", show.name }, { "CCLI", afterHash(at(0), 2) }, { "artist", at(1) }, { "author", at(2) },
                     { "composer", at(3) }, { "publisher", at(1) }, { "copyright", at(4) } };
        }
    }
    if (!source.empty() && !meta.count("publisher")) meta["publisher"] = source;
    show.meta = meta;
    show.notes = plainNotes;
    return show;
}

} // namespace bps::import
