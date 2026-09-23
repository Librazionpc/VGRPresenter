#include "modules/bible/BibleEngine.hpp"

#include "core/logging/Logger.hpp"
#include "modules/search/SearchEngine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>

namespace bps::bible {

namespace {

// Index adapter (docs/specs/24 §Search Integration): makes the Search Engine
// content-aware for type "bible". Verse documents carry their text as content;
// this adapter extracts it verbatim.
class BibleIndexAdapter final : public search::IIndexAdapter {
public:
    const char* Type() const noexcept override { return "bible"; }
    bool CanIndex(const search::SearchDocument& doc) const override {
        return doc.type == "bible";
    }
    Result<std::string> ExtractContent(const search::SearchDocument& doc) const override {
        return doc.content;
    }
};

std::string Lower(std::string_view s) {
    std::string out;
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string Slug(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "bible";
    return out;
}

// Canonical Search Engine document id for one verse:
// "bible:<bibleId>:<book>:<chapter>:<verse>".
std::string DocId(std::string_view bibleId, const BibleVerse& v) {
    return std::format("bible:{}:{}:{}:{}", bibleId, v.bookId, v.chapter, v.verse);
}

} // namespace

// PassageRef::ToString (declared in BibleTypes.hpp).
std::string PassageRef::ToString() const {
    std::string out = bookId;
    if (IsWholeBook()) return out;
    out += std::format(" {}", chapter);
    if (IsWholeChapter()) return out;
    out += std::format(":{}", verseStart);
    if (verseEnd != verseStart) out += std::format("-{}", verseEnd);
    return out;
}

BibleEngine& BibleEngine::Instance() {
    static BibleEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> BibleEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    if (providers_.empty()) {
        providers_.push_back(CreateXmlBibleProvider());
        providers_.push_back(CreateJsonBibleProvider());
        providers_.push_back(CreateUsfmBibleProvider());
        providers_.push_back(CreateOsisBibleProvider());
        providers_.push_back(CreatePlainTextBibleProvider());
    }
    WireEvents();
    return Ok();
}

Result<void> BibleEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> BibleEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> BibleEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        providers_.clear();
        bibles_.clear();
        notes_.clear();
        highlights_.clear();
        collections_.clear();
        initialized_.store(false);
    }
    return Ok();
}

Result<void> BibleEngine::Reload() {
    // Re-validate every installed Bible (docs/specs/24 §Import & Validation).
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, b] : bibles_) ids.push_back(id);
    }
    size_t failures = 0;
    for (const auto& id : ids) {
        auto bible = GetBible(id);
        if (!bible.ok()) continue;
        auto w = Validate(bible.value());
        if (!w.ok()) {
            ++failures;
            Logger::Instance().Warning("BibleEngine: re-validation failed for " + id +
                                           ": " + w.error().message, "BibleEngine");
        }
    }
    if (failures > 0) errorCount_.fetch_add(failures);
    return Ok();
}

Result<void> BibleEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    notes_.clear();
    highlights_.clear();
    collections_.clear();
    for (auto& [id, bible] : bibles_) bible.verses.clear();
    bibles_.clear();
    return Ok();
}

HealthReport BibleEngine::GetHealth() const {
    HealthReport h;
    h.state = errorCount_.load() > 0 ? HealthState::Degraded : HealthState::Healthy;
    h.errorCount = errorCount_.load();
    h.detail = std::format("{} bibles installed", bibles_.size());
    return h;
}

Metrics BibleEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = importCount_.load();
    m.health = GetHealth().state;
    return m;
}

// ---------------------------------------------------------------------------
// Provider registry
// ---------------------------------------------------------------------------
Result<void> BibleEngine::RegisterProvider(std::shared_ptr<IBibleProvider> provider) {
    if (!provider)
        return Error::Make(Err::InvalidArgument, "BibleEngine", "null provider");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : providers_)
        if (std::string(p->Name()) == provider->Name())
            return Error::Make(Err::AlreadyExists, "BibleEngine",
                               "provider already registered: " + std::string(provider->Name()));
    providers_.push_back(std::move(provider));
    return Ok();
}

