#include "modules/presentation/BookFill.hpp"

// No Json.hpp: the template's text box is no longer measured for a character
// capacity (its estimate was the second budget that made "40 words" behave as
// four — see BookFillOptions::wordsPerSlide).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <set>

namespace bps::presentation {

namespace {

// ---- references -------------------------------------------------------------------------------------------------------

std::string Letter(int index) { return std::string(1, static_cast<char>('a' + std::min(index, 25))); }

// A piece of a verse: a whole verse, or one part of a long one.
struct Part {
    int number = 0;
    int partIndex = 0;
    std::string label;       // "1", "1a", or "" (a continuation without a number, or numbers off)
    std::string text;
    bool continuation = false;   // not the first part of its verse
};

// How many words a piece of text holds (any run of non-space characters).
int WordCount(const std::string& text) {
    int n = 0;
    bool inWord = false;
    for (char c : text) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!space && !inWord) ++n;
        inWord = !space;
    }
    return n;
}

// Cuts `text` into parts of at most `words` words each — THE budget the operator
// set, and the only thing consulted (see BookFillOptions::wordsPerSlide). Every
// part ends at a word, so there is nothing to "wait" for mid-cut.
//
// ONE RULE, READABLE FROM THE NUMBER: the item is divided into the FEWEST parts
// the budget allows, and those parts hold as close to the same number of words as
// they can. Filling each part to the brim and pushing the rest on (what this used
// to do) is the same thing while the text divides evenly — twelve words at four
// words a part is 4+4+4 either way — and a worse one the moment it does not: a
// paragraph of 43 words at a 40-word budget came out as a full slide and a
// THREE-WORD slide ("… can be done."), because the last part is whatever was left
// over. Balancing asks how many parts the budget NEEDS (ceil) and shares the words
// across them, so 43 words is 22+21 and a two-word tail cannot exist. The budget
// is still never exceeded — a part is base or base+1 of `total/parts`, and
// ceil(total/parts) <= budget by construction — so "a slide shows at most this
// many words" stays exactly true.
//
// The old single-word exception (a one-word final part riding with the part before
// it) is gone with the leftovers it existed for: with parts = ceil(total/budget)
// and the words shared evenly, no part can hold one word unless the whole item is
// one word. (A `tolerance` percentage used to cover the same ground; a percentage
// is a second, invisible rule on top of a number the operator can see, and it is
// gone too.)
std::vector<std::string> SplitTextByWords(const std::string& text, int words) {
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty())
        return { std::string() };
    const size_t budget = static_cast<size_t>(std::max(1, words));
    const size_t total = tokens.size();
    // The fewest parts that respect the budget (>= 1, and <= total: a part always
    // gets at least one word), and the words shared as evenly as whole words allow
    // — the first `extra` parts take one word more than the rest.
    const size_t parts = (total + budget - 1) / budget;
    const size_t base = total / parts;
    const size_t extra = total % parts;
    std::vector<std::string> out;
    out.reserve(parts);
    size_t i = 0;
    for (size_t p = 0; p < parts; ++p) {
        const size_t take = base + (p < extra ? 1 : 0);
        std::string piece;
        for (size_t n = 0; n < take; ++n) {
            if (!piece.empty()) piece += ' ';
            piece += tokens[i + n];
        }
        out.push_back(std::move(piece));
        i += take;
    }
    return out;
}

std::string Trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// "King James Version (KJV)" -> "King James Version"
std::string WithoutBrackets(const std::string& name) {
    std::string out;
    int depth = 0;
    for (char c : name) {
        if (c == '(') ++depth;
        else if (c == ')') { if (depth > 0) --depth; }
        else if (depth == 0) out += c;
    }
    return Trim(out);
}

// ---- filling a template's text --------------------------------------------------------------------------------------------

// The values a placeholder resolves against are BookValues, declared in the
// header: this file is the only implementation of the vocabulary, and the
// scene builder calls the very same FillTokens (see the header note).

