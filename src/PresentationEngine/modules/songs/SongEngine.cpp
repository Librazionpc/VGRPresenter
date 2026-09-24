#include "modules/songs/SongEngine.hpp"

#include "core/logging/Logger.hpp"
#include "modules/search/SearchEngine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <format>

namespace bps::song {

namespace {

// Index adapter (docs/specs/25 §Search Integration): makes the Search Engine
// content-aware for type "song".
class SongIndexAdapter final : public search::IIndexAdapter {
public:
    const char* Type() const noexcept override { return "song"; }
    bool CanIndex(const search::SearchDocument& doc) const override {
        return doc.type == "song";
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
    return out;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Thresholds (docs/specs/25 §Duplicate Detection): imports reject near-identical
// songs; FindDuplicates surfaces likely duplicates from a wider net.
constexpr double kDuplicateImportThreshold = 0.70;
constexpr double kDuplicateReportThreshold = 0.50;

// Similarity is capped to the first kSimilarityChars characters per song so
// duplicate detection stays fast on very large libraries.
constexpr size_t kSimilarityChars = 400;

} // namespace

SongEngine& SongEngine::Instance() {
    static SongEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> SongEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    if (providers_.empty()) {
        providers_.push_back(CreateChordProProvider());
        providers_.push_back(CreateOpenSongProvider());
        providers_.push_back(CreateOpenLpProvider());
        providers_.push_back(CreateProPresenterProvider());
        providers_.push_back(CreateEasyWorshipProvider());
        providers_.push_back(CreatePlainTextSongProvider());
        providers_.push_back(CreateJsonSongProvider());
    }
    WireEvents();
    return Ok();
}

Result<void> SongEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> SongEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> SongEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        providers_.clear();
        songs_.clear();
        versions_.clear();
        collections_.clear();
        initialized_.store(false);
    }
    return Ok();
}

Result<void> SongEngine::Reload() {
    // Re-validate every stored song (docs/specs/25 §Import & Validation).
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, s] : songs_) ids.push_back(id);
    }
    size_t failures = 0;
    for (const auto& id : ids) {
        auto song = GetSong(id);
        if (!song.ok()) continue;
        if (!Validate(song.value()).ok()) ++failures;
    }
    if (failures > 0) errorCount_.fetch_add(failures);
    return Ok();
}

Result<void> SongEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    songs_.clear();
    versions_.clear();
    collections_.clear();
    return Ok();
}

HealthReport SongEngine::GetHealth() const {
    HealthReport h;
    h.state = errorCount_.load() > 0 ? HealthState::Degraded : HealthState::Healthy;
    h.errorCount = errorCount_.load();
    h.detail = std::format("{} songs stored", songs_.size());
    return h;
}

Metrics SongEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = importCount_.load();
    m.health = GetHealth().state;
    return m;
}

// ---------------------------------------------------------------------------
// Provider registry
// ---------------------------------------------------------------------------
Result<void> SongEngine::RegisterProvider(std::shared_ptr<ISongProvider> provider) {
    if (!provider)
        return Error::Make(Err::InvalidArgument, "SongEngine", "null provider");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : providers_)
        if (std::string(p->Name()) == provider->Name())
            return Error::Make(Err::AlreadyExists, "SongEngine",
                               "provider already registered: " + std::string(provider->Name()));
    providers_.push_back(std::move(provider));
    return Ok();
}

Result<void> SongEngine::UnregisterProvider(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = providers_.begin(); it != providers_.end(); ++it)
        if (std::string((*it)->Name()) == name) {
            providers_.erase(it);
            return Ok();
        }
    return Error::Make(Err::NotFound, "SongEngine", "provider not found: " + std::string(name));
}

std::vector<std::string> SongEngine::ProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& p : providers_) out.push_back(p->Name());
    return out;
}

