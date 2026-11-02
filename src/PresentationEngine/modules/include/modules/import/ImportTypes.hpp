#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::import {
// What a format produces.
enum class ImportKind : int {
    Show = 0,       // songs / presentations -> shows
    Bible,          // scripture -> installed Bibles
    Project,        // a FreeShow project: a list of shows
    Template,       // slide templates
    Calendar,       // calendar events
    Other,          // actions, stage layouts, themes: things this app has no counterpart for (yet)
};

const char* ToString(ImportKind kind);

struct ImportFormat {
    std::string id;                       // "txt", "chordpro", "videopsalm"...
    std::string name;                     // "Text file"
    std::string description;              // a line about it (may be empty)
    std::vector<std::string> extensions;  // lower case, without the dot: { "txt" }
    ImportKind kind = ImportKind::Show;
    bool available = true;                // false: known, but its converter is not written yet (the dialog says so)
    // Where the Import dialog puts it (FreeShow's Import popup): "freeshow" (its own files), "media" (Lessons.church, PDF,
    // PowerPoint), "text" (song and lyric formats), "bible" (scripture), "calendar".
    std::string section = "text";
    bool primary = false;                 // shown up front in its section; the rest are behind "More options"
    std::string tutorial;                 // a note shown before the file picker (where to find the file)
    std::string icon;                     // an icon name from the app's icon set
};

// One file handed to a converter. `content` is the file's bytes (text formats are UTF-8 or convertible to it).
struct ImportFile {
    std::string name;        // file name without extension ("Amazing Grace")
    std::string extension;   // lower case, without the dot
    std::string path;        // where it came from (may be empty)
    std::string content;
};

// ---- the neutral show a converter produces ------------------------------------------------------------------------------

struct ImportedSlide {
    std::string text;         // lines separated by "\n"
    std::string notes;
    std::string chordsJson;   // "[[{\"pos\":0,\"key\":\"C\"}], ...]" one entry per line, or "" (no chords)
    // FreeShow slides may contain several independently positioned text items.
    // Other import formats leave this empty and use the ordinary single text box.
    std::vector<presentation::ContentBlock> blocks;
};

// A GROUP of the show (Verse, Chorus...): its first slide and any child slides that follow it.
struct ImportedSection {
    std::string group;        // the group type: "verse" | "chorus" | "pre_chorus" | "bridge" | "intro" | "outro" | "tag" | "break" | a custom name
    std::string label;        // how the slide list names it ("Verse", "Chorus", or the file's own heading)
    std::vector<ImportedSlide> slides;
};

struct ImportedShow {
    std::string name;
    std::string category;                       // the kind of show: "song" | "notes" | "scripture"...
    std::map<std::string, std::string> meta;    // title, author, CCLI, copyright, key...
    std::string notes;
    std::vector<ImportedSection> sections;      // in the order they are sung / shown (a repeated chorus appears again)
    std::string origin;                         // the format it came from ("txt", "chordpro"...)
    std::string sourceFolder;
    std::vector<std::string> mediaRefs;
};

struct ImportResult {
    std::vector<ImportedShow> shows;
    std::vector<std::string> bibles;            // ids of Bibles installed by this import
    std::vector<std::string> warnings;          // a file that could not be read, and why
    size_t files = 0;                           // how many files were handed in
};
} // namespace bps::import