// The value of a token, or false when `token` is not one of ours. `known` (optional)
// reports something finer: whether the token is a KEY THIS VOCABULARY KNOWS, as
// opposed to a `table_`/`meta_`-looking name with a typo in it — which still
// resolves (a misspelt table_ token fills blank rather than leaking braces onto a
// slide) but which the metadata registry's boot check must hear about, since a chip
// carrying one would render nothing forever.
bool Resolve(const std::string& token, const BookValues& v, std::string& out, bool& swallowSpace,
             bool* known = nullptr) {
    swallowSpace = false;
    if (known) *known = false;
    if (token.rfind("meta_", 0) == 0) {
        if (known) *known = token == "meta_copyright";   // the one meta_ field there is
        out = token == "meta_copyright" ? v.copyright : std::string();
        return true;
    }
    // ---- The Table's own family -------------------------------------------------
    //
    // A sermon has no chapter, no Bible version and no verse numbers, so the
    // whole scripture_ family above says the WRONG THING for it ("{scripture_name}"
    // is the Bible's version — blank for a sermon — and "{scripture_book}" has to
    // carry the entire citation line). Each table_ token names ONE part of the
    // citation instead, which is what lets a design put the code, the year, the
    // title and the paragraph numbers in four separate slots, or any combination
    // of them. They exist for The Table alone; the scripture_ family still
    // resolves too, so templates saved before this change keep filling.
    if (token.rfind("table", 0) == 0) {
        if (known) *known = true;   // ...and the final else below takes it back
        if (token == "table_text") out = v.text;
        else if (token == "table_paragraphs") out = v.numbers;
        else if (token == "table_reference") out = v.reference;
        else if (token == "table_reference_full") out = v.referenceFull;
        else if (token == "table_reference_last") out = v.referenceLast;
        else if (token == "table_citation") out = v.book;
        else if (token == "table_code") out = v.code;
        else if (token == "table_code_paragraphs") {
            // The two things a printed sermon reference is made of, in the order
            // they are written: the date code and the paragraphs on THIS slide
            // ("47-0412 1-3"). A design whose footer has one slot for a
            // reference, and no room for the sermon's title, wants exactly this
            // — {table_reference} carries the title between them.
            out = v.code;
            if (!v.numbers.empty()) {
                if (!out.empty()) out += " ";
                out += v.numbers;
            }
        }
        else if (token == "table_year") out = v.year;
        else if (token == "table_title") out = v.title;
        else if (token == "table_number") { out.clear(); swallowSpace = true; }
        else { out.clear(); if (known) *known = false; }   // still ours: an unknown table_ token fills blank, never stays literal
        return true;
    }
    if (token.rfind("scripture", 0) != 0) return false;
    size_t i = 9;
    int n = 1;
    if (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) {
        n = 0;
        while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) n = n * 10 + (token[i++] - '0');
    }
    if (i >= token.size() || token[i] != '_') return false;
    const std::string key = token.substr(i + 1);
    // WHICH VERSION THIS TOKEN ASKS FOR: n = 1 is the selected Bible, n = 2..4 the
    // PARALLEL ones (ParallelBible) — the tab's extra versions, whose columns sit
    // beside the first. A version that is not selected renders empty, exactly as a
    // second Bible always did when the app could only ever have one.
    const BookValues::BibleColumn* column = nullptr;
    if (n >= 2) {
        const std::size_t extra = static_cast<std::size_t>(n - 2);
        if (extra + 1 < v.bibles.size())          // bibles[0] is the selected one
            column = &v.bibles[extra + 1];
    }
    const bool ours = n <= 1 || column != nullptr;

    if (key == "number") { if (known) *known = true; out.clear(); swallowSpace = true; return true; }
    if (key == "red_jesus" || key == "undertitle") { if (known) *known = true; out.clear(); return true; }
    if (!ours) { if (known) *known = true; out.clear(); return true; }   // a real key, just a version that is not on the slide
    // THE FAMILY ANSWERS FIRST. The three Bible-only facts (the version's name,
    // the book's abbreviation, the chapter) are not facts a sermon has, so on a
    // sermon they are empty even when a caller filled the field — that is what
    // stops a sermon's brand string or a stray value from landing in a slot
    // labelled "Bible version". Every other token is data, answered when it is
    // there (see BookValues::Answers).
    if (!v.Answers(key)) { out.clear(); return true; }
    if (known) *known = true;
    // PER VERSION: the verses and the facts that belong to a translation (its name
    //, its abbreviation of the book).
    if (key == "text") out = n <= 1 ? v.text : column->text;
    else if (key == "name") out = n <= 1 ? v.name : column->name;
    else if (key == "book_abbr") out = n <= 1 ? v.bookAbbr : column->bookAbbr;
    // PER PASSAGE — the same on every column, whichever version it is written in:
    // the reference, the whole-selection variants, the numbers and the book.
    else if (key == "reference") out = v.reference;
    else if (key == "reference_full") out = v.referenceFull;
    else if (key == "reference_last") out = v.referenceLast;
    else if (key == "verses") out = v.numbers;
    else if (key == "book") out = v.book;
    else if (key == "chapter") out = v.chapter;
    else { out.clear(); if (known) *known = false; }   // an unknown scripture_ key
    return true;
}