Result<void> BibleEngine::UnregisterProvider(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = providers_.begin(); it != providers_.end(); ++it)
        if (std::string((*it)->Name()) == name) {
            providers_.erase(it);
            return Ok();
        }
    return Error::Make(Err::NotFound, "BibleEngine", "provider not found: " + std::string(name));
}

std::vector<std::string> BibleEngine::ProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& p : providers_) out.push_back(p->Name());
    return out;
}

// ---------------------------------------------------------------------------
// Import pipeline (docs/specs/24 §Import & Validation)
// ---------------------------------------------------------------------------
Result<std::string> BibleEngine::Import(std::string_view source, std::string_view format,
                                        const ImportOptions& options) {
    std::string fmt = Lower(format);
    if (fmt.empty())
        return Error::Make(Err::Bible_UnsupportedFormat, "BibleEngine", "no format given");

    std::shared_ptr<IBibleProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : providers_) {
            if (Lower(p->Format()) == fmt) { provider = p; break; }
            for (const auto& ext : p->SupportedExtensions())
                if (Lower(ext) == fmt) { provider = p; break; }
            if (provider) break;
        }
    }
    if (!provider)
        return Error::Make(Err::Bible_UnsupportedFormat, "BibleEngine",
                           "no provider for format: " + std::string(format));

    auto parsed = provider->Parse(source, fmt);
    if (!parsed.ok()) return parsed.error();

    BibleVersion bible = std::move(parsed.value());
    if (bible.metadata.id.empty()) {
        bible.metadata.id = !options.id.empty() ? options.id
                                                : Slug(bible.metadata.name.empty()
                                                           ? std::string(format)
                                                           : bible.metadata.name);
    }
    if (!options.id.empty()) bible.metadata.id = options.id;
    if (!options.name.empty()) bible.metadata.name = options.name;
    if (bible.metadata.name.empty()) bible.metadata.name = bible.metadata.id;
    if (bible.metadata.abbreviation.empty())
        bible.metadata.abbreviation = bible.metadata.id;

    auto warnings = Validate(bible);
    if (!warnings.ok()) return warnings.error();
    if (warnings.value() > 0)
        (void)EventBus::Instance().Publish(events::BibleValidationFailed{
            bible.metadata.id,
            std::format("import completed with {} warnings", warnings.value())});

    BibleVersion oldBible;
    bool hadOld = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bibles_.find(bible.metadata.id);
        if (it != bibles_.end()) {
            if (!options.replace)
                return Error::Make(Err::Bible_AlreadyExists, "BibleEngine",
                                   "bible already installed: " + bible.metadata.id);
            hadOld = true;
            oldBible = it->second;
        }
        bibles_[bible.metadata.id] = bible;
    }

    // Replacing a translation: drop its stale index documents first so the
    // global index never serves outdated verse text — whether or not this
    // import re-indexes (docs/specs/24 §Search).
    if (hadOld) {
        auto& search = search::SearchEngine::Instance();
        for (const auto& v : oldBible.verses)
            (void)search.RemoveDocument(DocId(bible.metadata.id, v));
    }
    if (options.index) {
        auto indexed = IndexBible(bible,
                                  options.onProgress ? &options.onProgress : nullptr);
        if (indexed.ok())
            (void)EventBus::Instance().Publish(
                events::BibleIndexed{bible.metadata.id, indexed.value()});
        else
            Logger::Instance().Warning("BibleEngine: indexing failed for " +
                                           bible.metadata.id + ": " +
                                           indexed.error().message, "BibleEngine");
    }

    importCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(
        events::BibleImported{bible.metadata.id, bible.metadata.name, bible.verses.size()});
    (void)EventBus::Instance().Publish(
        events::BibleValidationCompleted{bible.metadata.id, warnings.value()});
    Logger::Instance().Info("Bible imported: " + bible.metadata.id + " (" +
                                std::format("{} verses)", bible.verses.size()), "BibleEngine");
    return bible.metadata.id;
}

