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
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

// The three things a search surface asks about one term in one paragraph —
// answered in a SINGLE walk of the text, because that walk is the hot loop of
// every search keystroke (the ranking pass asks it of every term in every
// paragraph). They used to be separate scans: a `find` for "does it occur at
// all", then ContainsWord walking the paragraph again, then StartsWord
// walking it a third time — up to three passes per term per paragraph, and a
// four-word query paid twelve. The walk stops at the first whole-word
// occurrence: a whole word IS a word start, so nothing later in the text can
// change either answer.
//   found  — the bytes occur somewhere (a mid-word substring counts)
//   whole  — ... as a complete word ("we" in "we", not in "went")
//   start  — ... at the START of a word ("friend" in "friends")
// `whole` implies `start`.
struct TermPresence {
    bool found = false;
    bool whole = false;
    bool start = false;
};

inline TermPresence PresenceOf(std::string_view hay, std::string_view needle) {
    TermPresence out;
    if (needle.empty()) return out;
    const auto isWord = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
    for (size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + 1)) {
        out.found = true;
        if (at > 0 && isWord(static_cast<unsigned char>(hay[at - 1]))) continue;   // mid-word
        const size_t end = at + needle.size();
        if (end >= hay.size() || !isWord(static_cast<unsigned char>(hay[end]))) {
            out.whole = true;
            out.start = true;
            return out;
        }
        out.start = true;   // a word starts here; it carries on past the needle
    }
    return out;
}

