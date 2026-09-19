#pragma once

// SongEngine (docs/specs/25): the Phase 13 facade. Owns the provider registry,
// song registry, import/validation pipeline, chord transposition, arrangement
// management, collections, duplicate detection, versioning/recovery, and
// Search Engine integration. Knows nothing about presentation, rendering,
// display, or UI — it only provides structured musical content.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/songs/Chord.hpp"
#include "modules/songs/ISongProvider.hpp"
#include "modules/songs/SongTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::song {

class SongEngine final : public IService {
public:
    static SongEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "SongEngine"; }

    // --- Provider registry (docs/specs/25 §Plugin Architecture) ---
    Result<void> RegisterProvider(std::shared_ptr<ISongProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);
    std::vector<std::string> ProviderNames() const;

    // --- Import pipeline: detect -> parse -> validate -> dedupe -> store -> index ---
    Result<std::string> Import(std::string_view source, std::string_view format,
                               const ImportOptions& options = {});
    Result<void> RemoveSong(std::string_view songId);
    std::vector<std::string> SongIds() const;
    size_t SongCount() const;

    // --- CRUD on the canonical model (stable ids, never filenames) ---
    Result<std::string> CreateSong(const Song& song);
    Result<Song> GetSong(std::string_view songId) const;
    Result<void> UpdateSong(std::string_view songId, const Song& song);
    Result<std::vector<Song>> Search(std::string_view query) const;

    // --- Transposition (docs/specs/25 §Transposition) — never mutates the original ---
    Result<Song> Transposed(std::string_view songId, int semitones) const;
    Result<void> SetPerformanceKey(std::string_view songId, std::string_view key);

    // --- Arrangements (docs/specs/25 §Arrangements) ---
    Result<void> AddArrangement(std::string_view songId, const Arrangement& arr);
    Result<void> RemoveArrangement(std::string_view songId, std::string_view arrId);
    Result<std::vector<Arrangement>> Arrangements(std::string_view songId) const;

    // --- Sections (first-class; reorder without rewriting lyrics) ---
    Result<void> ReorderSections(std::string_view songId,
                                 const std::vector<std::string>& orderedSectionIds);

    // --- Collections (favorites, worship sets, custom categories) ---
    Result<void> CreateCollection(std::string_view name);
    Result<void> AddToCollection(std::string_view collection, std::string_view songId);
    Result<std::vector<std::string>> Collection(std::string_view collection) const;
    std::vector<std::string> CollectionNames() const;

    // --- Duplicate detection (docs/specs/25 §Duplicate Detection) ---
    Result<std::vector<DuplicateGroup>> FindDuplicates() const;
    Result<std::vector<std::string>> LikelyDuplicates(std::string_view songId) const;

    // --- Versioning & recovery (docs/specs/25 §Versioning & Recovery) ---
    Result<std::vector<SongVersion>> Versions(std::string_view songId) const;
    Result<Song> RestoreVersion(std::string_view songId, std::string_view versionId) const;

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);

private:
    SongEngine() = default;

    Result<size_t> Validate(const Song& song) const;
    Result<size_t> IndexSong(const Song& song);

    // Normalized lyric text for duplicate detection (case/punctuation/space
    // folded).
    static std::string NormalizeLyrics(const Song& song);
    static double Similarity(const Song& a, const Song& b);
    // Scores a candidate song against every stored song (sorted, highest first).
    Result<std::vector<std::pair<std::string, double>>> ScoredDuplicates(const Song& song) const;
    // Snapshots the song into its version history (bounded).
    void SnapshotLocked(const Song& song);
    // Adds the "original" arrangement (all sections, in order) when the song
    // has none (docs/specs/25 §Arrangements).
    static void EnsureOriginalArrangement(Song& song);

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ISongProvider>> providers_;
    std::map<std::string, Song, std::less<>> songs_;
    std::map<std::string, std::vector<SongVersion>, std::less<>> versions_;   // songId
    std::map<std::string, std::vector<std::string>, std::less<>> collections_;
    std::vector<Subscription> subscriptions_;
    size_t versionLimit_ = 32;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> importCount_{0};
};

} // namespace bps::song