Result<size_t> BibleEngine::Validate(const BibleVersion& bible) const {
    size_t warnings = 0;
    if (bible.metadata.id.empty())
        return Error::Make(Err::Bible_ValidationFailed, "BibleEngine",
                           "translation id is empty");
    if (bible.books.empty())
        return Error::Make(Err::Bible_ValidationFailed, "BibleEngine",
                           "bible has no books");
    if (bible.verses.empty())
        return Error::Make(Err::Bible_ValidationFailed, "BibleEngine",
                           "bible has no verses");
    for (const auto& v : bible.verses) {
        if (v.chapter <= 0 || v.verse <= 0) {
            ++warnings;
            continue;
        }
        bool knownBook = false;
        for (const auto& b : bible.books)
            if (b.id == v.bookId) { knownBook = true; break; }
        if (!knownBook) ++warnings;
        if (v.text.empty()) ++warnings;
    }
    return warnings;
}

Result<size_t> BibleEngine::IndexBible(const BibleVersion& bible,
                                       const std::function<void(size_t, size_t, std::string_view)>* onProgress) {
    // Incremental upsert per verse (docs/specs/24 §Search): importing a Bible
    // must never clobber the global index — other Bibles, songs, and
    // presentations stay searchable alongside this one.
    auto& search = search::SearchEngine::Instance();
    size_t indexed = 0;
    // Note: on error the loop aborts (partial index) — Import logs the failure
    // and the installed Bible remains readable through the in-memory store.
    for (const auto& v : bible.verses) {
        search::SearchDocument doc;
        PassageRef ref;
        ref.bookId = v.bookId;
        ref.chapter = v.chapter;
        ref.verseStart = v.verse;
        ref.verseEnd = v.verse;
        doc.id = DocId(bible.metadata.id, v);
        doc.type = "bible";
        doc.title = ref.ToString();
        doc.content = v.text;
        // The author field carries the stable bible id so filters (type + author)
        // can scope searches to one translation.
        doc.author = bible.metadata.id;
        doc.language = bible.metadata.language;
        doc.source = bible.metadata.id;
        doc.metadata["bible"] = bible.metadata.id;
        doc.metadata["book"] = v.bookId;
        doc.metadata["chapter"] = std::format("{}", v.chapter);
        doc.metadata["verse"] = std::format("{}", v.verse);
        doc.metadata["reference"] = ref.ToString();
        doc.tags = {"scripture", Lower(bible.metadata.id)};
        auto r = search.IndexDocument(doc);
        if (!r.ok()) return r.error();
        ++indexed;
        if (onProgress && *onProgress)
            (*onProgress)(indexed, bible.verses.size(), v.bookId);
    }
    return indexed;
}

Result<void> BibleEngine::RemoveBible(std::string_view bibleId) {
    BibleVersion removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bibles_.find(std::string(bibleId));
        if (it == bibles_.end())
            return Error::Make(Err::Bible_NotFound, "BibleEngine",
                               "bible not found: " + std::string(bibleId));
        removed = std::move(it->second);
        bibles_.erase(it);
    }
    // Drop this translation's documents from the global search index.
    auto& search = search::SearchEngine::Instance();
    for (const auto& v : removed.verses) (void)search.RemoveDocument(DocId(bibleId, v));
    (void)EventBus::Instance().Publish(events::BibleRemoved{std::string(bibleId)});
    return Ok();
}

std::vector<std::string> BibleEngine::BibleIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, b] : bibles_) out.push_back(id);
    return out;
}

size_t BibleEngine::BibleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bibles_.size();
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------
Result<BibleVersion> BibleEngine::GetBible(std::string_view bibleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bibles_.find(std::string(bibleId));
    if (it == bibles_.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine",
                           "bible not found: " + std::string(bibleId));
    return it->second;
}

