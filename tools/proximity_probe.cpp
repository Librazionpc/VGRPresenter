// Standalone probe: the real TheTableLibrary::Search on the REAL library file,
// for the query "then friend". Prints every hit with its score so the ranking
// can be checked against the ground truth (para 9 holds the verbatim
// "Then, friends," — para 15/29 hold the words far apart).
#include <modules/library/TheTableLibrary.hpp>
#include <modules/search/SearchEngine.hpp>

#include <cstdio>
#include <string>

namespace bl = bps::library;
using bps::search::SearchEngine;

int main(int argc, char** argv) {
    const char* file = argc > 1 ? argv[1] : "the_table.json";
    const char* query = argc > 2 ? argv[2] : "then friend";

    auto lib = bl::TheTableLibrary::Open(file);
    if (!lib) { std::printf("Open failed\n"); return 1; }

    // The engine index is EMPTY in a standalone process until the library
    // registers its sermons (this is also what the app does at boot, on a
    // thread — here synchronously).
    lib->IndexWithSearchEngine();

    auto res = lib->Search(query, 12);
    if (!res.ok()) { std::printf("Search error\n"); return 1; }
    std::printf("query=%s  hits=%zu\n", query, res.value().size());
    for (const auto& h : res.value()) {
        std::printf("  score=%8.1f  %s  spanned=%d\n         snippet: %.90s\n",
                    h.score, h.reference.c_str(), h.spanned ? 1 : 0, h.snippet.c_str());
    }
    // Verdicts: does a verbatim paragraph exist, and where does it rank?
    int verbatimRank = -1;
    for (size_t i = 0; i < res.value().size(); ++i) {
        // The verbatim paragraph says "Then, friends, isn" right after "…might?"
        if (res.value()[i].snippet.find("Then, friends, isn") != std::string::npos)
            verbatimRank = static_cast<int>(i);
    }
    std::printf("VERDICT: verbatim 'Then, friends, isn' row rank=%d%s\n", verbatimRank,
                verbatimRank == 0 ? " (FIRST — correct)" :
                verbatimRank > 0   ? " (NOT first — ranking faulty)" :
                                     " (not in top hits)");
    return 0;
}
