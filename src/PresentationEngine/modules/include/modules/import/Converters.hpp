#pragma once

// The song and document formats FreeShow's Import reads, ported into the engine. Each function turns ONE file into what it holds and
// knows nothing of the UI, the library or the disk; ImportFiles() (ImportEngine.hpp) picks the right one by the format the user chose.
//
//   VideoPsalm    .json / .vpc   a songbook whose keys are not quoted (its own not-quite-JSON)
//   Quelea        .xml / .qsp    one song's XML, or a song pack (a zip of them)
//   VerseVIEW     .xml           a song database, the slides in CDATA
//   Songbeamer    (no extension) "#Key=value" header, sections split by "---", verse order, base64 comments
//   Lessons.church .json         a lesson: its sections and the media each plays (the media itself is not downloaded)
//   PowerPoint    .pptx          the words on each slide (and its speaker notes)
//   Word          .docx          the words, a blank line starting a new slide
//   Calendar      .ics           events

#include "modules/import/ImportTypes.hpp"

#include <string>
#include <vector>

namespace bps::import {

Result<std::vector<ImportedShow>> ParseVideoPsalm(const ImportFile& file);
Result<std::vector<ImportedShow>> ParseQuelea(const ImportFile& file);
Result<std::vector<ImportedShow>> ParseVerseView(const ImportFile& file);
Result<ImportedShow> ParseSongbeamer(const ImportFile& file);
Result<std::vector<ImportedShow>> ParseLessonsChurch(const ImportFile& file);
Result<ImportedShow> ParsePowerPoint(const ImportFile& file);
Result<ImportedShow> ParseWord(const ImportFile& file);
Result<std::vector<ImportedEvent>> ParseIcs(const std::string& text);

// The JSON these files are written in when it is not quite JSON: keys without quotes, a trailing comma, raw line breaks inside strings.
Result<json::Value> ParseRelaxedJson(std::string_view text);

} // namespace bps::import