Result<BibleBook> BibleEngine::GetBook(std::string_view bibleId, std::string_view bookId) const {
    auto bible = GetBible(bibleId);
    if (!bible.ok()) return bible.error();
    for (const auto& b : bible.value().books)
        if (b.id == bookId) return b;
    return Error::Make(Err::Bible_NotFound, "BibleEngine",
                       "book not found: " + std::string(bookId));
}

Result<std::vector<BibleVerse>> BibleEngine::GetPassage(std::string_view bibleId,
                                                        const PassageRef& ref) const {
    auto bible = GetBible(bibleId);
    if (!bible.ok()) return bible.error();
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");

    std::vector<BibleVerse> out;
    if (ref.IsWholeBook()) {
        for (const auto& [key, ch] : bible.value().chapters) {
            if (key.starts_with(ref.bookId + "."))
                out.insert(out.end(), ch.verses.begin(), ch.verses.end());
        }
        if (out.empty())
            return Error::Make(Err::Bible_NotFound, "BibleEngine",
                               "book not found: " + ref.bookId);
        return out;
    }
    auto chIt = bible.value().chapters.find(std::format("{}.{}", ref.bookId, ref.chapter));
    if (chIt == bible.value().chapters.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine",
                           "chapter not found: " + ref.ToString());
    if (ref.IsWholeChapter()) return chIt->second.verses;
    for (const auto& v : chIt->second.verses)
        if (v.verse >= ref.verseStart && v.verse <= ref.verseEnd)
            out.push_back(v);
    if (out.empty())
        return Error::Make(Err::Bible_NotFound, "BibleEngine",
                           "verse not found: " + ref.ToString());
    return out;
}

Result<BibleVerse> BibleEngine::GetVerse(std::string_view bibleId, std::string_view bookId,
                                         int chapter, int verse) const {
    PassageRef ref;
    ref.bookId = std::string(bookId);
    ref.chapter = chapter;
    ref.verseStart = verse;
    ref.verseEnd = verse;
    auto passage = GetPassage(bibleId, ref);
    if (!passage.ok()) return passage.error();
    return passage.value().front();
}

