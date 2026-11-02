#pragma once

#include "core/common/Common.hpp"
#include "modules/bible/BibleTypes.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::bible {
class IBibleProvider {
public:
    virtual ~IBibleProvider() = default;

    // Stable provider name, e.g. "zefania-xml".
    virtual const char* Name() const noexcept = 0;

    // Format tag matched during import ("xml", "json", "usfm", "osis", "txt").
    virtual const char* Format() const noexcept = 0;

    // File extensions this provider can consume (".xml", ".usfm", ...).
    virtual std::vector<std::string> SupportedExtensions() const = 0;

    virtual Result<BibleVersion> Parse(std::string_view source,
                                       std::string_view format) const = 0;
};

std::shared_ptr<IBibleProvider> CreateXmlBibleProvider();

// JSON: {"metadata": {...}, "books": [...], "verses": [{book, chapter, verse,
// text, heading, redLetter, footnotes, crossRefs}]}.
std::shared_ptr<IBibleProvider> CreateJsonBibleProvider();

// USFM: \id GEN, \h, \c 1, \v 1 ..., \s headings, \f footnotes.
std::shared_ptr<IBibleProvider> CreateUsfmBibleProvider();

// OSIS: <osis><osisText><div type="book"><chapter><verse>...
std::shared_ptr<IBibleProvider> CreateOsisBibleProvider();

// Plain text: "GEN 1:1 In the beginning..." lines.
std::shared_ptr<IBibleProvider> CreatePlainTextBibleProvider();
} // namespace bps::bible