// ---------------------------------------------------------------------------
// Import pipeline (docs/specs/25 §Import)
// ---------------------------------------------------------------------------
Result<std::string> SongEngine::Import(std::string_view source, std::string_view format,
                                       const ImportOptions& options) {
    std::string fmt = Lower(format);
    if (fmt.empty())
        return Error::Make(Err::Song_UnsupportedFormat, "SongEngine", "no format given");

    // Collect every provider claiming this format — format-name matches first,
    // then extension-only claims — and try each until one parses: several song
    // formats share ".xml" (OpenSong, OpenLP, EasyWorship), so the first
    // claimant must not have the last word.
    std::vector<std::shared_ptr<ISongProvider>> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : providers_)
            if (Lower(p->Format()) == fmt) candidates.push_back(p);
        for (const auto& p : providers_) {
            if (Lower(p->Format()) == fmt) continue;
            for (const auto& ext : p->SupportedExtensions()) {
                // Extensions carry a leading dot (".xml"); the requested
                // format is the bare suffix ("xml") — compare without it.
                std::string e = Lower(ext);
                if (!e.empty() && e.front() == '.') e.erase(0, 1);
                if (e == fmt) { candidates.push_back(p); break; }
            }
        }
    }
    if (candidates.empty())
        return Error::Make(Err::Song_UnsupportedFormat, "SongEngine",
                           "no provider for format: " + std::string(format));

    Result<Song> parsed = Error::Make(Err::Song_UnsupportedFormat, "SongEngine",
                                      "no provider could parse format: " +
                                          std::string(format));
    for (const auto& provider : candidates) {
        parsed = provider->Parse(source, fmt);
        if (parsed.ok()) break;
    }
    if (!parsed.ok()) return parsed.error();
    Song song = std::move(parsed.value());
    // Some interchange formats carry no title; a library entry still needs one
    // (docs/specs/25 §Validation) — the engine supplies a neutral fallback and
    // surfaces it as a warning instead of rejecting the import.
    if (song.metadata.title.empty()) song.metadata.title = "Untitled";

    auto warnings = Validate(song);
    if (!warnings.ok()) return warnings.error();

    // Duplicate detection at import (docs/specs/25 §Duplicate Detection):
    // never blindly create "Amazing Grace (2)".
    if (options.checkDuplicates) {
        if (auto dup = ScoredDuplicates(song); dup.ok() && !dup.value().empty() &&
            dup.value().front().second >= kDuplicateImportThreshold)
            return Error::Make(Err::Song_Duplicate, "SongEngine",
                               "likely duplicate of song: " + dup.value().front().first);
    }

    if (song.id.empty() || song.id == "chordpro-import" || song.id == "song-json") {
        song.id = !options.id.empty() ? options.id : Slug(song.metadata.title) + "-" +
                                                       std::to_string(NowMs() % 100000);
    }
    if (song.metadata.title.empty())
        return Error::Make(Err::Song_ValidationFailed, "SongEngine", "song has no title");
    EnsureOriginalArrangement(song);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (songs_.count(song.id) > 0)
            return Error::Make(Err::Song_AlreadyExists, "SongEngine",
                               "song already registered: " + song.id);
        songs_[song.id] = song;
        SnapshotLocked(song);
    }

    if (options.index) {
        auto indexed = IndexSong(song);
        if (indexed.ok())
            (void)EventBus::Instance().Publish(events::SongIndexed{song.id, indexed.value()});
        else
            Logger::Instance().Warning("SongEngine: indexing failed for " + song.id + ": " +
                                           indexed.error().message, "SongEngine");
    }

    importCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::SongImported{song.id, song.metadata.title});
    (void)EventBus::Instance().Publish(events::SongValidated{song.id, warnings.value()});
    Logger::Instance().Info("Song imported: \"" + song.metadata.title + "\" (" + song.id + ")",
                            "SongEngine");
    return song.id;
}

