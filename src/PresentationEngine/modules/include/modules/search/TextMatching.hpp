// Shared text-matching primitives for search surfaces.
//
// One definition on purpose: the Search Engine's query path and The Table's
// paragraph scan must agree on what "word", "whole word" and "phrase" MEAN.
// They were private copies in two .cpp files and drifted once already (one
// became punctuation-tolerant, the other didn't) — so the helpers now live
// here and both files include this header. Internal to the engine modules:
// public APIs stay in SearchEngine.hpp / the library's own header.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace bps::search::textmatch {

// ASCII lower-case (bytes >= 0x80 are passed through untouched: UTF-8 text is
// matched verbatim; case folding beyond ASCII is not needed for this corpus).
inline std::string Lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// True when `needle` occurs in `hay` as a WHOLE word: the characters around
// the occurrence are not letters/digits (so "we" does not match "went").
// UTF-8 continuation bytes (>= 0x80) count as word letters so multibyte words
// stay whole.
inline bool ContainsWord(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return false;
    const auto isWord = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
    for (size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + 1)) {
        const bool leftOk = at == 0 || !isWord(static_cast<unsigned char>(hay[at - 1]));
        const size_t end = at + needle.size();
        const bool rightOk = end >= hay.size() || !isWord(static_cast<unsigned char>(hay[end]));
        if (leftOk && rightOk) return true;
    }
    return false;
}

// All the terms in order as a phrase, tolerating whatever punctuation/space
// sits between them ("then, friends" in the text matches the terms
// then+friends; a phrase re-wrapped across a line break still matches). Terms
// keep the user's word order; a single term is just a whole-word test. The
// LAST term may end mid-word ("then friend" matches "then, friends," — the
// user's final word unfinished), the others must be whole. Real verbatim
// presence — the strongest evidence a paragraph is what was asked for — and
// the reason a pasted line always finds its paragraph. When `matchPos` is
// given it receives the match's start (for snippet anchoring).
inline bool ContainsPhrase(std::string_view hay, const std::vector<std::string>& terms,
                           size_t* matchPos = nullptr) {
    if (terms.empty()) return false;
    if (matchPos) *matchPos = std::string_view::npos;
    if (terms.size() == 1) {
        const auto isWord = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
        for (size_t at = hay.find(terms.front()); at != std::string_view::npos;
             at = hay.find(terms.front(), at + 1)) {
            const bool leftOk = at == 0 || !isWord(static_cast<unsigned char>(hay[at - 1]));
            const size_t end = at + terms.front().size();
            const bool rightOk = end >= hay.size() || !isWord(static_cast<unsigned char>(hay[end]));
            if (leftOk && rightOk) {
                if (matchPos) *matchPos = at;
                return true;
            }
        }
        return false;
    }
    const auto isWord = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
    const std::string& first = terms.front();
    const size_t last = terms.size() - 1;
    for (size_t at = hay.find(first); at != std::string_view::npos;
         at = hay.find(first, at + 1)) {
        if (at > 0 && isWord(static_cast<unsigned char>(hay[at - 1]))) continue;  // not a word start
        size_t pos = at + first.size();
        if (pos < hay.size() && isWord(static_cast<unsigned char>(hay[pos]))) continue;
        bool ok = true;
        for (size_t t = 1; t < terms.size(); ++t) {
            while (pos < hay.size() && !isWord(static_cast<unsigned char>(hay[pos]))) ++pos;
            if (hay.compare(pos, terms[t].size(), terms[t]) != 0) { ok = false; break; }
            pos += terms[t].size();
            if (t < last && pos < hay.size() && isWord(static_cast<unsigned char>(hay[pos]))) { ok = false; break; }
        }
        if (ok) {
            if (matchPos) *matchPos = at;
            return true;
        }
    }
    return false;
}

// Lower-case alphanumerics runs as words (UTF-8 letters/digits stay together:
// bytes >= 0x80 count as word letters). The query-side tokenizer for surfaces
// that match whole words.
inline std::vector<std::string> TokenizeWords(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u >= 0x80) {
            cur.push_back(static_cast<char>(std::tolower(u)));
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// True when `needle` STARTS a word in `hay` — the left side is a boundary and
// the needle fills to (or past) the word's end: "friend" in "friends" (the
// completion shape: the user just didn't type the trailing letters), not
// "friend" in "boyfriend" (mid-word). A word-start extension is a STRONG hit —
// the same word the user meant, plus an ending — while a mid-word substring is
// weak (probably a different word entirely).
inline bool StartsWord(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return false;
    const auto isWord = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
    for (size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + 1)) {
        if (at > 0 && isWord(static_cast<unsigned char>(hay[at - 1]))) continue;   // mid-word
        return true;   // a word starts here with the needle
    }
    return false;
}

