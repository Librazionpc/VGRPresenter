#pragma once

// Reference Resolver (docs/specs/24 §Reference Resolution). Parses and
// normalizes Scripture references such as "John 3:16", "John 3", "Psalm 23",
// "Genesis 1:1-10", "1 Corinthians 13", "John 3:16-18", and whole books, and
// expands them into explicit PassageRef values. Book names resolve through the
// target Bible's book table (name + aliases) with a built-in canonical
// abbreviation fallback so any common spelling resolves.

#include "core/common/Common.hpp"
#include "modules/bible/BibleTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::bible {

class ReferenceResolver {
public:
    // Resolves a single reference ("John 3:16") against `books`.
    // Returns Bible_InvalidReference when the text cannot be parsed or the
    // book is unknown.
    static Result<PassageRef> Resolve(std::string_view text,
                                      const std::vector<BibleBook>& books);

    // Resolves a comma/semicolon-separated list of references. Unknown items
    // are skipped (so "John 3:16, Psalm 23" resolves both) unless nothing
    // resolves at all.
    static Result<std::vector<PassageRef>> ResolveAll(
        std::string_view text, const std::vector<BibleBook>& books);

    // Lowercases, trims, and collapses whitespace.
    static std::string Normalize(std::string_view text);

    // Canonical abbreviation table for all 66 books, used as a fallback when a
    // Bible's book table omits an alias ("Jn", "1Co", "Ps", ...).
    static const char* CanonicalAbbreviation(std::string_view bookId);

private:
    // Parses a normalized reference body after the book name was consumed.
    static Result<PassageRef> ParseBody(const std::string& normalized,
                                        const std::string& raw,
                                        const BibleBook& book);
};

} // namespace bps::bible