Result<size_t> SongEngine::Validate(const Song& song) const {
    size_t warnings = 0;
    if (song.metadata.title.empty())
        return Error::Make(Err::Song_ValidationFailed, "SongEngine", "song has no title");
    if (song.sections.empty())
        return Error::Make(Err::Song_ValidationFailed, "SongEngine", "song has no sections");
    for (const auto& sec : song.sections) {
        if (sec.id.empty() || sec.name.empty()) ++warnings;
        for (const auto& line : sec.lines)
            if (line.lyrics.empty() && line.chords.empty()) ++warnings;
    }
    // Arrangements must reference existing sections.
    for (const auto& arr : song.arrangements) {
        for (const auto& sid : arr.sectionIds) {
            bool found = false;
            for (const auto& sec : song.sections)
                if (sec.id == sid) { found = true; break; }
            if (!found) ++warnings;
        }
    }
    return warnings;
}

Result<size_t> SongEngine::IndexSong(const Song& song) {
    auto& search = search::SearchEngine::Instance();
    search::SearchDocument doc;
    doc.id = "song:" + song.id;
    doc.type = "song";
    doc.title = song.metadata.title;
    doc.author = song.metadata.authors.empty() ? "" : song.metadata.authors.front();
    doc.language = song.metadata.language;
    doc.source = "song";
    std::string content;
    for (const auto& sec : song.sections) {
        content += sec.name + " ";
        for (const auto& line : sec.lines) {
            content += line.lyrics + " ";
            for (const auto& cr : line.chords) content += cr.chord.Display() + " ";
        }
    }
    for (const auto& a : song.metadata.authors) content += a + " ";
    for (const auto& t : song.metadata.tags) content += t + " ";
    doc.content = content;
    doc.metadata["song"] = song.id;
    doc.metadata["key"] = song.metadata.performanceKey.empty()
                              ? song.metadata.originalKey
                              : song.metadata.performanceKey;
    doc.tags = song.metadata.tags;
    auto indexed = search.IndexDocument(doc);
    return indexed.ok() ? Result<size_t>(1) : Result<size_t>(indexed.error());
}

Result<void> SongEngine::RemoveSong(std::string_view songId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (songs_.erase(std::string(songId)) == 0)
            return Error::Make(Err::Song_NotFound, "SongEngine",
                               "song not found: " + std::string(songId));
        versions_.erase(std::string(songId));
    }
    (void)search::SearchEngine::Instance().RemoveDocument("song:" + std::string(songId));
    (void)EventBus::Instance().Publish(events::SongDeleted{std::string(songId)});
    return Ok();
}

std::vector<std::string> SongEngine::SongIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, s] : songs_) out.push_back(id);
    return out;
}

size_t SongEngine::SongCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return songs_.size();
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------
Result<std::string> SongEngine::CreateSong(const Song& song) {
    auto warnings = Validate(song);
    if (!warnings.ok()) return warnings.error();
    Song stored = song;
    if (stored.id.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine", "song id is required");
    if (stored.createdMs == 0) stored.createdMs = NowMs();
    stored.modifiedMs = NowMs();
    EnsureOriginalArrangement(stored);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (songs_.count(stored.id) > 0)
            return Error::Make(Err::Song_AlreadyExists, "SongEngine",
                               "song already registered: " + stored.id);
        songs_[stored.id] = stored;
        SnapshotLocked(stored);
    }
    (void)IndexSong(stored);
    importCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::SongImported{stored.id, stored.metadata.title});
    return stored.id;
}

Result<Song> SongEngine::GetSong(std::string_view songId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = songs_.find(std::string(songId));
    if (it == songs_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    return it->second;
}

Result<void> SongEngine::UpdateSong(std::string_view songId, const Song& song) {
    auto warnings = Validate(song);
    if (!warnings.ok()) return warnings.error();
    Song updated = song;
    updated.id = std::string(songId);
    updated.modifiedMs = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = songs_.find(std::string(songId));
        if (it == songs_.end())
            return Error::Make(Err::Song_NotFound, "SongEngine",
                               "song not found: " + std::string(songId));
        if (it->second.createdMs != 0) updated.createdMs = it->second.createdMs;
        SnapshotLocked(it->second);   // previous state is recoverable
        it->second = updated;
    }
    (void)IndexSong(updated);
    (void)EventBus::Instance().Publish(events::SongUpdated{updated.id, updated.metadata.title});
    return Ok();
}

