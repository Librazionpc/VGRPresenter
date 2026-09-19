#include "modules/songs/Chord.hpp"

#include <cctype>

namespace bps::song {

namespace {

// Chromatic scale with canonical sharp roots.
const char* const kSharpRoots[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};

// Alternative flat spelling per index (used when the source used flats).
const char* const kFlatRoots[12] = {"C", "Db", "D", "Eb", "E", "F",
                                    "Gb", "G", "Ab", "A", "Bb", "B"};

// Resolves a root name to its chromatic index; -1 when not a root.
int RootIndex(std::string_view root) {
    if (root.empty()) return -1;
    char c = root[0];
    if (c < 'A' || c > 'G') return -1;
    int idx = -1;
    switch (c) {
        case 'C': idx = 0; break;
        case 'D': idx = 2; break;
        case 'E': idx = 4; break;
        case 'F': idx = 5; break;
        case 'G': idx = 7; break;
        case 'A': idx = 9; break;
        case 'B': idx = 11; break;
    }
    if (root.size() >= 2) {
        if (root[1] == '#') ++idx;
        else if (root[1] == 'b') --idx;
    }
    // Reject a second accidental character (e.g. "C##") — not a root.
    if (root.size() >= 3 && (root[2] == '#' || root[2] == 'b')) return -1;
    idx = ((idx % 12) + 12) % 12;
    return idx;
}

} // namespace

std::string Chord::Display() const {
    if (!custom.empty()) return custom;
    std::string out = root;
    switch (quality) {
        case ChordQuality::Major:           break;
        case ChordQuality::Minor:           out += "m"; break;
        case ChordQuality::Diminished:      out += "dim"; break;
        case ChordQuality::Augmented:       out += "aug"; break;
        case ChordQuality::Suspended:       out += extension; break;   // "sus4"
        case ChordQuality::DominantSeventh: out += "7"; break;
        case ChordQuality::Seventh:         out += extension; break;   // "maj7"
        case ChordQuality::Extended:        out += extension; break;
        case ChordQuality::Custom:          break;
    }
    if (quality != ChordQuality::Suspended && quality != ChordQuality::Seventh &&
        quality != ChordQuality::Extended && !extension.empty()) {
        // Minor/diminished/augmented with a numeric extension: "m7", "dim7".
        out += extension;
    }
    if (!bass.empty()) {
        out += "/";
        out += bass;
    }
    return out;
}

Result<Chord> ChordSystem::Parse(std::string_view token) {
    std::string t;
    for (char c : token)
        if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
    if (t.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine", "empty chord token");

    // Strip inline brackets: "[C]" or "[D/F#]".
    if (t.front() == '[' && t.back() == ']') t = t.substr(1, t.size() - 2);

    Chord chord;
    std::string chordPart = t;
    size_t slash = t.find('/');
    if (slash != std::string::npos) {
        chordPart = t.substr(0, slash);
        chord.bass = t.substr(slash + 1);
        // Normalize the bass root spelling.
        int bassIdx = RootIndex(chord.bass);
        if (bassIdx >= 0) chord.bass = kSharpRoots[bassIdx];
    }
    if (chordPart.empty()) {
        chord.custom = t;   // "/F#" alone is not a chord
        return chord;
    }

    char c = chordPart[0];
    if (c < 'A' || c > 'G') {
        chord.custom = t;
        return chord;
    }
    chord.root = std::string(1, c);
    size_t i = 1;
    if (i < chordPart.size() && (chordPart[i] == '#' || chordPart[i] == 'b')) {
        chord.root += chordPart[i];
        ++i;
    }
    if (i < chordPart.size() && (chordPart[i] == '#' || chordPart[i] == 'b')) {
        chord.custom = t;   // double accidental — preserve as-is
        return chord;
    }
    std::string suffix = chordPart.substr(i);
    chord.extension = suffix;

    if (suffix.empty()) {
        chord.quality = ChordQuality::Major;
    } else if (suffix == "m") {
        chord.quality = ChordQuality::Minor;
        chord.extension.clear();
    } else if (suffix.starts_with("maj")) {
        chord.quality = ChordQuality::Seventh;      // "maj7", "maj9", ...
    } else if (suffix[0] == 'm') {
        chord.quality = ChordQuality::Minor;        // "m7", "m9", "m7b5", ...
        chord.extension = suffix.substr(1);
    } else if (suffix.starts_with("dim")) {
        chord.quality = ChordQuality::Diminished;   // "dim", "dim7"
        chord.extension = suffix.substr(3);
    } else if (suffix.starts_with("aug")) {
        chord.quality = ChordQuality::Augmented;    // "aug", "aug7"
        chord.extension = suffix.substr(3);
    } else if (suffix.starts_with("sus")) {
        chord.quality = ChordQuality::Suspended;    // "sus2", "sus4"
    } else if (suffix == "7") {
        chord.quality = ChordQuality::DominantSeventh;
        chord.extension.clear();
    } else if (suffix[0] == '7' && suffix.size() > 1) {
        chord.quality = ChordQuality::DominantSeventh;   // "7b5", "7#9", ...
        chord.extension = suffix.substr(1);
    } else if (suffix == "6") {
        chord.quality = ChordQuality::Major;
        chord.extension = "6";
    } else if (suffix == "9" || suffix == "11" || suffix == "13" ||
               suffix.starts_with("add")) {
        chord.quality = ChordQuality::Extended;
    } else {
        chord.quality = ChordQuality::Custom;
        chord.custom = t;   // unrecognized notation — preserve verbatim
        return chord;
    }
    return chord;
}

Result<Chord> ChordSystem::Transpose(const Chord& chord, int semitones) {
    if (chord.custom.empty() && chord.root.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine", "empty chord");
    Chord out = chord;
    if (!chord.custom.empty()) return out;   // unknown notation stays untouched

    int idx = RootIndex(chord.root);
    if (idx < 0) return out;
    int target = ((idx + semitones) % 12 + 12) % 12;
    bool usedFlat = chord.root.size() >= 2 && chord.root[1] == 'b';
    out.root = usedFlat ? kFlatRoots[target] : kSharpRoots[target];

    if (!chord.bass.empty()) {
        int bIdx = RootIndex(chord.bass);
        if (bIdx >= 0) {
            int tb = ((bIdx + semitones) % 12 + 12) % 12;
            bool flatBass = chord.bass.size() >= 2 && chord.bass[1] == 'b';
            out.bass = flatBass ? kFlatRoots[tb] : kSharpRoots[tb];
        }
    }
    return out;
}

Result<Chord> ChordSystem::ParseAndTranspose(std::string_view token, int semitones) {
    auto chord = Parse(token);
    if (!chord.ok()) return chord.error();
    return Transpose(chord.value(), semitones);
}

Result<std::string> ChordSystem::NormalizeRoot(std::string_view root) {
    auto chord = Parse(root);
    if (!chord.ok()) return chord.error();
    if (!chord.value().custom.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine",
                           "not a recognized chord/root: " + std::string(root));
    return chord.value().root;
}

Result<std::string> ChordSystem::TransposeKey(std::string_view key, int semitones) {
    auto chord = Parse(key);
    if (!chord.ok()) return chord.error();
    if (!chord.value().custom.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine",
                           "not a recognized key: " + std::string(key));
    auto transposed = Transpose(chord.value(), semitones);
    if (!transposed.ok()) return transposed.error();
    // A key is either the root or root+"m" (minor key).
    return transposed.value().root +
           (transposed.value().quality == ChordQuality::Minor ? "m" : "");
}

std::vector<std::string> ChordSystem::AllRoots() {
    std::vector<std::string> out;
    for (const auto* r : kSharpRoots) out.push_back(r);
    return out;
}

bool ChordSystem::IsChordToken(std::string_view token) {
    auto r = Parse(token);
    return r.ok() && r.value().custom.empty();   // unrecognized notation is not a chord
}

} // namespace bps::song
