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
    // `bookId` + `chapter` are the reference panes' chapter lock: when the lock
    // is on, only the passage that is OPEN is searched — Revelation 1's verses,
    // never the rest of Revelation and never another book. Both tiers honour it
    // (a typed "John 3:16" resolves inside the scope too, so the search box
    // cannot jump out of the passage the lock names). Empty `bookId` = the whole
    // Bible; `chapter` 0 = every chapter of `bookId`.
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
    // The WHOLE Bible's notes, every reference at once — the notes drawer
    // must not sweep the highlights roster to find notes that carry no
    // highlight. Sorted most-recently-modified first.
    Result<std::vector<UserNote>> Notes(std::string_view bibleId) const;
    Result<void> SetHighlight(std::string_view bibleId, const PassageRef& ref, bool on);
    Result<std::vector<PassageRef>> Highlights(std::string_view bibleId) const;
    // --- What a mark on a verse costs, measured ---------------------------------
    // The split above is only worth having if it is actually cheap, and "cheap"
    // is a wall-clock claim: a highlight click is a Q_INVOKABLE on a QML
    // singleton, so its cost lands on the INTERFACE thread. The write records its
    // own two halves (the JSON built under the lock, the PAL write after it), and
    // the app logs them on every mark — the number a reader needs is the one the
    // running app measured, not one a comment asserts. A test asserts on the same
    // numbers (tests_bible.cpp: bible-mark-timing).
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

    // Installed-bible store (see SetStorePath): whole-store JSON rewrite per
    // import/remove — a bible is ~4 MB of verse text, well within the same
    // write-per-change budget the library stores already pay.
    //
    // SPLIT IN TWO ON PURPOSE (the lock-scope rule this module was breaking):
    // the JSON is BUILT under the lock and WRITTEN by the caller after it drops
    // it (see WriteStoreFile). The corpus is megabytes of verse text and
    // WriteAtomic is a PAL call: holding mutex_ across it parked every thread
    // that needs a chapter behind a disk write — the lock-the-guard-cannot-see
    // shape, because the wrap sat one call deep inside this helper.
    // A document BUILT under mutex_ and written after it is dropped. `seq` is
    // stamped at build time, under that same lock, and is what keeps the two
    // writes on disk in the order they were built (see WriteSlot).
    struct Snapshot {
        std::string json;
        std::uint64_t seq = 0;
        bool empty() const { return json.empty(); }
    };
    // ONE WRITER PER FILE, NEWEST WINS — and no lock across the PAL call.
    // Two mutators can overlap (a highlight on the interface thread, an import
    // on the engine worker). Each builds its snapshot under mutex_ and writes
    // with that lock released, so without this the OLDER document could reach
    // the file last and roll the newest mark off the disk. A writer PARKS its
    // snapshot here (a newer build replaces an un-written older one) and
    // leaves; whichever thread finds the slot idle becomes the drainer and
    // writes until the slot is empty. So the file's last content is always the
    // newest built snapshot, a burst of marks costs one write instead of one
    // each, and no lock is ever held across the disk call.
    // `written` only ever moves forward; `inFlight` is the drainer's claim.
    struct WriteSlot {
        Snapshot pending;
        std::uint64_t written = 0;
        bool inFlight = false;
    };
    // ""/seq 0 = nothing to write (no store path). Serializing cannot fail, so
    // there is no Result here: the caller writes the text or does not.
    Snapshot BuildStoreJsonLocked();
    // The PAL write, with NO ordering of its own: this is what the drainer
    // below calls, with every lock released. Verified + atomic, so a failure
    // leaves the previous file.
    Result<void> WriteStoreFile(const std::string& json);
    // Parks `snap` for the corpus and drains the slot (see WriteSlot). Never
    // called with mutex_ held; logs its own failure.
    void PersistStoreFile(Snapshot snap);
    // Consumes the ALREADY-PARSED store document. The file read, the parse and
    // the set-aside of a damaged store all happen BEFORE the engine lock is
    // taken (see Initialize), so no PAL call runs inside the locked scope.
    // `userRoot` is the ALREADY-PARSED user-data sidecar, or null when there is
    // none — in which case user data is read from the corpus document itself
    // (schema <= 2) and Initialize() migrates it into a sidecar.
    Result<size_t> LoadStoreLocked(const json::Value& root, const json::Value* userRoot);

    // JSON (de)serialization of one bible (metadata + books + chapters).
    json::Value BibleToJsonLocked(const BibleVersion& bible) const;
    BibleVersion BibleFromJsonLocked(const json::Value& node) const;
    // User-data persistence (docs/specs/24 §User Data): notes, highlights and
    // collections live in a SIDECAR file beside the corpus (userDataPath_), so
    // a highlight toggle writes kilobytes instead of reading, parsing and
    // rewriting the whole corpus under the engine lock. Mutex-HELD by contract
    // (they touch RefKey and the user-data maps only); BuildUserDataJsonLocked
    // is the sidecar document, WriteUserDataFile is its PAL write and is called
    // with the lock RELEASED. WriteUserDataLocked appends the user-data arrays
    // into the corpus root OBJECT -- used only while the corpus is still the
    // data's only home (schema 2, see userDataFileWritten_), the JSON lib
    // exposing operator[] on the object map and never on Value.
    void WriteUserDataLocked(json::Value::Object& root);
    void ReadUserDataLocked(const json::Value& root);
    Snapshot BuildUserDataJsonLocked();
    // The sidecar write's own numbers (see UserDataWriteStats): written by the two
    // halves of a mark, read by LastUserDataWrite() and by the log line the app
    // prints for each one.
    std::atomic<long long> lastUserDataBuildMs_{-1};
    std::atomic<long long> lastUserDataWriteMs_{-1};
    std::atomic<size_t> lastUserDataBytes_{0};
    std::atomic<size_t> userDataWrites_{0};
    // PAL: NEVER call with mutex_ held (same rule as WriteStoreFile).
    Result<void> WriteUserDataFile(const std::string& json);
    // Parks the sidecar snapshot a caller built under the lock and drains it
    // (see WriteSlot), recording — only once a write REALLY landed — that the
    // sidecar is now the user data's home. Called with the lock RELEASED;
    // failures are logged, never fatal: the corpus keeps carrying the data
    // until this succeeds.
    void PersistUserDataSidecar(Snapshot snap);
    // Parks `snap` in `slot` and drains it; `sidecar` selects the file and the
    // log line. No lock is held across the PAL write.
    void PumpSlot(WriteSlot& slot, Snapshot snap, bool sidecar);
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
    // Records that the sidecar is now the user data's home: from here on the
    // corpus document stops carrying the user-data arrays (schema 3). Flipped
    // only AFTER a sidecar write succeeded, so a failed write can never be the
    // reason a note disappears.
    std::atomic<bool> userDataFileWritten_{ false };
    // Set while installing a corpus that CARRIED the user data (schema <= 2):
    // Initialize() then writes the sidecar from memory, once, off the lock.
    std::atomic<bool> userDataFromCorpus_{ false };

    // Books for resolution: the target Bible's table, or the first installed
    // Bible's table when bibleId is empty.
    std::vector<BibleBook> BooksFor(std::string_view bibleId) const;

    // The two write slots and their order stamps (see WriteSlot). snapshotSeq_
    // is stamped under mutex_; writeMutex_ guards the slots ONLY — it is never
    // held across a PAL call, and no PAL call takes mutex_ either, so the two
    // can never deadlock.
    WriteSlot storeWrite_;
    WriteSlot userWrite_;
    std::uint64_t snapshotSeq_ = 0;   // guarded by mutex_
    mutable std::mutex writeMutex_;   // guards the two slots only

    mutable std::mutex mutex_;
    // Store-restore re-index runs off the boot path — but JOINED in Shutdown(),
    // never detached: a detached indexer outlives Kernel::Shutdown() and dies in
    // torn-down statics (the exit-time 0xc0000005 after picking a search result).
    std::thread reindexThread_;
    std::vector<std::shared_ptr<IBibleProvider>> providers_;
    std::map<std::string, BibleVersion, std::less<>> bibles_;
    // Where the store lives: imported bibles (the CORPUS file). "" = persistence
    // off (unit tests). Set by the Kernel boot before Initialize()'s restore
    // reads it.
    std::string storePath_;
    // The user-data sidecar (<corpus>.userdata.json, e.g. bibles.userdata.json)
    // — notes, highlights, collections, written on every mark. Derived from
    // storePath_ ("" = off).
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