Result<std::vector<Song>> SongEngine::Search(std::string_view query) const {
    auto& search = search::SearchEngine::Instance();
    search::SearchFilter filter;
    filter.type = "song";
    auto results = search.Search(query, filter, 50);
    if (!results.ok()) return results.error();
    std::vector<Song> out;
    for (const auto& r : results.value()) {
        if (r.documentId.rfind("song:", 0) != 0) continue;
        auto song = GetSong(r.documentId.substr(5));
        if (song.ok()) out.push_back(song.value());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transposition (docs/specs/25 §Transposition)
// ---------------------------------------------------------------------------
Result<Song> SongEngine::Transposed(std::string_view songId, int semitones) const {
    auto song = GetSong(songId);
    if (!song.ok()) return song.error();
    Song out = song.value();
    for (auto& sec : out.sections)
        for (auto& line : sec.lines)
            for (auto& cr : line.chords) {
                auto t = ChordSystem::Transpose(cr.chord, semitones);
                if (t.ok()) cr.chord = t.value();
            }
    if (!out.metadata.originalKey.empty()) {
        if (auto k = ChordSystem::TransposeKey(out.metadata.originalKey, semitones); k.ok())
            out.metadata.originalKey = k.value();
    }
    if (!out.metadata.performanceKey.empty()) {
        if (auto k = ChordSystem::TransposeKey(out.metadata.performanceKey, semitones); k.ok())
            out.metadata.performanceKey = k.value();
    }
    if (!out.metadata.preferredKey.empty()) {
        if (auto k = ChordSystem::TransposeKey(out.metadata.preferredKey, semitones); k.ok())
            out.metadata.preferredKey = k.value();
    }
    return out;
}

Result<void> SongEngine::SetPerformanceKey(std::string_view songId, std::string_view key) {
    auto normalized = ChordSystem::NormalizeRoot(key);
    if (!normalized.ok())
        return Error::Make(Err::InvalidArgument, "SongEngine",
                           "not a recognized key: " + std::string(key));
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = songs_.find(std::string(songId));
    if (it == songs_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    it->second.metadata.performanceKey = normalized.value();
    it->second.modifiedMs = NowMs();
    // Note: no snapshot on a pure key change (a transposable preference).
    (void)EventBus::Instance().Publish(events::SongKeyChanged{
        it->second.id, it->second.metadata.performanceKey});
    return Ok();
}

// ---------------------------------------------------------------------------
// Arrangements (docs/specs/25 §Arrangements)
// ---------------------------------------------------------------------------
void SongEngine::EnsureOriginalArrangement(Song& song) {
    if (!song.arrangements.empty()) return;
    Arrangement original;
    original.id = "original";
    original.name = "Original";
    for (const auto& sec : song.sections) original.sectionIds.push_back(sec.id);
    song.arrangements.push_back(std::move(original));
}

Result<void> SongEngine::AddArrangement(std::string_view songId, const Arrangement& arr) {
    if (arr.id.empty())
        return Error::Make(Err::InvalidArgument, "SongEngine", "arrangement id is required");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = songs_.find(std::string(songId));
    if (it == songs_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    // Every referenced section must exist (docs/specs/25 §Arrangements).
    for (const auto& sid : arr.sectionIds) {
        bool found = false;
        for (const auto& sec : it->second.sections)
            if (sec.id == sid) { found = true; break; }
        if (!found)
            return Error::Make(Err::Song_SectionNotFound, "SongEngine",
                               "arrangement references missing section: " + sid);
    }
    for (const auto& existing : it->second.arrangements)
        if (existing.id == arr.id)
            return Error::Make(Err::AlreadyExists, "SongEngine",
                               "arrangement already exists: " + arr.id);
    it->second.arrangements.push_back(arr);
    it->second.modifiedMs = NowMs();
    (void)EventBus::Instance().Publish(events::SongArrangementChanged{
        it->second.id, arr.id});
    return Ok();
}

Result<void> SongEngine::RemoveArrangement(std::string_view songId, std::string_view arrId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = songs_.find(std::string(songId));
    if (it == songs_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    auto& arrs = it->second.arrangements;
    for (auto a = arrs.begin(); a != arrs.end(); ++a)
        if (a->id == arrId) {
            arrs.erase(a);
            it->second.modifiedMs = NowMs();
            return Ok();
        }
    return Error::Make(Err::Song_ArrangementNotFound, "SongEngine",
                       "arrangement not found: " + std::string(arrId));
}

Result<std::vector<Arrangement>> SongEngine::Arrangements(std::string_view songId) const {
    auto song = GetSong(songId);
    if (!song.ok()) return song.error();
    return song.value().arrangements;
}

// ---------------------------------------------------------------------------
// Sections (first-class; reorder without rewriting lyrics)
// ---------------------------------------------------------------------------
Result<void> SongEngine::ReorderSections(std::string_view songId,
                                         const std::vector<std::string>& orderedSectionIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = songs_.find(std::string(songId));
    if (it == songs_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    if (orderedSectionIds.size() != it->second.sections.size())
        return Error::Make(Err::InvalidArgument, "SongEngine",
                           "reorder must list every section exactly once");
    std::vector<SongSection> reordered;
    reordered.reserve(orderedSectionIds.size());
    for (const auto& sid : orderedSectionIds) {
        auto s = std::find_if(it->second.sections.begin(), it->second.sections.end(),
                              [&](const SongSection& sec) { return sec.id == sid; });
        if (s == it->second.sections.end())
            return Error::Make(Err::Song_SectionNotFound, "SongEngine",
                               "unknown section: " + sid);
        reordered.push_back(*s);
    }
    it->second.sections = std::move(reordered);
    it->second.modifiedMs = NowMs();
    return Ok();
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------
Result<void> SongEngine::CreateCollection(std::string_view name) {
    if (std::string(name).empty())
        return Error::Make(Err::InvalidArgument, "SongEngine", "collection name is empty");
    std::lock_guard<std::mutex> lock(mutex_);
    collections_.emplace(std::string(name), std::vector<std::string>{});
    return Ok();
}

Result<void> SongEngine::AddToCollection(std::string_view collection, std::string_view songId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = collections_.find(std::string(collection));
    if (it == collections_.end())
        return Error::Make(Err::NotFound, "SongEngine",
                           "collection not found: " + std::string(collection));
    if (songs_.count(std::string(songId)) == 0)
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    it->second.push_back(std::string(songId));
    return Ok();
}

Result<std::vector<std::string>> SongEngine::Collection(std::string_view collection) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = collections_.find(std::string(collection));
    if (it == collections_.end())
        return Error::Make(Err::NotFound, "SongEngine",
                           "collection not found: " + std::string(collection));
    return it->second;
}

std::vector<std::string> SongEngine::CollectionNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, ids] : collections_) out.push_back(name);
    return out;
}

// ---------------------------------------------------------------------------
// Duplicate detection (docs/specs/25 §Duplicate Detection)
// ---------------------------------------------------------------------------
std::string SongEngine::NormalizeLyrics(const Song& song) {
    std::string out;
    for (const auto& sec : song.sections)
        for (const auto& line : sec.lines)
            for (char c : line.lyrics)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

double SongEngine::Similarity(const Song& a, const Song& b) {
    std::string la = NormalizeLyrics(a);
    std::string lb = NormalizeLyrics(b);
    if (la.size() > kSimilarityChars) la = la.substr(0, kSimilarityChars);
    if (lb.size() > kSimilarityChars) lb = lb.substr(0, kSimilarityChars);
    double lyricScore = 0.0;
    if (la.empty() && lb.empty()) {
        lyricScore = 1.0;
    } else if (!la.empty() && !lb.empty()) {
        // Longest-common-substring ratio: two versions of the same song share
        // almost all of their text in order; different songs share little.
        const std::string& s = la.size() <= lb.size() ? la : lb;
        const std::string& l = la.size() <= lb.size() ? lb : la;
        size_t best = 0;
        std::vector<size_t> prev(l.size() + 1, 0), cur(l.size() + 1, 0);
        for (size_t i = 1; i <= s.size(); ++i) {
            for (size_t j = 1; j <= l.size(); ++j) {
                if (s[i - 1] == l[j - 1]) {
                    cur[j] = prev[j - 1] + 1;
                    if (cur[j] > best) best = cur[j];
                } else {
                    cur[j] = 0;
                }
            }
            prev.swap(cur);
            std::fill(cur.begin(), cur.end(), 0);
        }
        lyricScore = static_cast<double>(best) / static_cast<double>(s.size());
    }
    const std::string ta = Lower(a.metadata.title);
    const std::string tb = Lower(b.metadata.title);
    double titleScore = ta == tb && !ta.empty() ? 1.0 : 0.0;
    // 70% lyrics + 30% title.
    return 0.7 * lyricScore + 0.3 * titleScore;
}

Result<std::vector<DuplicateGroup>> SongEngine::FindDuplicates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DuplicateGroup> groups;
    for (auto i = songs_.begin(); i != songs_.end(); ++i) {
        for (auto j = std::next(i); j != songs_.end(); ++j) {
            double sim = Similarity(i->second, j->second);
            if (sim >= kDuplicateReportThreshold) {
                auto it = std::find_if(groups.begin(), groups.end(), [&](const DuplicateGroup& g) {
                    return std::find(g.songIds.begin(), g.songIds.end(), i->first) != g.songIds.end();
                });
                if (it == groups.end()) {
                    DuplicateGroup g;
                    g.similarity = sim;
                    g.songIds = {i->first, j->first};
                    groups.push_back(std::move(g));
                } else {
                    it->songIds.push_back(j->first);
                    it->similarity = std::max(it->similarity, sim);
                }
            }
        }
    }
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                  return a.similarity > b.similarity;
              });
    return groups;
}

Result<std::vector<std::pair<std::string, double>>> SongEngine::ScoredDuplicates(
    const Song& song) const {
    std::vector<std::pair<std::string, double>> scored;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, s] : songs_) {
        if (id == song.id) continue;
        scored.emplace_back(id, Similarity(song, s));
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<std::string, double>& a,
                 const std::pair<std::string, double>& b) { return a.second > b.second; });
    return scored;
}

