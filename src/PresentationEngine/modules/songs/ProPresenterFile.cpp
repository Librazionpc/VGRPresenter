#include "modules/songs/ProPresenterFile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bps::song {
namespace {
constexpr const char* kModule = "SongProvider";

// ---------------------------------------------------------------------------
// Protobuf wire format — only the shapes these documents use (varint and
// length-delimited; fixed32/fixed64 fields are skipped, nothing else appears).
// ---------------------------------------------------------------------------
struct FieldRef {
    uint32_t number = 0;
    uint64_t value = 0;          // wire 0
    std::string_view bytes;      // wire 2
};

bool ReadVarint(std::string_view buffer, size_t& position, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    while (position < buffer.size() && shift < 64) {
        const auto byte = static_cast<unsigned char>(buffer[position++]);
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

template <typename Visit>
void ForEachField(std::string_view message, Visit&& visit) {
    size_t position = 0;
    while (position < message.size()) {
        uint64_t key = 0;
        if (!ReadVarint(message, position, key)) return;
        const auto number = static_cast<uint32_t>(key >> 3);
        const int wire = static_cast<int>(key & 0x7);
        if (number == 0) return;
        FieldRef field;
        field.number = number;
        if (wire == 0) {
            if (!ReadVarint(message, position, field.value)) return;
            visit(field);
            continue;
        }
        if (wire == 2) {
            uint64_t length = 0;
            if (!ReadVarint(message, position, length)) return;
            if (length > message.size() - position) return;
            field.bytes = message.substr(position, static_cast<size_t>(length));
            position += static_cast<size_t>(length);
            visit(field);
            continue;
        }
        if (wire == 5) {
            if (message.size() - position < 4) return;
            position += 4;
            continue;
        }
        if (wire == 1) {
            if (message.size() - position < 8) return;
            position += 8;
            continue;
        }
        return;   // deprecated groups, and the reserved 6/7 wire types
    }
}

// First length-delimited value of `number` (these documents never repeat the
// scalar fields this reads).
std::string_view FirstBytes(std::string_view message, uint32_t number) {
    std::string_view found;
    bool have = false;
    ForEachField(message, [&](const FieldRef& field) {
        if (!have && field.number == number && !field.bytes.empty()) {
            found = field.bytes;
            have = true;
        }
    });
    return found;
}

// `rv.data.UUID` is just { string } — 36 printable characters.
std::string UuidOf(std::string_view uuidMessage) {
    const std::string_view text = FirstBytes(uuidMessage, 1);
    for (unsigned char c : text)
        if (c < 0x21 || c > 0x7E) return {};
    return std::string(text);
}

bool LooksLikeText(std::string_view text) {
    if (text.empty() || text.size() > 300) return false;
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool LooksLikeRtf(std::string_view bytes) {
    return bytes.size() > 5 && bytes.compare(0, 5, "{\\rtf") == 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool IsSkippedDestination(std::string_view word) {
    static constexpr std::array<std::string_view, 21> kSkipped = {
        "fonttbl", "colortbl", "expandedcolortbl", "stylesheet", "info", "pict",
        "listtable", "listoverridetable", "rsidtbl", "generator", "themedata",
        "colorschememapping", "latentstyles", "cocoartf", "cocoatextscaling",
        "cocoaplatform", "filetbl", "revtbl", "xmlnstbl", "fontemb", "mmathPr",
    };
    return std::find(kSkipped.begin(), kSkipped.end(), word) != kSkipped.end();
}

void AppendUtf8(std::string& out, unsigned int codepoint) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// \'hh is a byte in the document's code page (these are cp1252). 0x80..0x9F
// differ from Latin-1, so they are mapped; the rest are their own code point.
void AppendCodePageByte(std::string& out, int byte) {
    static constexpr std::array<unsigned int, 32> kCp1252 = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    if (byte >= 0x80 && byte <= 0x9F) AppendUtf8(out, kCp1252[static_cast<size_t>(byte - 0x80)]);
    else AppendUtf8(out, static_cast<unsigned int>(byte));
}

std::string RtfToText(std::string_view rtf) {
    std::string out;
    std::vector<bool> skippedGroup;   // per open group: is this a skipped destination
    int skipDepth = 0;
    size_t i = 0;
    while (i < rtf.size()) {
        const char c = rtf[i];
        if (c == '{') {
            size_t j = i + 1;
            bool skip = false;
            if (j < rtf.size() && rtf[j] == '\\') {
                ++j;
                // A destination opens "{ \* \fonttbl ... }": the \* marker
                // ('skip me unless you know this one'), then the \fonttbl word.
                if (j < rtf.size() && rtf[j] == '*') {
                    ++j;
                    if (j < rtf.size() && rtf[j] == '\\') ++j;
                }
                const size_t start = j;
                while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
                skip = IsSkippedDestination(rtf.substr(start, j - start));
            }
            skippedGroup.push_back(skip);
            if (skip) ++skipDepth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (!skippedGroup.empty()) {
                if (skippedGroup.back()) --skipDepth;
                skippedGroup.pop_back();
            }
            ++i;
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= rtf.size()) break;
            const char next = rtf[i + 1];
            if (next == '\\' || next == '{' || next == '}') {
                if (skipDepth == 0) out.push_back(next);
                i += 2;
                continue;
            }
            if (next == '\'') {
                if (i + 3 < rtf.size()) {
                    const int high = HexValue(rtf[i + 2]);
                    const int low = HexValue(rtf[i + 3]);
                    if (high >= 0 && low >= 0 && skipDepth == 0) AppendCodePageByte(out, high * 16 + low);
                    i += 4;
                } else {
                    i += 2;
                }
                continue;
            }
            if (next == '~') {
                if (skipDepth == 0) out.push_back(' ');
                i += 2;
                continue;
            }
            if (next == '_') {
                if (skipDepth == 0) out.push_back('-');
                i += 2;
                continue;
            }
            if (next == '\n' || next == '\r') {
                if (skipDepth == 0) out.push_back('\n');
                i += 2;
                continue;
            }
            if (!std::isalpha(static_cast<unsigned char>(next))) {
                i += 2;   // \* \| and friends: no visible text
                continue;
            }
            size_t j = i + 1;
            while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
            const std::string_view word = rtf.substr(i + 1, j - i - 1);
            bool negative = false;
            if (j < rtf.size() && rtf[j] == '-') {
                negative = true;
                ++j;
            }
            const size_t digitsStart = j;
            while (j < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[j]))) ++j;
            const bool hasParam = j > digitsStart;
            long long parameter = 0;
            if (hasParam) {
                for (size_t d = digitsStart; d < j; ++d) parameter = parameter * 10 + (rtf[d] - '0');
                if (negative) parameter = -parameter;
            }
            if (j < rtf.size() && rtf[j] == ' ') ++j;   // the word's delimiter space
            if (skipDepth == 0) {
                if (word == "par" || word == "line" || word == "row" || word == "sect") out.push_back('\n');
                else if (word == "tab") out.push_back('\t');
                else if (word == "u" && hasParam) {
                    long long codepoint = parameter;
                    if (codepoint < 0) codepoint += 65536;
                    if (codepoint > 0 && codepoint <= 0x10FFFF) AppendUtf8(out, static_cast<unsigned int>(codepoint));
                }
            }
            if (word == "u" && hasParam && j < rtf.size()) {
                // \uN is followed by a fallback character the reader must skip.
                if (rtf[j] == '?') {
                    ++j;
                } else if (j + 3 < rtf.size() && rtf[j] == '\\' && rtf[j + 1] == '\'') {
                    j += 4;
                }
            }
            i = j;
            continue;
        }
        if (c != '\r' && c != '\n' && skipDepth == 0) out.push_back(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void CollectTextRuns(std::string_view message, std::vector<std::string_view>& out, int depth) {
    if (depth > 12) return;
    ForEachField(message, [&](const FieldRef& field) {
        if (field.bytes.empty()) return;
        if (LooksLikeRtf(field.bytes)) {
            if (field.number != 1) out.push_back(field.bytes);
            return;
        }
        CollectTextRuns(field.bytes, out, depth + 1);
    });
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool IsMediaPath(std::string_view path) {
    if (path.size() < 5 || path.size() > 1024) return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7F) return false;
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.size() - dot < 3) return false;
    return path.find('/') != std::string_view::npos || path.find('\\') != std::string_view::npos;
}

// A URL message is the one carrying the platform tag (field 3) — its path is in
// `local.path` (4.2), `external.path` (5.3) or the bare storage string (2/1).
std::string MediaPathOf(std::string_view message) {
    bool hasPlatform = false;
    std::string_view absolute;
    std::string_view relative;
    std::string_view local;
    std::string_view external;
    ForEachField(message, [&](const FieldRef& field) {
        if (field.number == 3) hasPlatform = true;
        if (field.number == 1 && field.bytes.size() > 0) absolute = field.bytes;
        if (field.number == 2 && field.bytes.size() > 0) relative = field.bytes;
        if (field.number == 4 && field.bytes.size() > 0) local = FirstBytes(field.bytes, 2);
        if (field.number == 5 && field.bytes.size() > 0) external = FirstBytes(field.bytes, 3);
    });
    if (!hasPlatform) return {};
    if (IsMediaPath(local)) return std::string(local);
    if (IsMediaPath(external)) return std::string(external);
    if (IsMediaPath(relative)) return std::string(relative);
    std::string path(absolute);
    if (path.rfind("file://", 0) == 0) path.erase(0, 7);
    // "file:///C:/…" leaves a leading slash before the drive letter.
    if (path.size() > 2 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
    return IsMediaPath(path) ? path : std::string();
}

bool IsMediaName(std::string_view bytes) {
    if (bytes.size() < 5 || bytes.size() > 200) return false;
    if (!LooksLikeText(bytes)) return false;
    if (bytes.find('/') != std::string_view::npos || bytes.find('\\') != std::string_view::npos) return false;
    const size_t dot = bytes.rfind('.');
    if (dot == std::string_view::npos || bytes.size() - dot < 4 || bytes.size() - dot > 6) return false;
    std::string extension;
    for (size_t i = dot; i < bytes.size(); ++i)
        extension.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(bytes[i]))));
    static constexpr std::array<std::string_view, 17> kMedia = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff", ".webp", ".svg",
        ".mp4", ".mov", ".m4v", ".webm", ".avi", ".mp3", ".wav", ".m4a",
    };
    return std::find(kMedia.begin(), kMedia.end(), extension) != kMedia.end();
}

void CollectMediaPaths(std::string_view message, std::vector<std::string>& out, int depth) {
    if (depth > 12 || out.size() >= 1000) return;
    ForEachField(message, [&](const FieldRef& field) {
        if (field.bytes.empty()) return;
        if (const std::string path = MediaPathOf(field.bytes); !path.empty()) {
            if (std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
            return;
        }
        if (IsMediaName(field.bytes)) {
            const std::string name(field.bytes);
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
            return;
        }
        CollectMediaPaths(field.bytes, out, depth + 1);
    });
}

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string CueText(std::string_view cue) {
    std::vector<std::string_view> runs;
    CollectTextRuns(cue, runs, 0);
    std::string best;
    for (const std::string_view run : runs) {
        std::string text = RtfToText(run);
        // One line per visible line of the text box; blank lines carry nothing.
        std::string cleaned;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find('\n', start);
            const std::string line = Trim(std::string_view(text).substr(
                start, end == std::string::npos ? std::string::npos : end - start));
            if (!line.empty()) {
                if (!cleaned.empty()) cleaned.push_back('\n');
                cleaned += line;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (cleaned.size() > best.size()) best = std::move(cleaned);
    }
    return best;
}

// --- section naming (the provider-local helpers SongProviders.cpp keeps to itself) ---
std::string SectionLabel(std::string_view name) {
    std::string out;
    for (char c : name) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out.empty() ? std::string("VERSE") : out;
}

std::string SectionId(std::string_view name, size_t index) {
    std::string base;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) base.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!base.empty() && base.back() != '-') base.push_back('-');
    }
    if (base.empty()) base = "section";
    return base + "-" + std::to_string(index + 1);
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct CueEntry {
    std::string uuid;
    std::string name;
    std::string text;
};

struct CueGroup {
    std::string name;
    std::vector<std::string> cueIds;
};
} // namespace

