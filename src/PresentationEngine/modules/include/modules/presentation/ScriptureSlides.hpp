#pragma once

// Scripture on a slide, the way FreeShow does it (components/drawer/bible/scripture.ts): a TEMPLATE whose text carries
// scripture placeholders is filled with the verses the user picked, and the verses are shared out over as many slides as they
// need. The ENGINE owns all of it - the placeholders, how a reference is written ("Genesis 1:1-3, 5"), how long verses are
// divided, how many verses a slide takes - so the Scripture tab, the live output and "Convert to show" all show the same thing.
//
// Placeholders a template's text may contain (N = 1..4 picks the Nth Bible of a parallel set; only the first exists here, the
// others are blank):
//   {scripture_text}       the verses, each led by its number (unless numbers are off)      {scriptureN_text}
//   {scripture_number}     a verse number kept only as a style marker - removed, the number is put in front of its verse
//   {scripture_reference}  "Genesis 1:1-3"          {scripture_reference_full} the whole selection   {scripture_reference_last} the
//                          whole selection, on the LAST slide only
//   {scripture_verses}     "1-3"        {scripture_book} "Genesis"   {scripture_book_abbr} "GEN"   {scripture_chapter} "1"
//   {scripture_name}       the version's name, without a bracketed part   {meta_copyright}   the version's copyright
//   {scripture_red_jesus}, {scripture_undertitle}: style markers, removed
// A template with none of them is "old style": its `text` and `ref` bound blocks get the verses and the reference.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <vector>

namespace bps::presentation {

struct ScriptureVerse {
    int number = 0;
    std::string text;
};

// What the user picked.
struct ScriptureSource {
    std::string versionName;     // "King James Version (KJV)"
    std::string copyright;
    std::string book;            // "Genesis"
    std::string bookAbbr;        // "GEN"
    int chapter = 0;
    std::vector<ScriptureVerse> verses;   // the selected verses, ascending
};

struct ScriptureSettings {
    bool verseNumbers = true;
    bool versesOnIndividualLines = false;
    bool splitLongVerses = false;        // divide a verse longer than longVersesChars over several slides
    bool splitLongVersesSuffix = false;  // ...and mark the parts 1a, 1b
    int longVersesChars = 100;
    int longVersesTolerance = 0;         // percent past the limit a cut may wait for a word end
    bool smartSplit = true;              // as many verses to a slide as the template's text box holds
    int versesPerSlide = 3;              // (smartSplit off)
};

struct ScriptureSlide {
    std::vector<ContentBlock> blocks;    // the template's blocks, filled in
    std::string reference;               // "Genesis 1:1-3" - this slide's verses
    std::string title;                   // the same, for a slide list
};

// Does the template use the scripture placeholders? (If not it is an old-style template.)
bool HasScriptureValues(const std::vector<ContentBlock>& templateBlocks);

// "Genesis 1:1-3, 5" for the given verses of one chapter; "Genesis 1" when `verses` is empty (the whole chapter).
std::string ScriptureReference(const std::string& book, int chapter, const std::vector<int>& verses);
// "1-3, 5" - the verse part alone.
std::string ScriptureVerseRange(const std::vector<int>& verses);

// Every slide the selection needs, or just the first when `onlyFirst` (a preview). An empty selection gives no slides.
std::vector<ScriptureSlide> BuildScriptureSlides(const std::vector<ContentBlock>& templateBlocks, const ScriptureSource& source,
                                                 const ScriptureSettings& settings, bool onlyFirst = false);

} // namespace bps::presentation