Result<std::vector<std::string>> SongEngine::LikelyDuplicates(std::string_view songId) const {
    auto target = GetSong(songId);
    if (!target.ok()) return target.error();
    auto scored = ScoredDuplicates(target.value());
    if (!scored.ok()) return scored.error();
    std::vector<std::string> out;
    for (const auto& [id, sim] : scored.value()) {
        if (sim < kDuplicateReportThreshold) break;
        out.push_back(id);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Versioning & recovery (docs/specs/25 §Versioning & Recovery)
// ---------------------------------------------------------------------------
void SongEngine::SnapshotLocked(const Song& song) {
    auto& history = versions_[song.id];
    SongVersion v;
    const int64_t now = NowMs();
    v.id = std::format("v{}-{}", now, history.size());
    v.createdMs = now;
    v.snapshot = song;
    history.push_back(std::move(v));
    if (history.size() > versionLimit_)
        history.erase(history.begin(), history.begin() +
                                           static_cast<long>(history.size() - versionLimit_));
}

Result<std::vector<SongVersion>> SongEngine::Versions(std::string_view songId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = versions_.find(std::string(songId));
    if (it == versions_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    return it->second;
}

Result<Song> SongEngine::RestoreVersion(std::string_view songId,
                                        std::string_view versionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = versions_.find(std::string(songId));
    if (it == versions_.end())
        return Error::Make(Err::Song_NotFound, "SongEngine",
                           "song not found: " + std::string(songId));
    for (const auto& v : it->second)
        if (v.id == versionId) return v.snapshot;
    return Error::Make(Err::Song_VersionNotFound, "SongEngine",
                       "version not found: " + std::string(versionId));
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void SongEngine::WireEvents() {
    if (!subscriptions_.empty()) return;
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
}

void SongEngine::UnwireEvents() {
    for (const auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

void SongEngine::OnConfigReload(const events::ConfigHotReload&) {
    (void)Reload();
}

} // namespace bps::song
