#pragma once

#include "core/common/Common.hpp"
#include "modules/songs/SongTypes.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::song {
class ISongProvider {
public:
    virtual ~ISongProvider() = default;

    // Stable provider name, e.g. "chordpro".
    virtual const char* Name() const noexcept = 0;

    // Format tag matched during import ("chordpro", "opensong", "openlp", ...).
    virtual const char* Format() const noexcept = 0;

    // File extensions this provider can consume (".cho", ".opn", ...).
    virtual std::vector<std::string> SupportedExtensions() const = 0;

    // Parses `source` into the canonical Song model. Returns a typed error on
    // malformed input — never a partial song.
    virtual Result<Song> Parse(std::string_view source, std::string_view format) const = 0;
};

std::shared_ptr<ISongProvider> CreateChordProProvider();

// OpenSong: <song><title><author><ccli><key><lyrics><verse name><lines>.
std::shared_ptr<ISongProvider> CreateOpenSongProvider();

// OpenLP: <song><properties><titles><authors><lyrics><verse name type><lines>.
std::shared_ptr<ISongProvider> CreateOpenLpProvider();

// ProPresenter: <rvb><song><title><properties><titles><lyrics><verse><lines>.
std::shared_ptr<ISongProvider> CreateProPresenterProvider();

// EasyWorship: <song><title> + repeated stanza/verse blocks of lines.
std::shared_ptr<ISongProvider> CreateEasyWorshipProvider();

// Plain text: "[Verse 1]" / "VERSE 1:" section headers, blank-line breaks.
std::shared_ptr<ISongProvider> CreatePlainTextSongProvider();

// JSON: the canonical Song shape.
std::shared_ptr<ISongProvider> CreateJsonSongProvider();
} // namespace bps::song