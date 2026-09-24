#include "modules/presentation/ScriptureSlides.hpp"

#include "core/config/Json.hpp"

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
    std::string label;       // "1", "1a", or "" (a continuation without a number, or numbers off)
    std::string text;
    bool continuation = false;   // not the first part of its verse
};

// Cuts `text` into pieces of about `limit` characters at word ends; a cut may wait up to `tolerance` percent past the limit.
std::vector<std::string> SplitText(const std::string& text, int limit, int tolerance) {
    std::vector<std::string> out;
    const size_t max = static_cast<size_t>(std::max(3, limit));
    const size_t reach = max + max * static_cast<size_t>(std::clamp(tolerance, 0, 100)) / 100;
    std::string rest = text;
    while (rest.size() > reach) {
        size_t cut = rest.rfind(' ', reach);
        if (cut == std::string::npos || cut < max / 2) cut = rest.find(' ', reach);   // a very long word: cut after it
        if (cut == std::string::npos || cut == 0) break;
        out.push_back(rest.substr(0, cut));
        rest.erase(0, cut + 1);
    }
    if (!rest.empty() || out.empty()) out.push_back(rest);
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

struct Values {
    std::string text, reference, referenceFull, referenceLast, verses, name, book, bookAbbr, chapter, copyright;
};

// The value of a scripture token, or nullopt when `token` is not one of ours.
bool Resolve(const std::string& token, const Values& v, std::string& out, bool& swallowSpace) {
    swallowSpace = false;
    if (token.rfind("meta_", 0) == 0) {
        out = token == "meta_copyright" ? v.copyright : std::string();
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
    const bool ours = n <= 1;   // only the first Bible exists; a second, third... is blank

    if (key == "number") { out.clear(); swallowSpace = true; return true; }
    if (key == "red_jesus" || key == "undertitle") { out.clear(); return true; }
    if (!ours) { out.clear(); return true; }
    if (key == "text") out = v.text;
    else if (key == "reference") out = v.reference;
    else if (key == "reference_full") out = v.referenceFull;
    else if (key == "reference_last") out = v.referenceLast;
    else if (key == "verses") out = v.verses;
    else if (key == "name") out = v.name;
    else if (key == "book") out = v.book;
    else if (key == "book_abbr") out = v.bookAbbr;
    else if (key == "chapter") out = v.chapter;
    else out.clear();
    return true;
}

std::string Fill(const std::string& in, const Values& v) {
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

// ---- the template's text box: how much fits ---------------------------------------------------------------------------------

size_t Capacity(const std::vector<ContentBlock>& blocks) {
    const ContentBlock* box = nullptr;
    for (const ContentBlock& b : blocks)
        if (b.kind == "text" && (b.text.find("{scripture_text}") != std::string::npos || b.text.find("{scripture1_text}") != std::string::npos)) { box = &b; break; }
    if (!box)
        for (const ContentBlock& b : blocks)
            if (b.kind == "text" && b.bind == "text") { box = &b; break; }
    if (!box) return 400;
    double fontSize = 60;
    if (auto meta = json::Parse(box->metaJson); meta.ok())
        if (const json::Value* f = meta.value().Find("fontSize"); f && f->asNumber() > 0) fontSize = f->asNumber();
    const double perLine = std::max(10.0, std::floor(box->width / (fontSize * 0.5)));
    const double lines = std::max(1.0, std::floor(box->height / (fontSize * 1.25)));
    return static_cast<size_t>(std::max(20.0, perLine * lines));
}

} // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool HasScriptureValues(const std::vector<ContentBlock>& blocks) {
    return std::any_of(blocks.begin(), blocks.end(),
                       [](const ContentBlock& b) { return b.kind == "text" && b.text.find("{scripture") != std::string::npos; });
}

std::string ScriptureVerseRange(const std::vector<int>& verses) {
    std::set<int> sorted(verses.begin(), verses.end());
    std::string out;
    for (auto it = sorted.begin(); it != sorted.end();) {
        int first = *it, last = *it;
        for (++it; it != sorted.end() && *it == last + 1; ++it) last = *it;
        if (!out.empty()) out += ", ";
        out += first == last ? std::to_string(first) : std::format("{}-{}", first, last);
    }
    return out;
}

std::string ScriptureReference(const std::string& book, int chapter, const std::vector<int>& verses) {
    std::string out = book;
    if (chapter > 0) out += std::format(" {}", chapter);
    // No chapter (The Table: the book IS the sermon citation "47-0412 - Faith Is The
    // Substance") -> the verse/paragraph range joins with a space, reading like the
    // citation line ("... Substance 1"), not a scripture colon ("... Substance:1").
    if (!verses.empty())
        out += chapter > 0 ? std::format(":{}", ScriptureVerseRange(verses))
                           : std::format(" {}", ScriptureVerseRange(verses));
    return out;
}

std::vector<ScriptureSlide> BuildScriptureSlides(const std::vector<ContentBlock>& tmpl, const ScriptureSource& source,
                                                 const ScriptureSettings& settings, bool onlyFirst) {
    std::vector<ScriptureSlide> slides;
    if (source.verses.empty()) return slides;

    const bool styled = HasScriptureValues(tmpl);

    // One selected verse with the verse in the reference: the number would say it twice —
    // UNLESS the template explicitly reserves a number slot ({scripture_number}): that
    // marker is the template author's "the number renders here" request, and it beats the
    // de-duplication heuristic (a user-reported gap: the toggle was ON and the marker was
    // in the template, yet a single-verse pick showed no number anywhere).
    bool numbers = settings.verseNumbers;
    const bool templateWantsNumberSlot = std::any_of(tmpl.begin(), tmpl.end(), [](const ContentBlock& b) {
        return b.kind == "text" && b.text.find("_number}") != std::string::npos;
    });
    if (source.verses.size() == 1 && styled && !templateWantsNumberSlot) {
        for (const ContentBlock& b : tmpl)
            if (b.text.find("{scripture_reference") != std::string::npos || b.text.find("{scripture1_reference") != std::string::npos ||
                b.text.find("{scripture_verse") != std::string::npos || b.text.find("{scripture1_verse") != std::string::npos)
                numbers = false;
    }

    // ---- the pieces to lay out ----
    std::vector<Part> parts;
    for (const ScriptureVerse& v : source.verses) {
        const std::string text = Trim(v.text);
        const bool divide = settings.splitLongVerses && !onlyFirst && static_cast<int>(text.size()) > settings.longVersesChars;
        const std::vector<std::string> pieces = divide ? SplitText(text, settings.longVersesChars, settings.longVersesTolerance)
                                                       : std::vector<std::string>{ text };
        for (size_t i = 0; i < pieces.size(); ++i) {
            Part p;
            p.number = v.number;
            p.text = pieces[i];
            p.continuation = i > 0;
            if (numbers) {
                if (i == 0) p.label = std::to_string(v.number) + (settings.splitLongVersesSuffix && pieces.size() > 1 ? Letter(0) : std::string());
                else if (settings.splitLongVersesSuffix) p.label = std::to_string(v.number) + Letter(static_cast<int>(i));
            }
            parts.push_back(std::move(p));
        }
    }

    // ---- which pieces go on which slide ----
    std::vector<std::vector<size_t>> groups;
    if (onlyFirst) {
        groups.emplace_back();
        for (size_t i = 0; i < parts.size(); ++i) groups.back().push_back(i);
    } else if (settings.smartSplit) {
        const size_t capacity = Capacity(tmpl);
        size_t used = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            const size_t size = parts[i].text.size() + parts[i].label.size() + 1;
            if (groups.empty() || (used + size > capacity && !groups.back().empty())) { groups.emplace_back(); used = 0; }
            groups.back().push_back(i);
            used += size;
        }
    } else {
        size_t per = static_cast<size_t>(std::max(1, settings.versesPerSlide));
        if ((parts.size() + per - 1) / per == 2) per = (parts.size() + 1) / 2;   // two slides: share the verses evenly
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i % per == 0) groups.emplace_back();
            groups.back().push_back(i);
        }
    }

    // ---- fill the template once per slide ----
    std::vector<int> allNumbers;
    for (const ScriptureVerse& v : source.verses) allNumbers.push_back(v.number);
    const std::string fullReference = ScriptureReference(source.book, source.chapter, allNumbers);

    for (size_t g = 0; g < groups.size(); ++g) {
        std::string text;
        std::vector<int> numbersHere;
        for (size_t k = 0; k < groups[g].size(); ++k) {
            const Part& p = parts[groups[g][k]];
            if (k > 0) text += (p.continuation || !settings.versesOnIndividualLines) ? " " : "\n";
            text += p.label.empty() ? p.text : p.label + " " + p.text;
            if (numbersHere.empty() || numbersHere.back() != p.number) numbersHere.push_back(p.number);
        }

        Values values;
        values.text = text;
        values.reference = ScriptureReference(source.book, source.chapter, numbersHere);
        values.referenceFull = fullReference;
        values.referenceLast = g + 1 == groups.size() ? fullReference : std::string();
        values.verses = ScriptureVerseRange(numbersHere);
        values.name = WithoutBrackets(source.versionName);
        values.book = source.book;
        values.bookAbbr = source.bookAbbr;
        // Chapter 0 (The Table: the "chapter" is folded into the citation in
        // `book`) renders empty rather than a bare "0".
        values.chapter = source.chapter > 0 ? std::to_string(source.chapter) : std::string();
        values.copyright = source.copyright;

        ScriptureSlide slide;
        slide.reference = values.reference;
        slide.title = values.reference;
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
