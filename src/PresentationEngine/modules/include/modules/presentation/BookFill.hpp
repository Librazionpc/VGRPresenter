#pragma once

// FILLING A TEMPLATE FROM A BOOK — the SHARED filler behind BOTH content tabs.
//
// A template's text carries placeholders; this module replaces them with the
// selection the user made and shares that selection out over as many slides as
// it needs. The selection is always the same shape — a BOOK, a chapter/sermon
// within it, and the numbered items the user picked inside that (verses of a
// Bible book, paragraphs of a Table year book) — which is why this file is named
// after the book and not after either tab: the Scripture tab, The Table tab, the
// live output and "Convert to show" are all THIS code, so a rule written once
// here is a rule no two surfaces can disagree about. What differs per tab is
// only its VOCABULARY, and that lives in the placeholders (below): the scripture_
// family for a Bible, the table_ family for a sermon.
//
// Placeholders a template's text may contain (N = 1..4 picks the Nth Bible of a parallel set; only the first exists here, the
// others are blank):
//   {scripture_text}       the verses, each led by its number (unless numbers are off)      {scriptureN_text}
//                          (N = 2..4 shows ANOTHER VERSION's verses of the same passage,
//                          when the tab has parallel versions selected — see ParallelBible)
//   {scripture_number}     a verse number kept only as a style marker - removed, the number is put in front of its verse
//   {scripture_reference}  "Genesis 1:1-3"          {scripture_reference_full} the whole selection   {scripture_reference_last} the
//                          whole selection, on the LAST slide only
//   {scripture_verses}     "1-3"        {scripture_book} "Genesis"   {scripture_book_abbr} "GEN"   {scripture_chapter} "1"
//   {scripture_name}       the version's name, without a bracketed part   {meta_copyright}   the version's copyright
//
// THE TABLE'S OWN FAMILY. A sermon is a citation line plus a run of PARAGRAPHS, and it has no chapter, no Bible version and
// no verse numbers, so borrowing the Bible's vocabulary meant a Table template had to translate ("verses" -> paragraphs,
// "book" -> the whole citation) and had nowhere to put the sermon's own title. Each table_ token names ONE part of the
// citation, so a design can place any combination of them in its own slots:
//   {table_text}           the paragraphs, each led by its number            {table_number}  the number slot (style marker)
//   {table_paragraphs}     "1-3"                                             {table_reference}      what this slide shows
//   {table_citation}       the whole citation line ("47-0412 - Faith Is The Substance")
//   {table_code}           the date code alone ("47-0412")                   {table_year}    the year alone ("1947")
//   {table_code_paragraphs}  the code and this slide's paragraphs together ("47-0412 1-3") — a printed reference's two parts
//   {table_title}          the sermon's title alone ("Faith Is The Substance")
//   {table_reference_full} / {table_reference_last}   the whole selection, on every / on the last slide
// The scripture_ family still RESOLVES for The Table, so every saved sermon template keeps working; the scripture-Named parts
// that a sermon has no answer for ({scripture_name}, {scripture_book_abbr}, {scripture_chapter}) fill blank there, exactly as
// they always did — and they are blank because the FAMILY has no answer for them (BookValues::Answers), not because the caller
// happened to leave three fields empty.
//   {scripture_red_jesus}, {scripture_undertitle}: style markers, removed
// A template with none of them is "old style": its `text` and `ref` bound blocks get the verses and the reference.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <vector>