Result<Song> ParseProPresenterFile(std::string_view content) {
    // A Presentation always carries its name (field 3); anything without one is
    // not a presentation document, whatever else it may be.
    const std::string_view title = FirstBytes(content, 3);
    if (!LooksLikeText(title))
        return Error::Make(Err::Song_UnsupportedFormat, kModule, "unsupported ProPresenter payload");

    std::vector<CueEntry> cues;
    std::vector<CueGroup> groups;
    std::string notes;
    std::string ccliAuthor;
    std::string ccliPublisher;
    std::string ccliYear;
    std::string ccliNumber;

    ForEachField(content, [&](const FieldRef& field) {
        if (field.bytes.empty()) return;
        if (field.number == 12) {
            CueGroup group;
            const std::string_view definition = FirstBytes(field.bytes, 1);
            const std::string_view name = FirstBytes(definition, 2);
            if (LooksLikeText(name)) group.name = std::string(name);
            ForEachField(field.bytes, [&](const FieldRef& child) {
                if (child.number == 2 && !child.bytes.empty()) {
                    const std::string id = UuidOf(child.bytes);
                    if (!id.empty()) group.cueIds.push_back(id);
                }
            });
            if (!group.name.empty() || !group.cueIds.empty()) groups.push_back(std::move(group));
            return;
        }
        if (field.number == 13) {
            CueEntry cue;
            ForEachField(field.bytes, [&](const FieldRef& child) {
                if (child.number == 1 && !child.bytes.empty() && cue.uuid.empty()) cue.uuid = UuidOf(child.bytes);
                else if (child.number == 2 && cue.name.empty() && LooksLikeText(child.bytes)) cue.name = std::string(child.bytes);
            });
            cue.text = CueText(field.bytes);
            cues.push_back(std::move(cue));
            return;
        }
        if (field.number == 7 && notes.empty() && LooksLikeText(field.bytes)) notes = std::string(field.bytes);
        if (field.number == 14) {
            const std::string_view author = FirstBytes(field.bytes, 1);
            const std::string_view publisher = FirstBytes(field.bytes, 4);
            const std::string_view album = FirstBytes(field.bytes, 8);
            if (ccliAuthor.empty() && LooksLikeText(author)) ccliAuthor = std::string(author);
            if (ccliPublisher.empty() && LooksLikeText(publisher)) ccliPublisher = std::string(publisher);
            if (ccliPublisher.empty() && LooksLikeText(album)) ccliPublisher = std::string(album);
            ForEachField(field.bytes, [&](const FieldRef& child) {
                if (ccliYear.empty() && child.number == 5 && child.value != 0) ccliYear = std::to_string(child.value);
                if (ccliNumber.empty() && child.number == 6 && child.value != 0) ccliNumber = std::to_string(child.value);
            });
        }
    });

    if (cues.empty())
        return Error::Make(Err::Song_ValidationFailed, kModule,
                           "the presentation holds no slides");

    Song song;
    song.metadata.title = std::string(title);
    if (!ccliAuthor.empty()) song.metadata.authors.push_back(ccliAuthor);
    if (!ccliPublisher.empty()) song.metadata.copyright = ccliPublisher;
    if (!ccliNumber.empty()) {
        song.metadata.ccli = ccliNumber;
        song.metadata.licensing.ccli = ccliNumber;
    }
    song.metadata.licensing.copyrightYear = ccliYear;
    song.metadata.licensing.copyrightHolder = ccliPublisher;
    song.metadata.licensing.source = "propresenter";
    song.metadata.notes = std::move(notes);

    // The pictures and videos the slides reference, so the import can bring the
    // deck's artwork along instead of leaving it behind on the old machine.
    std::vector<std::string> mediaPaths;
    CollectMediaPaths(content, mediaPaths, 0);
    for (std::string& path : mediaPaths)
        song.metadata.mediaRefs.push_back(MediaRef{"slide", std::move(path)});

    // One cue is one slide, so it becomes one section: a verse that spans two
    // slides stays two slides instead of two lines of a single one.
    std::vector<bool> used(cues.size(), false);
    auto addSection = [&](const std::string& label, const CueEntry& cue) {
        if (cue.text.empty()) return;   // a blank/placeholder slide has nothing to show
        SongSection section;
        section.name = SectionLabel(label.empty() ? std::string("VERSE") : label);
        section.id = SectionId(section.name, song.sections.size());
        size_t start = 0;
        while (start <= cue.text.size()) {
            const size_t end = cue.text.find('\n', start);
            SongLine line;
            line.lyrics = cue.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            section.lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        song.sections.push_back(std::move(section));
    };

    for (const CueGroup& group : groups) {
        for (const std::string& id : group.cueIds) {
            for (size_t i = 0; i < cues.size(); ++i) {
                if (used[i] || cues[i].uuid != id) continue;
                used[i] = true;
                addSection(group.name, cues[i]);
                break;
            }
        }
    }
    for (size_t i = 0; i < cues.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        addSection(cues[i].name, cues[i]);
    }

    if (song.sections.empty())
        return Error::Make(Err::Song_ValidationFailed, kModule,
                           "the presentation has no text on any slide");

    std::vector<std::string> order;
    order.reserve(song.sections.size());
    for (const SongSection& section : song.sections) order.push_back(section.id);
    Arrangement arrangement;
    arrangement.id = "original";
    arrangement.name = "Original";
    arrangement.sectionIds = std::move(order);
    song.arrangements.push_back(std::move(arrangement));

    song.createdMs = song.modifiedMs = NowMs();
    return song;
}
} // namespace bps::song