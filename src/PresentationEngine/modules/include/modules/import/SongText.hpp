#pragma once

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

std::vector<std::string> AutoGroupSections(std::vector<std::string>& sections, bool autoGroups = true);
} // namespace bps::import