namespace bps::presentation {

struct BookItem {
    int number = 0;
    std::string text;
};

struct BookPart {
    int number = 0;
    int partIndex = 0;          // zero-based; continuations display as 2-1, 2-2, etc.
    std::string text;
    bool continuation = false;
};

// ONE ADDITIONAL VERSION OF THE SAME PASSAGE — what a PARALLEL layout shows beside
// the selected Bible ({scripture2_text}, {scripture3_text}, {scripture4_text}).
//
// The items are that version's OWN verses for the passage, so a column is that
// translation's wording; the fill numbers, splits and groups them exactly as it
// does the selected Bible's, so the columns line up verse for verse on every
// slide. The layouts already ship ("Scripture 2", "Scripture 3", "Scripture 4",
// "Scripture Lower Third 2"); until now nothing could fill them, because the tab
// selected exactly one Bible.
struct ParallelBible {
    std::string name;            // "American Standard Version (ASV)" — may be empty
    std::string bookAbbr;        // that version's own abbreviation of the book, when it has one
    std::vector<BookItem> items; // its verses, by the same numbers as the selected Bible's
};

// WHICH CONTENT FAMILY a selection is.
//
// The filler serves BOTH content tabs, and their differences are exactly three:
// how a reference joins (a Bible reads "Genesis 1:1-3"; a citation reads
// "47-0412 - Faith Is The Substance 1-3" — no colon, no chapter), whether there
// is a chapter at all, and which parts of the vocabulary a family has an answer
// for (a sermon has no Bible version, no book abbreviation and no chapter).
//
// Every one of those used to be INFERRED — from `chapter > 0`, or from which
// fields the caller happened to fill — which made "chapter zero" a hidden
// synonym for "a sermon" and let two places read the same slide two ways (the
// code/text mix-up this header's BookSlide::body note describes was one
// symptom). The family is now NAMED, and every branch reads the name.
//
// The string tags the frontend uses ("scripture" | "table", SlideRules::
// ContentType) are this enum's spelling in a slide's meta; Tag()/FromTag() are
// the one conversion.
enum class ContentFamily { Scripture, Table };

// The family's tag, exactly as a slide's meta carries it (SlideRules::ContentType).
inline const char* Tag(ContentFamily family) {
    return family == ContentFamily::Table ? "table" : "scripture";
}

// A tag back to its family. An empty or unrecognised tag is SHOWS content
// (SlideRules' rule: an untagged slide follows the shows slot), and this is the
// one place that decides what a tag means.
inline ContentFamily FromTag(const std::string& tag) {
    return tag == "table" ? ContentFamily::Table : ContentFamily::Scripture;
}

// What the user picked.
struct BookSource {
    // WHICH TAB this came from (see ContentFamily above). Defaulted so a caller
    // that only has Bible-shaped data cannot produce a sermon by accident, but
    // every real caller names it: ScriptureService fills Scripture, TheTableService
    // fills Table.
    ContentFamily family = ContentFamily::Scripture;
    std::string versionName;     // "King James Version (KJV)"
    std::string copyright;
    std::string book;            // "Genesis", or The Table's whole citation line ("47-0412 - Faith Is The Substance")
    std::string bookAbbr;        // "GEN" (Scripture only — a sermon's `book` IS its citation)
    std::string code;            // The Table's citation code alone ("47-0412")
    std::string year;            // The Table's year book ("1947")
    std::string title;           // The Table's sermon title alone ("Faith Is The Substance")
    // The chapter WITHIN the book — a Bible's chapter. A sermon has none, and
    // the field stays 0 there; that is a fact about the DATA, not the signal
    // that an item is a sermon (which is `family` above).
    int chapter = 0;
    std::vector<BookItem> items;   // the selected items (verses / paragraphs), ascending
    // THE OTHER VERSIONS TO SHOW BESIDE IT (a parallel layout), in the order its
    // slots ask for: parallels[0] is {scripture2_*}, [1] is {scripture3_*}, [2] is
    // {scripture4_*}. Empty for a single-version layout, and for every table
    // selection (a sermon has one text). More than three are ignored: the
    // vocabulary stops at the fourth Bible.
    std::vector<ParallelBible> parallels;
};

// How to fill it. The field NAMES are tab-neutral on purpose — the same option
// is "Verse numbers" under Scripture and "Paragraph numbers" under The Table
// (AppSettings' AddFillOptions carries each tab's own singular/plural word) —
// while the stored config KEYS keep the names settings were saved with.
struct BookFillOptions {
    bool numbers = true;
    bool splitLongItems = false;        // divide an item longer than wordsPerSlide over several slides
    bool splitSuffix = false;           // ...and mark the parts 1a, 1b
    // A SLIDE'S TEXT BUDGET, IN WORDS — AND THE ONLY RULE THAT DECIDES WHERE A
    // LONG ITEM IS CUT. This replaced a character limit for the same reason the
    // field is named in words: an operator can judge "how many words fit this
    // slide", while "233 characters" needs counting and measured the wrong thing
    // anyway — the length of one verse, not how much a slide shows.
    //
    // It used to be one of THREE numbers: this one, a `tolerance` percent that let
    // a final piece run past the budget rather than be divided again, and the
    // template's own text box (estimated in characters, then divided by six, then
    // taken as the SMALLER of the two budgets). The box's estimate is what an
    // operator could not see: "Max words per slide: 40" silently became four, and a
    // sermon paragraph came out as 3-1 … 3-6 of four words each. Two of the three
    // are gone. What the operator sets is what the fill does, so where a part ends
    // is predictable from the number on screen: the item is divided into the FEWEST
    // parts the budget allows (3-1, 3-2, … or 1a, 1b with parts numbered), and those
    // parts hold as close to the same number of words as they can. Even sharing —
    // not "fill each part and push the rest on" — is what keeps a part from being
    // the leftover: a 43-word paragraph at a 40-word budget is 22 + 21, where
    // filling to the brim left a full slide and a three-word one ("… can be
    // done."). A part is base or base+1 of total/parts, so the budget is still
    // never exceeded.
    int wordsPerSlide = 40;
    // How many items one slide may take. Not a control any more (the "Max
    // verses/paragraphs" row is gone); the number is now each FAMILY's own
    // decision, set by the caller — see AppSettings::FillOptionsFor: Scripture
    // keeps the packing its shipped layouts assume, The Table takes one
    // paragraph to a slide (a sermon's paragraph is a whole thought, and its
    // length is what "Max words per slide" is for). The struct default is
    // Scripture's, so a caller that passes no options gets the same fill it
    // always did.
    int maxPerSlide = 3;
};

// ---- THE ONE PLACEHOLDER VOCABULARY -----------------------------------------
//
// Every `{token}` a template can carry resolves against these values, and this
// is the ONLY table that says which token means what. It has to be shared: the
// same placeholder text is rendered by TWO paths that must never disagree —
// the preview/roster fill (FillBookSlides, driven by the service's BookSource)
// and the scene builder, which binds an OUTPUT STYLE's or a family template's
// blocks onto a slide it only knows the text and reference of
// (SceneBuilder's BindTemplateBlocks). Those two used to answer differently:
// the fill produced the sentence, while the scene builder turned every
// non-`bind` block that still held a `{` into a blank line — a table design's
// reference block rendered as an EMPTY box on the outputs while the preview
// pane showed it filled (the reported "monitor just shows the item and not the
// text content"). Now both call FillTokens with the values they have, so a
// token either resolves to the same sentence on every surface or is blank on
// every surface.
//
// A value the caller does not have stays empty, and an empty value renders as
// an empty line — never as the raw token.
//
// Declared BEFORE BookSlide on purpose: a filled slide CARRIES the set it was
// filled with (see BookSlide::values), so the type has to be complete here.
struct BookValues {
    // WHICH FAMILY these values belong to, and it decides what a token can
    // answer: `{scripture_name}` (the Bible version), `{scripture_book_abbr}`
    // and `{scripture_chapter}` are facts only a Bible HAS, so on a sermon they
    // resolve empty — no matter what a caller put in `name`/`bookAbbr`/`chapter`.
    // Before this, "The Table" was assigned to `versionName` by the tab that
    // knows a sermon has no Bible version, and `{scripture_name}` then rendered
    // the brand string into a slot meant for "King James Version". Every other
    // token stays DATA-driven (an empty value renders empty), so a template
    // saved before the family existed keeps filling exactly as it did.
    ContentFamily family = ContentFamily::Scripture;
    // ONE COLUMN PER VERSION ON THIS SLIDE, the selected Bible first: [0].text is
    // the slide's body and [0].name is `name` above, while [1..] are the parallel
    // versions ({scripture2_text} is [1].text). Built by the fill — the same
    // labelling and grouping as the primary's — and carried on the slide so an
    // output style's own parallel slots resolve on air too.
    struct BibleColumn {
        std::string name;        // "King James Version" / the other version's name
        std::string bookAbbr;    // that version's abbreviation of the book
        std::string text;        // this slide's verses of that version, numbered alike
    };
    std::vector<BibleColumn> bibles;
    std::string text, reference, referenceFull, referenceLast, numbers, name, book, bookAbbr, chapter, copyright;
    // The Table's own parts (blank for Scripture): the citation's leading date
    // code, the year book, and the sermon's title with the code taken off.
    std::string code, year, title;
    // Does this family have an answer for the token?
    bool Answers(const std::string& key) const {
        if (family == ContentFamily::Scripture) return true;
        // A sermon: the three Bible-only facts. Everything else (the text, the
        // numbers, the book/citation, the table_ parts, a copyright the caller
        // supplied) is data and is answered.
        return key != "name" && key != "book_abbr" && key != "chapter";
    }
};

// ONE SLIDE'S WORTH of the filled template.
struct BookSlide {
    std::vector<ContentBlock> blocks;    // the template's blocks, filled in
    // THE BODY, and it is carried SEPARATELY on purpose: once the blocks are
    // filled, a design that carries several slots (the metadata palette inserts
    // one per field) no longer says which of them is the content — a table
    // design with the date code in its corner has an equally text-bearing code
    // block sitting first. The fill is the one place that knows, so it hands the
    // body on; the services put it on the slide and the engine reads it instead
    // of guessing from block order. Same string the body slot receives
    // (`values.text`), paragraph/sermon numbers included.
    std::string body;
    // EVERY VALUE THE FILL USED, carried on the slide for the same reason the
    // body is: the engine resolves the slots of an output style / family template
    // through SceneBuilder's SlideValues, which reads these off the slide's meta,
    // and the fill is the only place that HAS them. Without this the preview and
    // the outputs disagree by construction — a design whose corner slot is
    // {table_code_paragraphs} showed "47-0412 3" in the preview pane and an EMPTY
    // box on the outputs, because nothing ever put the code and the paragraph
    // numbers on the slide (the reported "it just remains the code"). The
    // services put them in the slide's meta, key for key, and SlideValues reads
    // them; a value that is genuinely empty stays empty and renders as an empty
    // line, never as a raw token.
    BookValues values;
    std::string reference;               // "Genesis 1:1-3" / "47-0412 - Faith Is The Substance 1-3"
    std::string title;                   // the same, for a slide list
    std::vector<int> numbers;            // source item numbers in this group
    std::vector<BookPart> parts;         // source pieces, used for selectable continuation rows
};

// `text` with every placeholder of the vocabulary above resolved. An UNKNOWN
// `{…}` is left LITERAL (the caller decides whether an unrecognised placeholder
// is a mistake worth showing or a leftover worth blanking), and a NUMBER SLOT
// ({scripture_number} / {table_number}) resolves to nothing while swallowing
// the space in front of its item — the engine draws that number separately.
std::string FillTokens(const std::string& text, const BookValues& values);

// Does `text` name at least one placeholder of that same vocabulary? What the
// scene builder asks before deciding a block is a template slot at all.
bool HasTokens(const std::string& text);

// DOES THIS FAMILY HAVE AN ANSWER FOR THIS TOKEN? `token` is the part inside the
// braces, exactly as Resolve() reads it ("table_code_paragraphs").
//
// This exists for the METADATA REGISTRY (MetadataFields.qml), whose two lists of
// fields are a hand-written mirror of the vocabulary above: the only thing that
// noticed the two drifting apart was a unit test, while a field the engine cannot
// fill for its own family kept sitting in the menu looking implemented (that is
// how {meta_copyright} and {scripture_book_abbr} behaved on a sermon). The registry
// asks this at load, so a mismatch is REPORTED at boot instead of discovered from
// an empty box on a slide.
//
// A NUMBER SLOT ({scripture_number} / {table_number}) answers true: it renders
// nothing on purpose (the engine draws the number itself), and that IS its answer.
bool FamilyAnswersToken(ContentFamily family, const std::string& token);

// Does the template name any placeholder at all? (If not it is an old-style
// template: its `text`/`ref` bound blocks get the items and the reference.)
bool HasPlaceholders(const std::vector<ContentBlock>& templateBlocks);

// "Genesis 1:1-3, 5" for the given items of one chapter; "Genesis 1" when
// `numbers` is empty (the whole chapter).
//
// THE FAMILY decides the shape, not the absence of a chapter: Scripture writes
// the chapter and joins the items with a colon ("Genesis 1:1-3"); Table writes
// no chapter — its `book` is already the whole citation line — and joins with a
// space ("47-0412 - Faith Is The Substance 1-3"), because a colon there reads as
// a Bible reference. Passing content that is a sermon as Scripture therefore
// produces a reference that looks like a Bible verse, which is what the family
// argument exists to prevent.
std::string BookReference(const std::string& book, int chapter, const std::vector<int>& numbers,
                          ContentFamily family = ContentFamily::Scripture);
// "1-3, 5" — the item numbers alone.
std::string NumberRange(const std::vector<int>& numbers);

// Every slide the selection needs, or just the first real output slide when
// `onlyFirst` (a preview). Splitting and slide limits apply in either mode.
// An empty selection gives no slides.
std::vector<BookSlide> FillBookSlides(const std::vector<ContentBlock>& templateBlocks, const BookSource& source,
                                      const BookFillOptions& options, bool onlyFirst = false);

} // namespace bps::presentation
