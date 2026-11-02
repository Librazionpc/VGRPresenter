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

inline bool ContainsWord(std::string_view hay, std::string_view needle) {
    return PresenceOf(hay, needle).whole;
}

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

inline bool IsWordByte(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

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

struct ResolvedWord { std::string word; double weight = 0.0; };
inline ResolvedWord ResolveWordInList(const std::string& term,
                                      const std::vector<std::string>& words) {
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