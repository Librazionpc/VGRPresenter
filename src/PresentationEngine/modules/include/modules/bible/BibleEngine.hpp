#pragma once

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
#include <cstdint>
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

    Result<std::vector<BibleSearchHit>> Search(std::string_view query,
                                               std::string_view bibleId = "",
                                               std::string_view bookId = "",
                                               int chapter = 0) const;

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
    Result<std::vector<UserNote>> Notes(std::string_view bibleId) const;
    Result<void> SetHighlight(std::string_view bibleId, const PassageRef& ref, bool on);
    Result<std::vector<PassageRef>> Highlights(std::string_view bibleId) const;
    struct UserDataWriteStats {
        long long lastBuildMs = -1;   // building the sidecar JSON (lock held)
        long long lastWriteMs = -1;   // WriteAtomic of the sidecar (lock released)
        size_t lastWriteBytes = 0;    // the sidecar's size
        size_t writes = 0;            // writes since boot
    };
    UserDataWriteStats LastUserDataWrite() const;

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

    struct Snapshot {
        std::string json;
        std::uint64_t seq = 0;
        bool empty() const { return json.empty(); }
    };
    struct WriteSlot {
        Snapshot pending;
        std::uint64_t written = 0;
        bool inFlight = false;
    };
    // ""/seq 0 = nothing to write (no store path). Serializing cannot fail, so
    // there is no Result here: the caller writes the text or does not.
    Snapshot BuildStoreJsonLocked();
    Result<void> WriteStoreFile(const std::string& json);
    // Parks `snap` for the corpus and drains the slot (see WriteSlot). Never
    // called with mutex_ held; logs its own failure.
    void PersistStoreFile(Snapshot snap);
    Result<size_t> LoadStoreLocked(const json::Value& root, const json::Value* userRoot);

    // JSON (de)serialization of one bible (metadata + books + chapters).
    json::Value BibleToJsonLocked(const BibleVersion& bible) const;
    BibleVersion BibleFromJsonLocked(const json::Value& node) const;
    void WriteUserDataLocked(json::Value::Object& root);
    void ReadUserDataLocked(const json::Value& root);
    Snapshot BuildUserDataJsonLocked();
    std::atomic<long long> lastUserDataBuildMs_{-1};
    std::atomic<long long> lastUserDataWriteMs_{-1};
    std::atomic<size_t> lastUserDataBytes_{0};
    std::atomic<size_t> userDataWrites_{0};
    // PAL: NEVER call with mutex_ held (same rule as WriteStoreFile).
    Result<void> WriteUserDataFile(const std::string& json);
    void PersistUserDataSidecar(Snapshot snap);
    // Parks `snap` in `slot` and drains it; `sidecar` selects the file and the
    // log line. No lock is held across the PAL write.
    void PumpSlot(WriteSlot& slot, Snapshot snap, bool sidecar);
    void FlattenVersesLocked(BibleVersion& bible) const;

    // Canonical reference key for user-data stores ("JHN 3:16").
    static std::string RefKey(const PassageRef& ref);
    // One reference <-> JSON object for the user-data arrays (single place).
    static json::Value RefToJson(const PassageRef& ref);
    static PassageRef RefFromJson(const json::Value& node);
    std::atomic<bool> userDataFileWritten_{ false };
    // Set while installing a corpus that CARRIED the user data (schema <= 2):
    // Initialize() then writes the sidecar from memory, once, off the lock.
    std::atomic<bool> userDataFromCorpus_{ false };

    // Books for resolution: the target Bible's table, or the first installed
    // Bible's table when bibleId is empty.
    std::vector<BibleBook> BooksFor(std::string_view bibleId) const;

    WriteSlot storeWrite_;
    WriteSlot userWrite_;
    std::uint64_t snapshotSeq_ = 0;   // guarded by mutex_
    mutable std::mutex writeMutex_;   // guards the two slots only

    mutable std::mutex mutex_;
    std::thread reindexThread_;
    std::vector<std::shared_ptr<IBibleProvider>> providers_;
    std::map<std::string, BibleVersion, std::less<>> bibles_;
    std::string storePath_;
    std::string userDataPath_;
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