std::string Fill(const std::string& in, const BookValues& v) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '{') {
            const size_t close = in.find('}', i);
            if (close != std::string::npos) {
                std::string value;
                bool swallow = false;
                if (Resolve(in.substr(i + 1, close - i - 1), v, value, swallow)) {
                    out += value;
                    i = close;
                    if (swallow && i + 1 < in.size() && in[i + 1] == ' ') ++i;
                    continue;
                }
            }
        }
        out += in[i];
    }
    return Trim(out).empty() ? std::string() : out;
}

// ---- one version's text for one slide --------------------------------------------------------------------------------------

// The label in front of a piece ("3 ", "3a "), by the SAME rule the selected
// Bible's parts use — the fill applies it to every version, so a parallel column
// numbers its verses exactly as the column beside it does.
std::string PartLabel(const BookItem& item, std::size_t part, std::size_t parts, const BookFillOptions& options) {
    if (!options.numbers)
        return {};
    if (part == 0)
        return std::to_string(item.number) + (options.splitSuffix && parts > 1 ? Letter(0) : std::string());
    return options.splitSuffix ? std::to_string(item.number) + Letter(static_cast<int>(part)) : std::string();
}

// ONE OTHER VERSION'S TEXT FOR ONE SLIDE: the items the slide shows, in that
// version's own wording, each led by its number (when numbers are on), divided the
// same way a long verse of the selected Bible is divided so the columns stay in
// step. An item this version does not have is simply absent from its column.
//
// `budget` is the slide's own word budget for this template (see the caller): the
// column divides by the SAME number the selected Bible's text does, because a
// column cut by one rule beside a body cut by another is how the two fell out of
// step.
std::string ColumnText(const std::vector<BookItem>& items, const std::vector<int>& numbersHere,
                       const BookFillOptions& options, int budget) {
    std::string out;
    bool first = true;
    for (const int wanted : numbersHere) {
        const BookItem* item = nullptr;
        for (const BookItem& candidate : items)
            if (candidate.number == wanted) { item = &candidate; break; }
        if (!item)
            continue;
        const std::string body = Trim(item->text);
        const std::vector<std::string> pieces =
            options.splitLongItems && WordCount(body) > budget
                ? SplitTextByWords(body, budget)
                : std::vector<std::string>{ body };
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (!first)
                out += ' ';
            first = false;
            const std::string label = PartLabel(*item, i, pieces.size(), options);
            if (!label.empty())
                out += label + " ";
            out += pieces[i];
        }
    }
    return out;
}

