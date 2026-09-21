#pragma once

// The import engine: the list of every format (ImportFormats()), and turning files of a format into shows.
//
//   ImportFormats()                         what the Import dialog draws - sections, order, extensions, what each is for
//   ImportFiles(formatId, files)            files -> ImportedShows (or installed Bibles); nothing is written to disk
//   ShowFromImported(show, template...)     an ImportedShow -> a presentation::Presentation, ready to save as a .vgr
//   ShowFromClipboardText(text)             "Paste from clipboard": the plain-text converter
//
// A format whose converter is not written yet stays in the list with available = false, so the dialog can say so instead of
// silently missing it.

#include "modules/import/ImportTypes.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::import {

const std::vector<ImportFormat>& ImportFormats();
const ImportFormat* FindImportFormat(std::string_view id);

// Every format that can read a file with this extension ("txt" -> txt, chordpro...), most specific first.
std::vector<const ImportFormat*> FormatsForExtension(std::string_view extension);

struct ImportOptions {
    int splitLines = 0;           // text formats: at most this many lines to a slide (0 = as written)
    bool noFormatting = true;     // text formats: keep the words exactly as written
};

// Reads the files of one format. A file that cannot be read is a warning; the others still import. An unknown or
// not-yet-available format is an error.
Result<ImportResult> ImportFiles(std::string_view formatId, const std::vector<ImportFile>& files, const ImportOptions& options = {});

// ---- to a show ----
struct ShowBuildOptions {
    // The blocks of the template the slides look like (a song template). Its block bound "text" shows the slide's words, "title" its
    // group's label, "notes" its notes; every other block is copied as it is. Empty = one plain centred text box.
    std::vector<presentation::ContentBlock> templateBlocks;
    std::string background = "transparent";
    std::string categoryName = "Songs";   // the show category the slides go in (its content type is the show's category)
};

presentation::Presentation ShowFromImported(const ImportedShow& show, const ShowBuildOptions& options = {});

// The plain-text converter, for text pasted or typed in: one show.
ImportedShow ShowFromClipboardText(const std::string& text, const ImportOptions& options = {});

} // namespace bps::import
