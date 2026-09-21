#pragma once

// Plain lyrics to a show - FreeShow's text converter (converters/txt.ts), the one every text-ish song format and the
// "paste from clipboard" import lean on:
//
//   [Verse 1] / "Verse 1:" / "Chorus x2"   group headers (brackets or a trailing colon, a repeat count x2..x9)
//   blank line                              a new slide
//   lines with chords over them             folded into [C]inline[G] chord markers, kept with the slide
//   Title=... / Author=... / CCLI=... / notes=...   metadata lines (any key; unknown keys are kept as they are)
//   "---" inside a slide                    what follows is that slide's notes
//   a trailing "www.ccli.com" block         SongSelect's copyright footer, read as metadata
//   sections with no header                 recognised by how alike they are: a section that repeats is a chorus / bridge,
//                                           short or repetitive ones are tags, the rest are verses
//
// Only the ENGINE reads lyrics this way, so the Import dialog, the clipboard import and every format built on it agree.

#include "modules/import/ImportTypes.hpp"

#include <string>

namespace bps::import {

struct SongTextOptions {
    std::string name;             // the show's name; empty = from the Title metadata, else from the first lines
    std::string category = "song";
    bool noFormatting = true;     // false: tidy the lines (capital first letter, no trailing punctuation, ":/:" repeats, comma breaks)
    bool autoGroups = true;       // false: a section with no header is a plain verse instead of being guessed
    int splitLines = 0;           // > 0: a slide never has more lines than this (the rest go to child slides)
};

ImportedShow ParseSongText(const std::string& text, const SongTextOptions& options = {});

// ---- pieces the other converters share ----
// "Verse 1" -> "verse", "Pre-chorus" -> "pre_chorus", "" when the label is not one of the standard groups.
std::string FindGroupMatch(const std::string& label);
// The display label of a standard group id ("pre_chorus" -> "Pre-Chorus"); a custom group is returned as it is.
std::string GroupLabel(const std::string& group);
// 0..1 how alike two strings are (FreeShow's edit-distance measure).
double TextSimilarity(const std::string& a, const std::string& b);

} // namespace bps::import