Result<TranslationMetadata> BibleEngine::Metadata(std::string_view bibleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bibles_.find(std::string(bibleId));
    if (it == bibles_.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine", "bible not found: " + std::string(bibleId));
    return it->second.metadata;
}

Result<std::vector<BookOutline>> BibleEngine::Outline(std::string_view bibleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bibles_.find(std::string(bibleId));
    if (it == bibles_.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine", "bible not found: " + std::string(bibleId));
    // The chapters are keyed "GEN.10" (sorted as text), so group them per book and order them as numbers.
    std::map<std::string, std::vector<std::pair<int, int>>, std::less<>> perBook;
    for (const auto& [key, chapter] : it->second.chapters)
        perBook[chapter.bookId].emplace_back(chapter.number, static_cast<int>(chapter.verses.size()));
    std::vector<BookOutline> out;
    for (const BibleBook& book : it->second.books) {
        auto found = perBook.find(book.id);
        if (found == perBook.end() || found->second.empty()) continue;   // a book the Bible does not contain
        std::sort(found->second.begin(), found->second.end());
        BookOutline o;
        o.book = book;
        for (const auto& [number, verses] : found->second) {
            o.chapters.push_back(number);
            o.verseCounts.push_back(verses);
        }
        out.push_back(std::move(o));
    }
    return out;
}

Result<BibleChapter> BibleEngine::GetChapter(std::string_view bibleId, std::string_view bookId, int chapter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bibles_.find(std::string(bibleId));
    if (it == bibles_.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine", "bible not found: " + std::string(bibleId));
    auto ch = it->second.chapters.find(std::format("{}.{}", bookId, chapter));
    if (ch == it->second.chapters.end())
        return Error::Make(Err::Bible_NotFound, "BibleEngine", std::format("chapter not found: {} {}", bookId, chapter));
    return ch->second;
}

Result<size_t> BibleEngine::VerseCount(std::string_view bibleId) const {
    auto bible = GetBible(bibleId);
    if (!bible.ok()) return bible.error();
    return bible.value().verses.size();
}

// ---------------------------------------------------------------------------
// Reference resolution
// ---------------------------------------------------------------------------
std::vector<BibleBook> BibleEngine::BooksFor(std::string_view bibleId) const {
    if (!bibleId.empty()) {
        if (auto bible = GetBible(bibleId); bible.ok())
            return bible.value().books;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bibles_.empty()) return bibles_.begin()->second.books;
    return {};
}

Result<PassageRef> BibleEngine::ResolveReference(std::string_view text,
                                                 std::string_view bibleId) const {
    resolveCount_.fetch_add(1);
    auto ref = ReferenceResolver::Resolve(text, BooksFor(bibleId));
    if (ref.ok())
        (void)EventBus::Instance().Publish(events::BiblePassageResolved{
            std::string(text), ref.value().bookId, ref.value().chapter,
            ref.value().verseStart, ref.value().verseEnd});
    return ref;
}

Result<std::vector<PassageRef>> BibleEngine::ResolveReferences(std::string_view text,
                                                               std::string_view bibleId) const {
    return ReferenceResolver::ResolveAll(text, BooksFor(bibleId));
}

// ---------------------------------------------------------------------------
// Search (docs/specs/24 §Search Integration)
// ---------------------------------------------------------------------------
Result<std::vector<BibleSearchHit>> BibleEngine::Search(std::string_view query,
                                                        std::string_view bibleId) const {
    std::vector<BibleSearchHit> hits;
    // 1. Reference resolution: "John 3:16" is a lookup, not a text search.
    if (auto refs = ResolveReferences(query, bibleId); refs.ok()) {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!bibleId.empty()) {
                ids.push_back(std::string(bibleId));
            } else {
                for (const auto& [id, b] : bibles_) ids.push_back(id);
            }
        }
        for (const auto& id : ids) {
            for (const auto& ref : refs.value()) {
                if (auto passage = GetPassage(id, ref); passage.ok()) {
                    for (const auto& v : passage.value()) {
                        BibleSearchHit hit;
                        hit.bibleId = id;
                        PassageRef r = ref;
                        r.verseStart = v.verse;
                        r.verseEnd = v.verse;
                        hit.reference = r.ToString();
                        hit.bookId = v.bookId;
                        hit.chapter = v.chapter;
                        hit.verse = v.verse;
                        hit.snippet = v.text;
                        hit.score = 100.0;   // exact reference beats text ranking
                        hits.push_back(std::move(hit));
                    }
                }
            }
        }
    }

    // 2. Full-text search against actual verse content.
    auto& search = search::SearchEngine::Instance();
    search::SearchFilter filter;
    filter.type = "bible";
    if (!bibleId.empty()) filter.author = std::string(bibleId);
    if (auto results = search.Search(query, filter, 50); results.ok()) {
        for (const auto& r : results.value()) {
            // Document id: bible:<bible>:<book>:<chapter>:<verse>
            size_t p1 = r.documentId.find(':');
            size_t p2 = p1 == std::string::npos ? std::string::npos : r.documentId.find(':', p1 + 1);
            size_t p3 = p2 == std::string::npos ? std::string::npos : r.documentId.find(':', p2 + 1);
            size_t p4 = p3 == std::string::npos ? std::string::npos : r.documentId.find(':', p3 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos ||
                p4 == std::string::npos)
                continue;
            BibleSearchHit hit;
            hit.bibleId = r.documentId.substr(p1 + 1, p2 - p1 - 1);
            hit.bookId = r.documentId.substr(p2 + 1, p3 - p2 - 1);
            hit.chapter = std::atoi(r.documentId.substr(p3 + 1, p4 - p3 - 1).c_str());
            hit.verse = std::atoi(r.documentId.substr(p4 + 1).c_str());
            hit.reference = std::format("{} {}:{}", hit.bookId, hit.chapter, hit.verse);
            hit.snippet = r.snippet.empty() ? r.title : r.snippet;
            hit.score = r.score;
            // Deduplicate against reference hits.
            bool dup = false;
            for (const auto& h : hits)
                if (h.bibleId == hit.bibleId && h.bookId == hit.bookId &&
                    h.chapter == hit.chapter && h.verse == hit.verse)
                    dup = true;
            if (!dup) hits.push_back(std::move(hit));
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const BibleSearchHit& a, const BibleSearchHit& b) {
        return a.score > b.score;
    });
    (void)EventBus::Instance().Publish(
        events::BibleSearchCompleted{std::string(query), hits.size()});
    return hits;
}

// ---------------------------------------------------------------------------
// Parallel Bible (docs/specs/24 §Parallel Bible Support)
// ---------------------------------------------------------------------------
Result<std::vector<ParallelVerse>> BibleEngine::Compare(
    std::string_view refText, const std::vector<std::string>& bibleIds) const {
    auto refs = ResolveReferences(refText, bibleIds.empty() ? "" : bibleIds.front());
    if (!refs.ok()) return refs.error();

    std::vector<ParallelVerse> out;
    for (const auto& ref : refs.value()) {
        // Determine the verse span to align.
        if (ref.IsWholeBook() || ref.IsWholeChapter()) {
            auto first = GetPassage(bibleIds.front(), ref);
            if (!first.ok()) continue;
            int maxVerse = 0;
            for (const auto& v : first.value()) maxVerse = std::max(maxVerse, v.verse);
            for (int v = 1; v <= maxVerse; ++v) {
                PassageRef single = ref;
                single.verseStart = v;
                single.verseEnd = v;
                for (const auto& id : bibleIds) {
                    ParallelVerse pv;
                    pv.bibleId = id;
                    pv.reference = single.ToString();
                    if (auto passage = GetPassage(id, single); passage.ok() && !passage.value().empty()) {
                        pv.present = true;
                        pv.text = passage.value().front().text;
                    }
                    out.push_back(std::move(pv));
                }
            }
            continue;
        }
        for (int v = ref.verseStart; v <= ref.verseEnd; ++v) {
            PassageRef single = ref;
            single.verseStart = v;
            single.verseEnd = v;
            for (const auto& id : bibleIds) {
                ParallelVerse pv;
                pv.bibleId = id;
                pv.reference = single.ToString();
                if (auto passage = GetPassage(id, single); passage.ok() && !passage.value().empty()) {
                    pv.present = true;
                    pv.text = passage.value().front().text;
                }
                out.push_back(std::move(pv));
            }
        }
    }
    if (out.empty())
        return Error::Make(Err::Bible_NotFound, "BibleEngine", "nothing to compare");
    return out;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
Result<std::string> BibleEngine::Format(std::string_view bibleId, const PassageRef& ref,
                                        const FormatOptions& options) const {
    auto bible = GetBible(bibleId);
    if (!bible.ok()) return bible.error();
    return BibleFormatter::Format(bible.value(), ref, options);
}

// ---------------------------------------------------------------------------
// User data (docs/specs/24 §User Data) — always keyed by canonical reference,
// never stored inside Scripture.
// ---------------------------------------------------------------------------
std::string BibleEngine::RefKey(const PassageRef& ref) {
    std::string key = std::format("{}:{}", ref.bookId, ref.chapter);
    if (ref.IsWholeChapter() || ref.IsWholeBook()) return key;
    key += std::format(":{}-{}", ref.verseStart, ref.verseEnd);
    return key;
}

Result<void> BibleEngine::AddNote(std::string_view bibleId, const PassageRef& ref,
                                  const std::string& text) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");
    if (text.empty())
        return Error::Make(Err::InvalidArgument, "BibleEngine", "note text is empty");
    UserNote note;
    note.id = std::format(
        "note-{}",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    note.ref = ref;
    note.text = text;
    note.createdMs = note.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    std::lock_guard<std::mutex> lock(mutex_);
    notes_[std::string(bibleId) + "|" + RefKey(ref)].push_back(std::move(note));
    return Ok();
}

Result<std::vector<UserNote>> BibleEngine::Notes(std::string_view bibleId,
                                                 const PassageRef& ref) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = notes_.find(std::string(bibleId) + "|" + RefKey(ref));
    return it == notes_.end() ? std::vector<UserNote>() : it->second;
}

Result<void> BibleEngine::SetHighlight(std::string_view bibleId, const PassageRef& ref,
                                       bool on) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");
    std::lock_guard<std::mutex> lock(mutex_);
    auto& list = highlights_[std::string(bibleId)];
    auto it = std::find_if(list.begin(), list.end(), [&](const PassageRef& r) {
        return RefKey(r) == RefKey(ref);
    });
    if (on && it == list.end())
        list.push_back(ref);
    else if (!on && it != list.end())
        list.erase(it);
    return Ok();
}

