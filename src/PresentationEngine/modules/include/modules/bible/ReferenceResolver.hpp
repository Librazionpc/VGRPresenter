#pragma once

#include "core/common/Common.hpp"
#include "modules/bible/BibleTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::bible {
class ReferenceResolver {
public:
    static Result<PassageRef> Resolve(std::string_view text,
                                      const std::vector<BibleBook>& books);

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