// Bounded Levenshtein distance with an early exit: anything provably beyond
// `maxDist` returns maxDist+1 (the caller only cares whether the distance is
// within budget — this is a typo filter, not a metric). Fixed buffers (terms
// past kFuzzyMaxTerm chars are pastes, not typos — the fuzzy path never sees
// them) keep the scan allocation-free.
constexpr size_t kFuzzyMaxTerm = 24;
inline size_t EditDistanceBounded(std::string_view a, std::string_view b, size_t maxDist) {
    if (a == b) return 0;
    size_t hi = a.size(), lo = b.size();
    const std::string_view* longS = &a;
    const std::string_view* shortS = &b;
    if (hi < lo) { std::swap(hi, lo); longS = &b; shortS = &a; }
    if (hi - lo > maxDist || lo > kFuzzyMaxTerm) return maxDist + 1;
    std::array<size_t, kFuzzyMaxTerm + 1> prev{}, cur{};
    for (size_t j = 0; j <= lo; ++j) prev[j] = j;
    for (size_t i = 1; i <= hi; ++i) {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= lo; ++j) {
            const size_t cost = (*longS)[i - 1] == (*shortS)[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (cur[j] < rowMin) rowMin = cur[j];
        }
        if (rowMin > maxDist) return maxDist + 1;   // whole row beyond budget
        std::swap(prev, cur);
    }
    return prev[lo];
}

// Resolution of one query word against a word list (the engine's vocabulary,
// a media library's file names, a show's titles — anything): the shared rules
// every search surface applies.
//   exact word present        -> {"", 1.0}      (as typed — caller keeps it)
//   "friend" + list["friends"] -> {"friends", 1.0}   (incomplete word completed)
//   "frend" + list["friend"]   -> {"friend", 0.7}    (typo corrected)
//   nothing close              -> {"", 0.0}      (unresolvable — will not match)
// The completion goes to the most frequent list word the term prefixes; the
// typo path takes the closest word by bounded edit distance (ties by
// frequency), keeping the term's first letter and a ±maxDist length band —
// the overwhelmingly common typo shape, which holds even large lists to the
// sub-millisecond range.
struct ResolvedWord { std::string word; double weight = 0.0; };
inline ResolvedWord ResolveWordInList(const std::string& term,
                                      const std::vector<std::string>& words) {
    const auto count = [&](std::string_view w) {
        return static_cast<size_t>(std::count(words.begin(), words.end(), w));
    };
    if (count(term) > 0) return {std::string(), 1.0};   // spelled right: as typed
    // 1) Completion: the most frequent vocabulary word this term prefixes.
    std::string best;
    size_t bestN = 0;
    for (const std::string& w : words) {
        if (w.size() <= term.size() || w.compare(0, term.size(), term) != 0) continue;
        const size_t n = count(w);
        if (n > bestN) { bestN = n; best = w; }
    }
    if (!best.empty()) return {best, 1.0};   // a stem, not a typo: full weight
    // 2) Typo: closest vocabulary word by bounded edit distance. Equal-distance
    //    ties prefer the INSERTION shape — the query is the candidate minus one
    //    letter ("thn" -> "then", "frind" -> "friend"), the classic fast-typing
    //    dropped-letter error — over substitutions ("thn" -> "the"), which are
    //    usually a DIFFERENT common word that merely happens to be close. Then
    //    frequency decides what is left.
    const size_t maxDist = term.size() <= 4 ? 1 : 2;
    std::string typo;
    size_t bestDist = maxDist + 1, bestN2 = 0;
    bool bestInsert = false;
    const auto isInsertion = [&](std::string_view w) {
        if (w.size() != term.size() + 1) return false;
        size_t i = 0, j = 0;
        bool skipped = false;
        while (i < term.size() && j < w.size()) {
            if (term[i] == w[j]) { ++i; ++j; continue; }
            if (skipped) return false;
            skipped = true; ++j;
        }
        return true;
    };
    for (const std::string& w : words) {
        if (w.size() < 3 || w[0] != term[0]) continue;
        if (w.size() + maxDist < term.size() || w.size() > term.size() + maxDist) continue;
        const size_t d = EditDistanceBounded(term, w, bestDist);
        if (d > maxDist) continue;
        const bool insert = isInsertion(w);
        bool better = d < bestDist;
        if (!better && d == bestDist) {
            if (insert && !bestInsert) better = true;                       // dropped letter over substitution
            else if (insert == bestInsert && count(w) > bestN2) better = true;   // then frequency
        }
        if (better) {
            typo = w; bestDist = d; bestN2 = count(w); bestInsert = insert;
        }
    }
    if (!typo.empty()) return {typo, 0.7};
    return {};   // unresolvable
}

} // namespace bps::search::textmatch