Result<std::vector<PassageRef>> BibleEngine::Highlights(std::string_view bibleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = highlights_.find(std::string(bibleId));
    return it == highlights_.end() ? std::vector<PassageRef>() : it->second;
}

Result<void> BibleEngine::AddCollection(std::string_view name) {
    if (std::string(name).empty())
        return Error::Make(Err::InvalidArgument, "BibleEngine", "collection name is empty");
    std::lock_guard<std::mutex> lock(mutex_);
    collections_.emplace(std::string(name), std::vector<PassageRef>{});
    return Ok();
}

Result<void> BibleEngine::AddToCollection(std::string_view collection, const PassageRef& ref) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = collections_.find(std::string(collection));
    if (it == collections_.end())
        return Error::Make(Err::NotFound, "BibleEngine",
                           "collection not found: " + std::string(collection));
    it->second.push_back(ref);
    return Ok();
}

Result<std::vector<PassageRef>> BibleEngine::Collection(std::string_view collection) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = collections_.find(std::string(collection));
    if (it == collections_.end())
        return Error::Make(Err::NotFound, "BibleEngine",
                           "collection not found: " + std::string(collection));
    return it->second;
}

std::vector<std::string> BibleEngine::CollectionNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, refs] : collections_) out.push_back(name);
    return out;
}

