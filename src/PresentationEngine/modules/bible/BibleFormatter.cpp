#include "modules/bible/BibleFormatter.hpp"

#include <format>
#include <sstream>

namespace bps::bible {

Result<std::string> BibleFormatter::Format(const BibleVersion& bible, const PassageRef& ref,
                                           const FormatOptions& opts) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");

    std::vector<BibleVerse> verses;
    if (ref.IsWholeBook()) {
        for (const auto& [key, ch] : bible.chapters)
            if (key.starts_with(ref.bookId + "."))
                verses.insert(verses.end(), ch.verses.begin(), ch.verses.end());
    } else {
        auto chIt = bible.chapters.find(std::format("{}.{}", ref.bookId, ref.chapter));
        if (chIt == bible.chapters.end())
            return Error::Make(Err::Bible_NotFound, "BibleEngine",
                               "chapter not found: " + ref.ToString());
        if (ref.IsWholeChapter()) {
            verses = chIt->second.verses;
        } else {
            for (const auto& v : chIt->second.verses)
                if (v.verse >= ref.verseStart && v.verse <= ref.verseEnd)
                    verses.push_back(v);
        }
    }
    if (verses.empty())
        return Error::Make(Err::Bible_NotFound, "BibleEngine",
                           "passage not found: " + ref.ToString());

    std::ostringstream out;
    std::string activeHeading;
    int currentChapter = 0;
    for (const auto& v : verses) {
        if (opts.includeHeadings && currentChapter != v.chapter) {
            currentChapter = v.chapter;
            auto chIt = bible.chapters.find(std::format("{}.{}", v.bookId, v.chapter));
            if (chIt != bible.chapters.end() && !chIt->second.title.empty())
                out << chIt->second.title << "\n";
        }
        if (opts.includeHeadings && !v.heading.empty() && v.heading != activeHeading) {
            activeHeading = v.heading;
            if (opts.mode == FormatOptions::Mode::Paragraph) {
                if (out.tellp() > 0) out << "\n";
                out << v.heading << "\n";
            } else {
                out << v.heading << "\n";
            }
        }
        std::string text = v.text;
        if (opts.includeFootnotes) {
            for (const auto& f : v.footnotes) text += std::format(" [{}: {}]", f.marker, f.text);
        }
        if (opts.redLetterMarkers && v.redLetter) text = std::format("<red>{}</red>", text);

        if (opts.mode == FormatOptions::Mode::VersePerLine) {
            if (opts.includeNumbers)
                out << v.verse << ". ";
            out << text << "\n";
        } else {
            if (opts.includeNumbers && verses.size() > 1)
                out << "[" << v.verse << "] ";
            out << text;
            if (&v != &verses.back()) out << " ";
        }
    }
    return out.str();
}

} // namespace bps::bible
