#pragma once

// BibleEngine (docs/specs/24): the Phase 12 facade. Owns the provider
// registry, the installed-Bible registry, the import/validation pipeline,
// reference resolution, query engine, formatting, parallel-Bible comparison,
// user data (notes/highlights/collections), cross references, and Search
// Engine integration. Knows nothing about presentation, rendering, display, or
// UI — it only provides structured Scripture.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/bible/BibleFormatter.hpp"
#include "modules/bible/BibleTypes.hpp"
#include "modules/bible/IBibleProvider.hpp"
#include "modules/bible/ReferenceResolver.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bps::bible {

class BibleEngine final : public IService {
public:
    static BibleEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "BibleEngine"; }

    // --- Provider registry (docs/specs/24 §Plugin Support) ---
    Result<void> RegisterProvider(std::shared_ptr<IBibleProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);

    // --- Installed-bible persistence ----------------------------------------
    // Imported bibles survive restarts: Import()/RemoveBible() rewrite the JSON
    // store at `path`, Initialize() restores it. OFF until a path is set (unit
    // tests import freely without touching the user's real store); the Kernel
    // boot points it at the app's engine data dir ("bibles.json" inside it).
    void SetStorePath(std::string_view path);
    const std::string& StorePath() const noexcept { return storePath_; }
    std::vector<std::string> ProviderNames() const;

    // --- Import pipeline: detect -> validate -> convert -> store -> index ---
    Result<std::string> Import(std::string_view source, std::string_view format,
                               const ImportOptions& options = {});
    Result<void> RemoveBible(std::string_view bibleId);
    std::vector<std::string> BibleIds() const;
    size_t BibleCount() const;

    // --- Lookup ---
    Result<BibleVersion> GetBible(std::string_view bibleId) const;
    Result<BibleBook> GetBook(std::string_view bibleId, std::string_view bookId) const;
    Result<std::vector<BibleVerse>> GetPassage(std::string_view bibleId,
                                               const PassageRef& ref) const;
    Result<BibleVerse> GetVerse(std::string_view bibleId, std::string_view bookId,
                                int chapter, int verse) const;
    Result<size_t> VerseCount(std::string_view bibleId) const;
    // The cheap ways to browse a Bible (GetBible / GetPassage copy the whole text): its details, its books with their
    // chapter and verse counts, and one chapter.
    Result<TranslationMetadata> Metadata(std::string_view bibleId) const;
    Result<std::vector<BookOutline>> Outline(std::string_view bibleId) const;
    Result<BibleChapter> GetChapter(std::string_view bibleId, std::string_view bookId, int chapter) const;

    // --- Reference resolution ---
    Result<PassageRef> ResolveReference(std::string_view text,
                                        std::string_view bibleId = "") const;
    Result<std::vector<PassageRef>> ResolveReferences(std::string_view text,
                                                      std::string_view bibleId = "") const;

    // --- Search (actual verse content, via the Search Engine) ---
    Result<std::vector<BibleSearchHit>> Search(std::string_view query,
                                               std::string_view bibleId = "") const;

    // --- Parallel Bible (align the same reference across versions) ---
    Result<std::vector<ParallelVerse>> Compare(std::string_view refText,
                                               const std::vector<std::string>& bibleIds) const;

    // --- Formatting ---
    Result<std::string> Format(std::string_view bibleId, const PassageRef& ref,
                               const FormatOptions& options = {}) const;

    // --- User data (stored separately from Scripture; persisted in the store) ---
    Result<void> AddNote(std::string_view bibleId, const PassageRef& ref,
                         const std::string& text);
    // A note removed is a note that never happened. Removing one that was
    // never stored is a benign Ok (idempotent, like SetHighlight off).
    Result<void> RemoveNote(std::string_view bibleId, const PassageRef& ref);
    Result<std::vector<UserNote>> Notes(std::string_view bibleId,
                                        const PassageRef& ref) const;
    // The WHOLE Bible's notes, every reference at once — the notes drawer
    // must not sweep the highlights roster to find notes that carry no
    // highlight. Sorted most-recently-modified first.
    Result<std::vector<UserNote>> Notes(std::string_view bibleId) const;
    Result<void> SetHighlight(std::string_view bibleId, const PassageRef& ref, bool on);
    Result<std::vector<PassageRef>> Highlights(std::string_view bibleId) const;
    Result<void> AddCollection(std::string_view name);
    Result<void> AddToCollection(std::string_view collection, const PassageRef& ref);
    Result<std::vector<PassageRef>> Collection(std::string_view collection) const;
    std::vector<std::string> CollectionNames() const;

    // --- Cross references ---
    Result<std::vector<CrossReference>> CrossReferences(std::string_view bibleId,
                                                        const PassageRef& ref) const;

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);