// ---------------------------------------------------------------------------
// Cross references
// ---------------------------------------------------------------------------
Result<std::vector<CrossReference>> BibleEngine::CrossReferences(std::string_view bibleId,
                                                                 const PassageRef& ref) const {
    auto bible = GetBible(bibleId);
    if (!bible.ok()) return bible.error();
    if (ref.IsWholeBook() || ref.IsWholeChapter()) {
        std::vector<CrossReference> out;
        if (ref.IsWholeChapter()) {
            auto chIt = bible.value().chapters.find(std::format("{}.{}", ref.bookId, ref.chapter));
            if (chIt == bible.value().chapters.end()) return out;
            for (const auto& v : chIt->second.verses)
                out.insert(out.end(), v.crossRefs.begin(), v.crossRefs.end());
        }
        return out;
    }
    auto passage = GetPassage(bibleId, ref);
    if (!passage.ok()) return passage.error();
    std::vector<CrossReference> out;
    for (const auto& v : passage.value())
        out.insert(out.end(), v.crossRefs.begin(), v.crossRefs.end());
    return out;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void BibleEngine::WireEvents() {
    if (!subscriptions_.empty()) return;
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
}

void BibleEngine::UnwireEvents() {
    for (const auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

void BibleEngine::OnConfigReload(const events::ConfigHotReload&) {
    (void)Reload();
}

} // namespace bps::bible
