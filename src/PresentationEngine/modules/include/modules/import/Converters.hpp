#pragma once

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