private:
    BibleEngine() = default;

    // Validation runs inside Import after the provider parse (docs/specs/24
    // §Import & Validation). Returns the number of warnings.
    Result<size_t> Validate(const BibleVersion& bible) const;

    // Indexes verse content into the Search Engine (docs/specs/24 §Search).
    // `onProgress`, when given, is called per verse (indexed, total, bookId).
    Result<size_t> IndexBible(const BibleVersion& bible,
                              const std::function<void(size_t, size_t, std::string_view)>* onProgress = nullptr);

    // Installed-bible store (see SetStorePath): whole-store JSON rewrite per
    // import/remove — a bible is ~4 MB of verse text, well within the same
    // write-per-change budget the library stores already pay.
    Result<void> SaveStoreLocked();
    Result<size_t> LoadStoreLocked();

    // JSON (de)serialization of one bible (metadata + books + chapters).
    json::Value BibleToJsonLocked(const BibleVersion& bible) const;
    BibleVersion BibleFromJsonLocked(const json::Value& node) const;
    // User-data persistence (docs/specs/24 §User Data): notes, highlights, and
    // collections ride in the SAME store file as the bibles (schema 2) and pay
    // the same write-per-change budget. Mutex-HELD by contract (they touch
    // RefKey and the user-data maps only). WriteUserDataLocked appends the
    // user-data arrays into the store's root OBJECT (the JSON lib exposes
    // operator[] on the object map, never on Value).
    void WriteUserDataLocked(json::Value::Object& root);
    void ReadUserDataLocked(const json::Value& root);
    // RESTORE of the flattened verse list from the stored chapters: the store
    // keeps only the canonical shape (GetChapter/Outline read `chapters`), so
    // rebuild `verses` in book order — Import()'s own shape — never a per-read
    // re-flatten on every GetPassage call.
    void FlattenVersesLocked(BibleVersion& bible) const;

    // Canonical reference key for user-data stores ("JHN 3:16").
    static std::string RefKey(const PassageRef& ref);
    // One reference <-> JSON object for the user-data arrays (single place).
    static json::Value RefToJson(const PassageRef& ref);
    static PassageRef RefFromJson(const json::Value& node);
    // Re-write the store with the current user data (mutex HELD; no-op without
    // a store path — the unit-test default keeps everything in memory).
    Result<void> SaveUserDataLocked();

    // Books for resolution: the target Bible's table, or the first installed
    // Bible's table when bibleId is empty.
    std::vector<BibleBook> BooksFor(std::string_view bibleId) const;

    mutable std::mutex mutex_;
    // Store-restore re-index runs off the boot path — but JOINED in Shutdown(),
    // never detached: a detached indexer outlives Kernel::Shutdown() and dies in
    // torn-down statics (the exit-time 0xc0000005 after picking a search result).
    std::thread reindexThread_;
    std::vector<std::shared_ptr<IBibleProvider>> providers_;
    std::map<std::string, BibleVersion, std::less<>> bibles_;
    // Where the store lives: imported bibles AND user data (notes, highlights,
    // collections — one JSON, one write-per-change). "" = persistence off
    // (unit tests). Set by the Kernel boot before Initialize()'s restore
    // reads it.
    std::string storePath_;
    // User data — never inside Scripture (docs/specs/24 §User Data).
    std::map<std::string, std::vector<UserNote>, std::less<>> notes_;       // bible|ref
    std::map<std::string, std::vector<PassageRef>, std::less<>> highlights_; // bibleId
    std::map<std::string, std::vector<PassageRef>, std::less<>> collections_;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> importCount_{0};
    // mutable: ResolveReference() is const but counts resolutions.
    mutable std::atomic<uint64_t> resolveCount_{0};
};

} // namespace bps::bible