// Does this text ask for the REFERENCE, in either family's spelling? Used by the
// single-item de-duplication below (the reference already names the one picked
// item, so its leading number would say it twice). The list used to carry only
// the scripture spellings, so the same single-paragraph pick kept a redundant
// leading number under The Table and dropped it under Scripture — one rule, two
// answers. The spellings are PREFIX matches on purpose: {scripture_reference}
// also covers {scripture_reference_full} and {scripture_reference_last}.
constexpr const char* kReferenceSlots[] = { "{scripture_reference", "{scripture1_reference", "{scripture_verse",
                                             "{scripture1_verse",     "{table_reference",     "{table_citation" };

bool NamesReferenceSlot(const std::string& text) {
    for (const char* slot : kReferenceSlots)
        if (text.find(slot) != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

std::string FillTokens(const std::string& text, const BookValues& values) {
    return Fill(text, values);
}

bool HasTokens(const std::string& text) {
    // Recognition only — the values are irrelevant to whether a token is ours
    // (Resolve() reads `v` solely to produce the replacement).
    static const BookValues kEmpty;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;
        const size_t close = text.find('}', i);
        if (close == std::string::npos) break;
        std::string ignored;
        bool swallow = false;
        if (Resolve(text.substr(i + 1, close - i - 1), kEmpty, ignored, swallow)) return true;
        i = close;
    }
    return false;
}

bool FamilyAnswersToken(ContentFamily family, const std::string& token) {
    // Is it a key of our vocabulary? A name that merely LOOKS like one (a typo, or
    // nothing to do with us) is not, which is what makes this check worth having:
    // the registry's chip would otherwise render blank for ever.
    BookValues probe;
    probe.family = family;
    std::string value;
    bool swallow = false;
    bool known = false;
    if (!Resolve(token, probe, value, swallow, &known) || !known)
        return false;
    // The table_ and meta_ families answer from DATA: an empty value is the answer
    // when the caller has nothing, never a missing capability.
    if (token.rfind("scripture", 0) != 0)
        return true;
    // A scripture token may be numbered ({scripture1_name}); the NUMBER is not part
    // of the key the family answers for.
    std::size_t i = 9;
    while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) ++i;
    if (i >= token.size() || token[i] != '_')
        return true;   // a legacy spelling Resolve() accepts as-is
    const std::string key = token.substr(i + 1);
    if (key == "number" || key == "red_jesus" || key == "undertitle")
        return true;   // style markers: nothing on purpose
    return probe.Answers(key);
}

bool HasPlaceholders(const std::vector<ContentBlock>& blocks) {
    // A "styled" template is one that names at least one placeholder — the
    // scripture family, The Table's own family, or a meta_ field. A table_
    // token counts on its own: a layout whose only placeholder is the date code
    // is still a template that expects the engine to fill it, and an earlier
    // scripture-only test reported it as a plain "old style" layout (so the
    // code never appeared at all).
    return std::any_of(blocks.begin(), blocks.end(), [](const ContentBlock& b) {
        return b.kind == "text"
            && (b.text.find("{scripture") != std::string::npos
                || b.text.find("{table_") != std::string::npos
                || b.text.find("{meta_") != std::string::npos);
    });
}

std::string NumberRange(const std::vector<int>& numbers) {
    std::set<int> sorted(numbers.begin(), numbers.end());
    std::string out;
    for (auto it = sorted.begin(); it != sorted.end();) {
        int first = *it, last = *it;
        for (++it; it != sorted.end() && *it == last + 1; ++it) last = *it;
        if (!out.empty()) out += ", ";
        out += first == last ? std::to_string(first) : std::format("{}-{}", first, last);
    }
    return out;
}