// True when `needle` occurs in `hay` as a WHOLE word: the characters around
// the occurrence are not letters/digits (so "we" does not match "went").
// UTF-8 continuation bytes (>= 0x80) count as word letters so multibyte words
// stay whole. One definition of the boundary rule: PresenceOf below is where
// it now lives, and the callers that need more than one answer per walk (the
// ranking passes) ask it once instead of twice.
inline bool ContainsWord(std::string_view hay, std::string_view needle) {
    return PresenceOf(hay, needle).whole;
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
//
// `lastWhole` closes the last term's mid-word tolerance — the EXACT-WORD
// searches set it so "my wife" stops matching "my wives": locked, the sentence
// must be there as typed, to the character.
inline bool ContainsPhrase(std::string_view hay, const std::vector<std::string>& terms,
                           size_t* matchPos = nullptr, bool lastWhole = false) {
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
            if ((t < last || lastWhole) && pos < hay.size()
                && isWord(static_cast<unsigned char>(hay[pos]))) { ok = false; break; }
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

// ---- WORD SPANS: the allocation-free half of TokenizeWords ---------------
// A hot scan must ask "which of these words does this paragraph carry" for tens
// of thousands of paragraphs per keystroke, and TokenizeWords answers it by
// building a std::string (its own heap block) per word — the allocation storm
// that made a common-word sermon search take seconds. These helpers split the
// same words into SPANS into the caller's own text instead, so a whole corpus
// scan allocates one reused vector and nothing else.
//
// Callers must pass text that is ALREADY lower-case (the lowered paragraphs are
// cached) and compare terms case-sensitively, exactly as the tokenizing
// variants do after their own lowering. A span's bytes are the same run
// TokenizeWords would have produced for that word.

// The byte class a word is made of — the SAME rule TokenizeWords uses
// (letters/digits plus UTF-8 continuation bytes, so a multibyte word stays one
// word).
inline bool IsWordByte(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

// Appends every word span of `segs` (one paragraph, or a paragraph and its
// neighbour for the spanned-match case) to `out`. `out` is NOT cleared: callers
// clear and reuse one vector across a whole scan.
inline void AppendWordSpans(const std::string_view* segs, size_t segCount,
                            std::vector<std::string_view>& out) {
    for (size_t s = 0; s < segCount; ++s) {
        const std::string_view text = segs[s];
        size_t i = 0;
        while (i < text.size()) {
            if (!IsWordByte(static_cast<unsigned char>(text[i]))) { ++i; continue; }
            const size_t start = i;
            while (i < text.size() && IsWordByte(static_cast<unsigned char>(text[i]))) ++i;
            out.emplace_back(text.substr(start, i - start));
        }
    }
}

// ContainsNearPhrase over ALREADY-SPLIT words: the identical rule and window,
// with no word vector built for the paragraph.
inline bool ContainsNearPhraseWords(const std::vector<std::string_view>& words,
                                    const std::vector<std::string>& terms,
                                    size_t maxGapWords) {
    if (terms.empty()) return false;
    if (words.size() < terms.size()) return false;
    for (size_t start = 0; start + terms.size() <= words.size(); ++start) {
        if (words[start] != terms[0]) continue;
        size_t pos = start;
        bool ok = true;
        for (size_t t = 1; t < terms.size(); ++t) {
            size_t next = std::string::npos;
            for (size_t j = pos + 1; j <= pos + 1 + maxGapWords && j < words.size(); ++j)
                if (words[j] == terms[t]) { next = j; break; }
            if (next == std::string::npos) { ok = false; break; }
            pos = next;
        }
        if (ok) return true;
    }
    return false;
}

// Does `words` carry terms[termStart .. termStart+termCount) IN ORDER, with up
// to `maxGapWords` other words between consecutive ones? The single test the
// EXACT-WORD ladder's window scan is built from (the caller loops the windows
// longest-first). Spans are compared as whole words — the same strictness the
// tokenizing version has.
inline bool ContainsTermRunWords(const std::vector<std::string_view>& words,
                                 const std::vector<std::string>& terms,
                                 size_t termStart, size_t termCount, size_t maxGapWords) {
    if (termCount == 0 || words.empty() || termStart + termCount > terms.size()) return false;
    for (size_t start = 0; start < words.size(); ++start) {
        if (words[start] != terms[termStart]) continue;
        size_t pos = start;
        bool matched = true;
        for (size_t t = 1; t < termCount; ++t) {
            const size_t end = std::min(words.size(), pos + 1 + maxGapWords + 1);
            size_t next = pos + 1;
            while (next < end && words[next] != terms[termStart + t]) ++next;
            if (next == end) { matched = false; break; }
            pos = next;
        }
        if (matched) return true;
    }
    return false;
}

// Like ContainsPhrase, but tolerates up to `maxGapWords` OTHER words between
// each consecutive pair of terms (ContainsPhrase itself allows only
// punctuation/whitespace there — zero gap). "compelling and compulsory,
// that" matches terms {compelling, compulsory, that} with maxGapWords >= 1
// even though the word "and" sits between the first two — a query that
// skipped a connector word the real text actually has must still read as
// the sentence, not as scattered words (user call: "because i missed some
// words in between ... if it can find a similar sentence['s] exact words").
// Bounded so a genuinely scattered match (the words spread across an entire
// paragraph) still does not count. Punctuation is never part of the match
// either way — both this and ContainsPhrase tokenize past it. Whole words
// only (terms are matched as complete tokens, same strictness ContainsPhrase
// gives EXACT-locked callers via lastWhole=true).
inline bool ContainsNearPhrase(std::string_view hay, const std::vector<std::string>& terms,
                               size_t maxGapWords) {
    if (terms.empty()) return false;
    const std::vector<std::string> words = TokenizeWords(hay);
    if (words.size() < terms.size()) return false;
    for (size_t start = 0; start + terms.size() <= words.size(); ++start) {
        if (words[start] != terms[0]) continue;
        size_t pos = start;
        bool ok = true;
        for (size_t t = 1; t < terms.size(); ++t) {
            size_t next = std::string::npos;
            for (size_t j = pos + 1; j <= pos + 1 + maxGapWords && j < words.size(); ++j)
                if (words[j] == terms[t]) { next = j; break; }
            if (next == std::string::npos) { ok = false; break; }
            pos = next;
        }
        if (ok) return true;
    }
    return false;
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

// True when `a` and `b` differ ONLY by one adjacent pair of letters swapped —
// the ie/ei slip ("recieve"/"receive", "thier"/"their", "freind"/"friend").
// Plain Levenshtein scores a swapped pair as TWO edits, and that is what let a
// one-letter substitution of a different word win the typo pass: typing
// "recieve" offered "relieve", searched for it, and found nothing the user
// meant. A swapped pair is one keystroke in the wrong order, so every typo pass
// counts it as one slip.
inline bool IsAdjacentTransposition(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    size_t first = std::string_view::npos, second = std::string_view::npos;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        if (first == std::string_view::npos) first = i;
        else if (second == std::string_view::npos) second = i;
        else return false;            // more than two differences: not one swap
    }
    // Exactly two differences, side by side, each holding the other's letter:
    // "recieve"/"receive" differ at 3 and 4 and agree everywhere else.
    return first != std::string_view::npos && second == first + 1
           && a[first] == b[second] && a[second] == b[first];
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
    // Word frequencies FIRST, in one pass. The two loops below ask how often a
    // candidate word occurs, and they ask it inside their own loops — counting
    // by scanning the whole vocabulary each time (std::count over a list that
    // holds 100k+ words) is what made a mistyped keystroke cost tens of
    // milliseconds. The table is built once and every count after it is a
    // lookup; the numbers are identical either way.
    std::unordered_map<std::string_view, uint32_t> freq;
    freq.reserve(words.size() * 2);
    for (const std::string& w : words) ++freq[std::string_view(w)];
    const auto count = [&](std::string_view w) -> size_t {
        const auto it = freq.find(w);
        return it == freq.end() ? 0 : it->second;
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
    //    ties prefer the ONE-SLIP shapes — the candidate is the query with one
    //    letter dropped ("thn" -> "then", "frind" -> "friend") or with an ADJACENT
    //    PAIR SWAPPED ("recieve" -> "receive") — over substitutions ("thn" ->
    //    "the"), which are usually a DIFFERENT common word that merely happens to
    //    be close. Then frequency decides what is left.
    //
    //    The transposition shape is called out separately because plain
    //    Levenshtein scores a swapped pair as TWO edits, so the real word lost to
    //    a one-letter neighbour: the user typed "recieve" and the search offered
    //    him "relieve" (one substitution), searched for that, and found nothing
    //    of what he meant. Swapping two adjacent letters is the ie/ei slip, one
    //    keystroke in the wrong order, and is counted as the single slip it is.
    const size_t maxDist = term.size() <= 4 ? 1 : 2;
    std::string typo;
    size_t bestDist = maxDist + 1, bestN2 = 0;
    bool bestSlip = false;
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
        const bool swap = IsAdjacentTransposition(term, w);
        const size_t d = swap ? 1 : EditDistanceBounded(term, w, bestDist);
        if (d > maxDist) continue;
        const bool slip = isInsertion(w) || swap;
        bool better = d < bestDist;
        if (!better && d == bestDist) {
            if (slip && !bestSlip) better = true;                        // a dropped letter or a swapped pair over a substitution
            else if (slip == bestSlip && count(w) > bestN2) better = true;   // then frequency
        }
        if (better) {
            typo = w; bestDist = d; bestN2 = count(w); bestSlip = slip;
        }
    }
    if (!typo.empty()) return {typo, 0.7};
    return {};   // unresolvable
}

} // namespace bps::search::textmatch
