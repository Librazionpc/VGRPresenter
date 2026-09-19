#pragma once

// Chord System (docs/specs/25 §Chord System / §Transposition). Chords are
// structured data — root, quality, extension, slash bass — never random text.
// The parser recognizes G, Am, D/F#, Cmaj7, Em7, G/B, C#m7b5 and friends;
// unrecognized notation is preserved as `custom` so nothing is ever lost.
// Transposition maps every chord through a chromatic table, preserves
// qualities/extensions/slash basses, and keeps the original spelling family
// (sharps stay sharps, flats stay flats).

#include "core/common/Common.hpp"
#include "modules/songs/SongTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::song {

class ChordSystem {
public:
    // Parses a single chord token ("G", "Am", "D/F#", "Cmaj7", "Em7", "G/B").
    static Result<Chord> Parse(std::string_view token);

    // Transposes a chord by `semitones` (-12..12).
    static Result<Chord> Transpose(const Chord& chord, int semitones);

    // Parses + transposes a token in one step.
    static Result<Chord> ParseAndTranspose(std::string_view token, int semitones);

    // Transposes a key name ("C" -> "D" at +2).
    static Result<std::string> TransposeKey(std::string_view key, int semitones);

    // Normalizes a key/root name to its canonical spelling.
    static Result<std::string> NormalizeRoot(std::string_view root);

    // All 12 canonical roots, starting at C.
    static std::vector<std::string> AllRoots();

    // Whether a token looks like a chord (used by plain-text/chord-line
    // detection): every whitespace-separated token parses as a chord.
    static bool IsChordToken(std::string_view token);
};

} // namespace bps::song
