#pragma once

// Song Engine canonical model (Phase 13, docs/specs/25). A song is structured
// musical content: metadata, sections, lyrics, chords, arrangements, notes,
// licensing, and media references. Every external format (ChordPro, OpenSong,
// OpenLP, ...) is converted into these types by a provider — the rest of the
// platform never sees interchange formats.

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::song {

// --- Chord (docs/specs/25 §Chord System) --------------------------------------
enum class ChordQuality : int {
    Major = 0,
    Minor,
    Diminished,
    Augmented,
    Suspended,
    DominantSeventh,
    Seventh,
    Extended,
    Custom,
};

struct Chord {
    std::string root;          // "C", "C#", "Db", ...
    ChordQuality quality = ChordQuality::Major;
    std::string extension;     // "7", "maj7", "sus4", "m7b5", "" (extension text)
    std::string bass;          // slash bass root ("" = none) — "D/F#" -> "F#"
    std::string custom;        // preserved when the notation is unrecognized

    bool operator==(const Chord& o) const {
        return root == o.root && quality == o.quality && extension == o.extension &&
               bass == o.bass && custom == o.custom;
    }
    bool operator!=(const Chord& o) const { return !(*this == o); }

    // Canonical text form: "Cmaj7", "D/F#", "Am7".
    std::string Display() const;
};

// A chord anchored at a column within a lyric line (chord-above-lyrics).
struct ChordRef {
    size_t column = 0;
    Chord chord;
};

// --- Song content --------------------------------------------------------------
struct SongLine {
    std::vector<ChordRef> chords;   // chord positions (structured)
    std::string lyrics;             // line text
};

struct SongSection {
    std::string id;                 // stable ("v1", "chorus", ...)
    std::string name;               // "VERSE 1", "CHORUS", "BRIDGE", custom
    std::vector<SongLine> lines;
};

// --- Licensing (docs/specs/25 §Copyright & Licensing) ---------------------------
struct LicensingInfo {
    std::string copyrightHolder;
    std::string copyrightYear;
    std::string license;
    std::string ccli;
    std::string source;
    std::string restrictions;
};

// --- Media references (ids only — never duplicated content) ---------------------
struct MediaRef {
    std::string kind;      // "background", "audio", "artwork", ...
    std::string mediaId;   // reference to a Media Engine asset id
};

struct SongMetadata {
    std::string title;
    std::vector<std::string> authors;
    std::string copyright;
    std::string ccli;
    std::string language;
    std::string originalKey;      // "C", "D#m", ...
    std::string preferredKey;
    std::string performanceKey;   // current performance key
    std::string tempo;            // "72 bpm"
    std::string timeSignature;    // "4/4"
    std::vector<std::string> tags;
    std::string notes;
    LicensingInfo licensing;
    std::vector<MediaRef> mediaRefs;
};

// --- Arrangement (docs/specs/25 §Arrangements) -----------------------------------
// An independent ordered list of section ids referencing the song's sections.
// The first arrangement in a song is the original order.
struct Arrangement {
    std::string id;                // "original", "sunday", "short", ...
    std::string name;
    std::vector<std::string> sectionIds;
};

// --- Song (docs/specs/25 §Canonical Song Model) ------------------------------------
struct Song {
    std::string id;                // stable — never the filename
    SongMetadata metadata;
    std::vector<SongSection> sections;
    std::vector<Arrangement> arrangements;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
};

// --- Versioning & recovery (docs/specs/25 §Versioning & Recovery) -------------------
struct SongVersion {
    std::string id;                // version id
    int64_t createdMs = 0;
    Song snapshot;
};

// --- Duplicate detection (docs/specs/25 §Duplicate Detection) ------------------------
struct DuplicateGroup {
    std::vector<std::string> songIds;
    double similarity = 0.0;       // 0..1 content similarity
};

// --- Import options --------------------------------------------------------------------
struct ImportOptions {
    std::string id;                // override song id
    bool index = true;             // auto-index into the Search Engine
    bool checkDuplicates = true;   // report likely duplicates on import
};

} // namespace bps::song