std::string BookReference(const std::string& book, int chapter, const std::vector<int>& numbers,
                          ContentFamily family) {
    std::string out = book;
    // A CHAPTER is a Bible's: a sermon's `book` is already its whole citation
    // line ("47-0412 - Faith Is The Substance"), so there is nothing to append.
    // This used to be `chapter > 0`, which meant a Bible-shaped caller with no
    // chapter data silently produced a sermon-shaped reference.
    if (family == ContentFamily::Scripture && chapter > 0)
        out += std::format(" {}", chapter);
    if (!numbers.empty()) {
        // The JOINER is the family's: a colon is a Bible reference
        // ("Genesis 1:1-3"); a citation joins with a space ("… Substance 1-3"),
        // which is how the printed sermon reference is written.
        if (family == ContentFamily::Scripture)
            out += std::format(":{}", NumberRange(numbers));
        else
            out += std::format(" {}", NumberRange(numbers));
    }
    return out;
}

std::vector<BookSlide> FillBookSlides(const std::vector<ContentBlock>& tmpl, const BookSource& source,
                                      const BookFillOptions& options, bool onlyFirst) {
    std::vector<BookSlide> slides;
    if (source.items.empty()) return slides;

    const bool styled = HasPlaceholders(tmpl);

    // One selected verse with the verse in the reference: the number would say it twice —
    // UNLESS the template explicitly reserves a number slot ({scripture_number}): that
    // marker is the template author's "the number renders here" request, and it beats the
    // de-duplication heuristic (a user-reported gap: the toggle was ON and the marker was
    // in the template, yet a single-verse pick showed no number anywhere).
    bool numbers = options.numbers;
    const bool templateWantsNumberSlot = std::any_of(tmpl.begin(), tmpl.end(), [](const ContentBlock& b) {
        return b.kind == "text" && b.text.find("_number}") != std::string::npos;
    });
    if (source.items.size() == 1 && styled && !templateWantsNumberSlot) {
        for (const ContentBlock& b : tmpl)
            if (NamesReferenceSlot(b.text))
                numbers = false;
    }

    // ---- the slide's word budget ----
    //
    // "Max words per slide" IS the budget: one number, the one on screen, and the
    // only rule that decides where a long item is cut. It used to be the SMALLER of
    // this number and the template's own text box, estimated in characters and
    // divided by six — so "40" could silently behave as four, and a sermon
    // paragraph came out as 3-1 … 3-6 of four words each. The box's estimate is
    // gone with the rest of the smart-split rule (see BookFillOptions).
    //
    // Read ONCE, before the pieces are cut: the division below and the parallel
    // columns must all use the same number, or a column and its body cut by two
    // rules fall out of step.
    const int budget = std::max(1, options.wordsPerSlide);

    // ---- the pieces to lay out ----
    std::vector<Part> parts;
    for (const BookItem& v : source.items) {
        const std::string text = Trim(v.text);
        const bool divide = options.splitLongItems && WordCount(text) > budget;
        const std::vector<std::string> pieces = divide ? SplitTextByWords(text, budget)
                                                       : std::vector<std::string>{ text };
        for (size_t i = 0; i < pieces.size(); ++i) {
            Part p;
            p.number = v.number;
            p.partIndex = static_cast<int>(i);
            p.text = pieces[i];
            p.continuation = i > 0;
            if (numbers) {
                if (i == 0) p.label = std::to_string(v.number) + (options.splitSuffix && pieces.size() > 1 ? Letter(0) : std::string());
                else if (options.splitSuffix) p.label = std::to_string(v.number) + Letter(static_cast<int>(i));
            }
            parts.push_back(std::move(p));
        }
    }

    // ---- which pieces go on which slide ----
    //
    // ONE rule: a slide takes `maxPerSlide` pieces (its family's own number — see
    // AppSettings::FillOptionsFor), in the order the division above produced them,
    // so the words the operator's budget pushed off one slide are exactly what the
    // next one shows. The template's text box no longer has a say here either: a
    // box measured in characters cannot be the thing that decides which paragraphs
    // share a slide.
    std::vector<std::vector<size_t>> groups;
    {
        size_t per = static_cast<size_t>(std::max(1, options.maxPerSlide));
        if ((parts.size() + per - 1) / per == 2) per = (parts.size() + 1) / 2;   // two slides: share the pieces evenly
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i % per == 0) groups.emplace_back();
            groups.back().push_back(i);
        }
    }
    // Preview is the first real output slide. Apply all splitting and limits
    // above, then keep that first group; otherwise long paragraphs looked
    // unsplit in the preview and its contents differed from the slide on air.
    if (onlyFirst && groups.size() > 1)
        groups.resize(1);

    // ---- fill the template once per slide ----
    std::vector<int> allNumbers;
    for (const BookItem& v : source.items) allNumbers.push_back(v.number);
    const std::string fullReference = BookReference(source.book, source.chapter, allNumbers, source.family);

    for (size_t g = 0; g < groups.size(); ++g) {
        std::string text;
        std::vector<int> numbersHere;
        std::vector<BookPart> slideParts;
        for (size_t k = 0; k < groups[g].size(); ++k) {
            const Part& p = parts[groups[g][k]];
            slideParts.push_back({ p.number, p.partIndex, p.text, p.continuation });
            if (k > 0) text += ' ';
            text += p.label.empty() ? p.text : p.label + " " + p.text;
            if (numbersHere.empty() || numbersHere.back() != p.number) numbersHere.push_back(p.number);
        }

        BookValues values;
        // THE FAMILY TRAVELS WITH THE VALUES: FillTokens and every surface that
        // reads them back (the slide's meta, SceneBuilder's SlideValues) answers
        // "what is this content?" from here instead of re-deriving it.
        values.family = source.family;
        values.text = text;
        values.reference = BookReference(source.book, source.chapter, numbersHere, source.family);
        values.referenceFull = fullReference;
        values.referenceLast = g + 1 == groups.size() ? fullReference : std::string();
        values.numbers = NumberRange(numbersHere);
        values.name = WithoutBrackets(source.versionName);
        values.book = source.book;
        values.bookAbbr = source.bookAbbr;
        // A CHAPTER is a Bible's: a sermon's chapter is folded into the citation
        // in `book`, and the field renders empty rather than as a bare "0".
        values.chapter = source.family == ContentFamily::Scripture && source.chapter > 0
                             ? std::to_string(source.chapter) : std::string();
        values.copyright = source.copyright;
        values.code = source.code;
        values.year = source.year;
        values.title = source.title;
        // ONE COLUMN PER VERSION ON THIS SLIDE (see BookValues::bibles), the
        // selected Bible first: its column IS the body, and each parallel version's
        // is its own wording of the very same verses, numbered alike. Capped at
        // four — the vocabulary stops at {scripture4_text}.
        values.bibles.clear();
        values.bibles.push_back({ values.name, source.bookAbbr, values.text });
        for (const ParallelBible& p : source.parallels) {
            if (values.bibles.size() >= 4)
                break;
            values.bibles.push_back({ WithoutBrackets(p.name), p.bookAbbr,
                                      ColumnText(p.items, numbersHere, options, budget) });
        }

        BookSlide slide;
        slide.body = values.text;      // the body the blocks now carry (see BookSlide)
        slide.values = values;         // ...and every other value the slots can ask for
        slide.reference = values.reference;
        slide.title = values.reference;
        slide.numbers = numbersHere;
        slide.parts = std::move(slideParts);
        slide.blocks = tmpl;
        for (ContentBlock& b : slide.blocks) {
            if (b.kind != "text") continue;
            if (styled) b.text = Fill(b.text, values);
            else if (b.bind == "text") b.text = text;
            else if (b.bind == "ref") b.text = values.reference;
        }
        slides.push_back(std::move(slide));
    }
    return slides;
}

} // namespace bps::presentation
