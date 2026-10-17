#include "modules/bible/BibleEngine.hpp"

#include "core/logging/Logger.hpp"
#include "core/recovery/DocumentRecovery.hpp"
#include "modules/search/SearchEngine.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <thread>

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
    std::vector<BibleVersion> reindex;
    // Store read + parse + recovery with NO lock held: the read is a PAL call and
    // setting a damaged store aside is disk work, and neither may run under
    // mutex_. The locked section below only consumes the parsed document.
    json::Value store;
    bool haveStore = false;
    if (!storePath_.empty()) {
        auto& fsys = platform::PlatformAccessor::Get().Filesystem();
        if (fsys.Exists(storePath_)) {
            auto body = fsys.ReadText(storePath_);
            if (body.ok()) {
                if (auto parsed = json::Parse(body.value()); parsed.ok()) {
                    store = std::move(parsed.value());
                    haveStore = true;
                } else {
                    // SELF-HEALING: a damaged corpus is set aside as evidence
                    // and the engine comes up empty instead of failing the
                    // boot, and the next save rebuilds it at the same path.
                    // (User data lives in its own file since schema 3, so a
                    // damaged corpus cannot take the notes with it.)
                    (void)recovery::SetAsideDamagedDocument(
                        storePath_, "BibleEngine", parsed.error().message);
                }
            }
        }
    }
    // The USER-DATA sidecar (schema 3) is read and parsed out here for the same
    // reason the corpus is: the read is a PAL call and the parse is work over
    // the operator's notes, and neither may run under mutex_ (LoadStoreLocked
    // only INSTALLS what this hands it). A missing sidecar means a schema <= 2
    // store — user data still inside the corpus — or a first boot.
    json::Value userStore;
    bool haveUserStore = false;
    if (!userDataPath_.empty()) {
        auto& fsys = platform::PlatformAccessor::Get().Filesystem();
        if (fsys.Exists(userDataPath_)) {
            auto body = fsys.ReadText(userDataPath_);
            if (body.ok()) {
                if (auto parsed = json::Parse(body.value()); parsed.ok()) {
                    userStore = std::move(parsed.value());
                    haveUserStore = true;
                } else {
                    (void)recovery::SetAsideDamagedDocument(
                        userDataPath_, "BibleEngine", parsed.error().message);
                }
            }
        }
    }
    {
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
        // Restore the installed-bible store (imported bibles survive restarts —
        // the user must never re-upload a full translation). A missing store is
        // the normal first boot, and a damaged one was already set aside as
        // evidence above; both leave the library empty, exactly like a first run.
        size_t restored = 0;
        if (haveStore) {
            if (auto loaded = LoadStoreLocked(store, haveUserStore ? &userStore : nullptr); loaded.ok())
                restored = loaded.value();
            else
                Logger::Instance().Warning("BibleEngine: bible store restore failed: " +
                                               loaded.error().message, "BibleEngine");
        }
        if (restored > 0) {
            // The Search Engine's index is per-process: re-index what the store
            // brought back, off the boot path (the The Table library's
            // precedent). Only the snapshot is taken here — the thread itself
            // is started BELOW, with the lock released.
            for (const auto& [id, b] : bibles_) reindex.push_back(b);
            Logger::Instance().Info(std::format("BibleEngine: restored {} bible(s) from store",
                                                bibles_.size()), "BibleEngine");
        }
    }
    // THE SCHEMA-3 MIGRATION: this boot read the user data out of the CORPUS
    // (schema <= 2). Write the sidecar from memory now — built under the lock,
    // written after it — so the corpus can stop carrying what the mark path
    // used to rewrite. Best effort on purpose: a failed write leaves
    // userDataFileWritten_ false, the corpus keeps carrying the data, and the
    // next boot (or the next mark) tries again.
    if (userDataFromCorpus_.load() && !haveUserStore && !userDataPath_.empty()) {
        Snapshot sidecar;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sidecar = BuildUserDataJsonLocked();
        }
        // Records the new home and logs it once the write really landed; a
        // failure leaves userDataFileWritten_ false, so the corpus keeps
        // carrying the data.
        PersistUserDataSidecar(std::move(sidecar));
    }
    if (!reindex.empty()) {
        // Join/start with mutex_ RELEASED: IndexBible takes that lock itself,
        // so joining a still-running re-index under the lock would deadlock
        // (Shutdown joins the same thread the same way — outside its locked
        // block). The body is a lambda here (not a free function) because
        // IndexBible is private — a lambda inside a member function keeps this
        // function's access — and it is DEFINED below the locked scope so the
        // lock-scope guard can see no lock is held above the call.
        if (reindexThread_.joinable()) reindexThread_.join();   // a prior restore's re-index
        reindexThread_ = std::thread([bibles = std::move(reindex)] {
            for (const BibleVersion& b : bibles) {
                auto indexed = BibleEngine::Instance().IndexBible(b);
                if (indexed.ok())
                    (void)EventBus::Instance().Publish(
                        events::BibleIndexed{b.metadata.id, indexed.value()});
                else
                    Logger::Instance().Warning("BibleEngine: re-index failed for " +
                                                   b.metadata.id + ": " + indexed.error().message,
                                               "BibleEngine");
            }
            Logger::Instance().Info("BibleEngine: restored bibles re-indexed for search",
                                    "BibleEngine");
        });
    }
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
    // The restore re-index must FINISH before systems go down under it (joined,
    // never orphaned — see the member comment).
    if (reindexThread_.joinable()) reindexThread_.join();
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
    Snapshot storeJson;
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notes_.clear();
        highlights_.clear();
        collections_.clear();
        for (auto& [id, bible] : bibles_) bible.verses.clear();
        bibles_.clear();
        // An emptied library persists as an empty store (no path = tests, no-op).
        // BUILT here, WRITTEN below with the lock released (see the header).
        if (!storePath_.empty()) storeJson = BuildStoreJsonLocked();
        if (userDataFileWritten_.load()) userJson = BuildUserDataJsonLocked();
    }
    // Through the write slots, so an older snapshot can never undo this clear
    // (see WriteSlot): the newest document is the one that ends up on disk.
    PersistUserDataSidecar(std::move(userJson));
    PersistStoreFile(std::move(storeJson));
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

    // Collect every provider claiming this format — format-name matches first,
    // then extension-only claims — and try each until one parses: several
    // formats share an extension (Zefania <XMLBIBLE> and OSIS both ship as
    // ".xml"), so the first claimant must not have the last word.
    std::vector<std::shared_ptr<IBibleProvider>> candidates;
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
        return Error::Make(Err::Bible_UnsupportedFormat, "BibleEngine",
                           "no provider for format: " + std::string(format));

    Result<BibleVersion> parsed = Error::Make(Err::Bible_UnsupportedFormat, "BibleEngine",
                                              "no provider could parse format: " +
                                                  std::string(format));
    for (const auto& provider : candidates) {
        parsed = provider->Parse(source, fmt);
        if (parsed.ok()) break;
    }
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
    Snapshot storeJson;
    Snapshot userJson;
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
        // A (re)imported translation drops any user data keyed to the OLD text
        // ("bible replaced" ≠ "notes preserved across versions" — the notes
        // drawer must not show notes from a translation the user removed).
        // Prefix-checked erase (not a plain range): "KJV|" must not sweep
        // "KJV2|..." entries when only KJV is being replaced.
        if (hadOld) {
            const std::string prefix = bible.metadata.id + "|";
            for (auto it = notes_.lower_bound(prefix); it != notes_.end() && it->first.starts_with(prefix);)
                it = notes_.erase(it);
            highlights_.erase(bible.metadata.id);
        }
        bibles_[bible.metadata.id] = bible;
        // Persist the whole corpus so the import survives a restart (no-op
        // while no store path is set — unit tests stay in memory). The JSON is
        // BUILT here and WRITTEN below, with mutex_ released: the corpus is
        // megabytes of verse text and WriteAtomic is a PAL call.
        if (!storePath_.empty()) storeJson = BuildStoreJsonLocked();
        // A REPLACED translation's notes and highlights were just dropped in
        // here: when the sidecar is their home, it has to follow — otherwise the
        // notes drawer keeps offering notes for a text that is gone.
        if (hadOld && userDataFileWritten_.load()) userJson = BuildUserDataJsonLocked();
    }
    // The write slots (see WriteSlot in the header) put these on disk in the
    // order they were BUILT, and log their own failures.
    PersistUserDataSidecar(std::move(userJson));
    PersistStoreFile(std::move(storeJson));

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
    Snapshot storeJson;
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bibles_.find(std::string(bibleId));
        if (it == bibles_.end())
            return Error::Make(Err::Bible_NotFound, "BibleEngine",
                               "bible not found: " + std::string(bibleId));
        removed = std::move(it->second);
        bibles_.erase(it);
        // Its user data has nowhere to live (every screen keys user data by
        // bibleId): drop it with the translation, so a purge is a purge.
        // Prefix-checked erase ("KJV" must not sweep "KJV2" — see Import).
        {
            const std::string prefix = std::string(bibleId) + "|";
            for (auto it = notes_.lower_bound(prefix); it != notes_.end() && it->first.starts_with(prefix);)
                it = notes_.erase(it);
            highlights_.erase(std::string(bibleId));
        }
        // Keep the store in sync so a removed translation stays removed after
        // a restart (no-op while no store path is set). Built under the lock,
        // written after it is released.
        if (!storePath_.empty()) storeJson = BuildStoreJsonLocked();
        // The removal dropped this translation's user data: its sidecar follows.
        if (userDataFileWritten_.load()) userJson = BuildUserDataJsonLocked();
    }
    PersistUserDataSidecar(std::move(userJson));
    PersistStoreFile(std::move(storeJson));
    // Drop this translation's documents from the global search index.
    auto& search = search::SearchEngine::Instance();
    for (const auto& v : removed.verses) (void)search.RemoveDocument(DocId(bibleId, v));
    (void)EventBus::Instance().Publish(events::BibleRemoved{std::string(bibleId)});
    return Ok();
}

// ---------------------------------------------------------------------------
// Installed-bible persistence (docs/specs/24 §Import — restart survival)
// ---------------------------------------------------------------------------
void BibleEngine::SetStorePath(std::string_view path) {
    std::string store(path);
    // The user-data SIDECAR lives beside the corpus and is named after it
    // ("bibles.json" -> "bibles.userdata.json"), so one path from the Kernel
    // still configures both and no other module's file can collide with it.
    // Derived here, outside the lock: path splitting is pure string work, and
    // the locked scope stays a two-assignment scope.
    std::string sidecar;
    if (!store.empty()) {
        static const std::string kJson = ".json";
        std::string base = store;
        if (base.ends_with(kJson)) base.erase(base.size() - kJson.size());
        sidecar = base + ".userdata.json";
    }
    bool liveChange = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        liveChange = initialized_.load();
        storePath_ = std::move(store);
        userDataPath_ = std::move(sidecar);
        // A new location is a new layout to establish: until a sidecar is
        // written THERE, the corpus at that path carries the user data (see
        // BuildStoreJsonLocked). Only a path set BEFORE Initialize() can be
        // established this way — see the `liveChange` warning below.
        if (!liveChange) {
            userDataFileWritten_.store(false);
            userDataFromCorpus_.store(false);
        }
    }
    if (liveChange) {
        // A path set on a RUNNING engine does not re-read the corpus or the
        // sidecar (Initialize() is idempotent), so the flags have to keep
        // describing what is actually loaded in memory. Clearing them here was
        // the quiet data-divergence: the next mark would write the notes into
        // the new corpus while the sidecar at the old path stayed stale.
        Logger::Instance().Warning(
            "BibleEngine: SetStorePath called after Initialize — the new location takes "
            "effect on the next boot; the user-data layout is left as loaded",
            "BibleEngine");
    }
}

// One bible -> JSON. Only the canonical shape persists: metadata + books +
// chapters ("GEN.1" keys, the map GetChapter()/Outline() read). `verses` is
// the flattened view and rebuilds on restore (FlattenVersesLocked), halving
// the store size versus keeping both shapes.
json::Value BibleEngine::BibleToJsonLocked(const BibleVersion& bible) const {
    using J = json::Value;
    J::Object meta;
    meta["id"] = J::String(bible.metadata.id);
    meta["name"] = J::String(bible.metadata.name);
    meta["language"] = J::String(bible.metadata.language);
    meta["copyright"] = J::String(bible.metadata.copyright);
    meta["license"] = J::String(bible.metadata.license);
    meta["source"] = J::String(bible.metadata.source);
    meta["version"] = J::String(bible.metadata.version);
    meta["abbreviation"] = J::String(bible.metadata.abbreviation);
    meta["readOnly"] = J::Bool(bible.metadata.readOnly);

    J::Array books;
    for (const BibleBook& b : bible.books) {
        J::Object bo;
        bo["id"] = J::String(b.id);
        bo["name"] = J::String(b.name);
        bo["testament"] = J::String(b.testament);
        bo["order"] = J::Number(b.order);
        if (!b.aliases.empty()) {
            J::Array al;
            for (const std::string& a : b.aliases) al.push_back(J::String(a));
            bo["aliases"] = J(std::move(al));
        }
        books.push_back(J(std::move(bo)));
    }

    J::Array chapters;
    for (const auto& [key, ch] : bible.chapters) {
        J::Object co;
        co["key"] = J::String(key);
        co["bookId"] = J::String(ch.bookId);
        co["number"] = J::Number(ch.number);
        if (!ch.title.empty()) co["title"] = J::String(ch.title);
        J::Array vs;
        for (const BibleVerse& v : ch.verses) {
            J::Object vo;
            vo["bookId"] = J::String(v.bookId);
            vo["chapter"] = J::Number(v.chapter);
            vo["verse"] = J::Number(v.verse);
            vo["text"] = J::String(v.text);
            if (!v.heading.empty()) vo["heading"] = J::String(v.heading);
            if (v.redLetter) vo["redLetter"] = J::Bool(true);
            vs.push_back(J(std::move(vo)));
        }
        co["verses"] = J(std::move(vs));
        chapters.push_back(J(std::move(co)));
    }

    J::Object root;
    root["metadata"] = J(std::move(meta));
    root["books"] = J(std::move(books));
    root["chapters"] = J(std::move(chapters));
    return J(std::move(root));
}

BibleVersion BibleEngine::BibleFromJsonLocked(const json::Value& node) const {
    using J = json::Value;
    BibleVersion bible;
    const J* meta = node.Find("metadata");
    if (meta && meta->asObject()) {
        bible.metadata.id = std::string(meta->Find("id") ? meta->Find("id")->asString() : "");
        bible.metadata.name = std::string(meta->Find("name") ? meta->Find("name")->asString() : "");
        bible.metadata.language = std::string(meta->Find("language") ? meta->Find("language")->asString() : "");
        bible.metadata.copyright = std::string(meta->Find("copyright") ? meta->Find("copyright")->asString() : "");
        bible.metadata.license = std::string(meta->Find("license") ? meta->Find("license")->asString() : "");
        bible.metadata.source = std::string(meta->Find("source") ? meta->Find("source")->asString() : "");
        bible.metadata.version = std::string(meta->Find("version") ? meta->Find("version")->asString() : "");
        bible.metadata.abbreviation = std::string(meta->Find("abbreviation") ? meta->Find("abbreviation")->asString() : "");
        bible.metadata.readOnly = meta->Find("readOnly") ? meta->Find("readOnly")->asBool() : false;
    }
    if (const J* books = node.Find("books"); books && books->asArray()) {
        for (const J& bv : *books->asArray()) {
            BibleBook b;
            b.id = std::string(bv.Find("id") ? bv.Find("id")->asString() : "");
            b.name = std::string(bv.Find("name") ? bv.Find("name")->asString() : "");
            b.testament = std::string(bv.Find("testament") ? bv.Find("testament")->asString() : "");
            b.order = bv.Find("order") ? static_cast<int>(bv.Find("order")->asInt()) : 0;
            if (const J* aliases = bv.Find("aliases"); aliases && aliases->asArray())
                for (const J& a : *aliases->asArray()) b.aliases.emplace_back(a.asString());
            bible.books.push_back(std::move(b));
        }
    }
    if (const J* chapters = node.Find("chapters"); chapters && chapters->asArray()) {
        for (const J& cv : *chapters->asArray()) {
            BibleChapter ch;
            ch.bookId = std::string(cv.Find("bookId") ? cv.Find("bookId")->asString() : "");
            ch.number = cv.Find("number") ? static_cast<int>(cv.Find("number")->asInt()) : 0;
            ch.title = std::string(cv.Find("title") ? cv.Find("title")->asString() : "");
            if (const J* vs = cv.Find("verses"); vs && vs->asArray()) {
                for (const J& vv : *vs->asArray()) {
                    BibleVerse v;
                    v.bookId = std::string(vv.Find("bookId") ? vv.Find("bookId")->asString() : "");
                    v.chapter = vv.Find("chapter") ? static_cast<int>(vv.Find("chapter")->asInt()) : 0;
                    v.verse = vv.Find("verse") ? static_cast<int>(vv.Find("verse")->asInt()) : 0;
                    v.text = std::string(vv.Find("text") ? vv.Find("text")->asString() : "");
                    v.heading = std::string(vv.Find("heading") ? vv.Find("heading")->asString() : "");
                    v.redLetter = vv.Find("redLetter") ? vv.Find("redLetter")->asBool() : false;
                    ch.verses.push_back(std::move(v));
                }
            }
            bible.chapters[std::format("{}.{}", ch.bookId, ch.number)] = std::move(ch);
        }
    }
    FlattenVersesLocked(bible);
    return bible;
}

void BibleEngine::FlattenVersesLocked(BibleVersion& bible) const {
    bible.verses.clear();
    for (const BibleBook& b : bible.books) {
        for (const auto& [key, ch] : bible.chapters) {
            if (ch.bookId != b.id) continue;
            for (const BibleVerse& v : ch.verses) bible.verses.push_back(v);
        }
    }
}

Result<size_t> BibleEngine::LoadStoreLocked(const json::Value& root, const json::Value* userRoot) {
    bibles_.clear();
    size_t count = 0;
    if (const json::Value* arr = root.Find("bibles"); arr && arr->asArray()) {
        for (const json::Value& bv : *arr->asArray()) {
            BibleVersion bible = BibleFromJsonLocked(bv);
            if (bible.metadata.id.empty() || bible.books.empty() || bible.chapters.empty())
                continue;   // skip a damaged entry, keep the rest
            bibles_[bible.metadata.id] = std::move(bible);
            ++count;
        }
    }
    // USER DATA: the SIDECAR is its home (schema 3) when it is there — the
    // corpus's own copy is then ignored, however old it is. Without a sidecar
    // the corpus's arrays are consumed (schema <= 2) and the corpus is REMEMBERED
    // as their home, so the next corpus save keeps carrying them until the
    // migration write lands (a note must never be lost to a failed write).
    if (userRoot) {
        ReadUserDataLocked(*userRoot);
        userDataFileWritten_.store(true);
        userDataFromCorpus_.store(false);
    } else {
        const bool carried = root.Find("notes") != nullptr || root.Find("highlights") != nullptr
                             || root.Find("collections") != nullptr;
        ReadUserDataLocked(root);
        userDataFromCorpus_.store(carried);
    }
    return count;
}

BibleEngine::Snapshot BibleEngine::BuildStoreJsonLocked() {
    Snapshot out;
    if (storePath_.empty()) return out;
    using J = json::Value;
    J::Array arr;
    for (const auto& [id, bible] : bibles_)
        arr.push_back(BibleToJsonLocked(bible));
    J::Object root;
    // schema 3 = user data lives in the sidecar; schema 2 = THIS file still
    // carries it, which is true exactly until the sidecar's first successful
    // write (userDataFileWritten_). A bible is ~4 MB of verse text, so that
    // switch is what lets a mark stop rewriting megabytes.
    const bool corpusCarriesUserData = !userDataFileWritten_.load();
    root["schema"] = J::Number(corpusCarriesUserData ? 2 : 3);
    root["bibles"] = J(std::move(arr));
    if (corpusCarriesUserData)
        WriteUserDataLocked(root);   // still ONE file: never orphan user data
    out.json = J(std::move(root)).ToString();
    // Stamped under the SAME lock that produced the document — that is what
    // orders the write against an older snapshot built by another thread (see
    // WriteSlot in the header).
    out.seq = ++snapshotSeq_;
    return out;
}

Result<void> BibleEngine::WriteStoreFile(const std::string& json) {
    if (storePath_.empty() || json.empty()) return Ok();
    // ATOMIC (WriteAtomic creates the parent dir too). Called with mutex_
    // RELEASED — see the header: this PAL write is what used to park every
    // scripture read behind an import.
    auto& platform = platform::PlatformAccessor::Get();
    const auto t0 = std::chrono::steady_clock::now();
    auto written = platform.Filesystem().WriteAtomic(storePath_, json);
    // Logged on the same terms as the sidecar write below, and for the same
    // reason: this is the MEGABYTES-per-change path, so a mark that ever reads
    // or rewrites the corpus again is visible in the log as a corpus write next
    // to the mark — where it used to be invisible (the two writes were the same
    // line of code). `tools/selftest/run_mark_sidecar_test.sh` counts both after
    // a mark: sidecar writes go up, corpus writes stay at zero.
    if (written.ok())
        Logger::Instance().Info(
            "BibleEngine: store written (corpus) in " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count()) +
                " ms (" + std::to_string(json.size()) + " bytes)",
            "BibleEngine");
    return written;
}

// --- User-data <-> JSON -----------------------------------------------------

// The SIDECAR document (notes, highlights, collections) — the only thing a mark
// on a verse writes, and kilobytes rather than the corpus's megabytes.
// Mutex-HELD by contract: it walks the user-data maps. The write is
// WriteUserDataFile below, called with the lock released.
BibleEngine::Snapshot BibleEngine::BuildUserDataJsonLocked() {
    const auto t0 = std::chrono::steady_clock::now();
    using J = json::Value;
    J::Object root;
    root["schema"] = J::Number(1);   // the sidecar's own schema, not the corpus's
    WriteUserDataLocked(root);
    Snapshot out;
    out.json = J(std::move(root)).ToString();
    out.seq = ++snapshotSeq_;
    lastUserDataBuildMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
    lastUserDataBytes_.store(out.json.size());
    return out;
}

Result<void> BibleEngine::WriteUserDataFile(const std::string& json) {
    if (userDataPath_.empty() || json.empty()) return Ok();
    auto& platform = platform::PlatformAccessor::Get();
    // ATOMIC, like the corpus: the sidecar holds the user's notes and
    // highlights, so an interrupted save must never damage it.
    const auto t0 = std::chrono::steady_clock::now();
    auto written = platform.Filesystem().WriteAtomic(userDataPath_, json);
    lastUserDataWriteMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
    return written;
}

BibleEngine::UserDataWriteStats BibleEngine::LastUserDataWrite() const {
    UserDataWriteStats s;
    s.lastBuildMs = lastUserDataBuildMs_.load();
    s.lastWriteMs = lastUserDataWriteMs_.load();
    s.lastWriteBytes = lastUserDataBytes_.load();
    s.writes = userDataWrites_.load();
    return s;
}

void BibleEngine::PersistUserDataSidecar(Snapshot snap) {
    if (userDataPath_.empty() || snap.empty()) return;
    PumpSlot(userWrite_, std::move(snap), true);
}

void BibleEngine::PersistStoreFile(Snapshot snap) {
    if (storePath_.empty() || snap.empty()) return;
    PumpSlot(storeWrite_, std::move(snap), false);
}

// The single writer: parks the newest snapshot in the slot, claims the drain,
// and writes until nothing newer is left. Every OTHER caller leaves its
// snapshot parked and returns, so the order the files see is the order the
// snapshots were BUILT — never the order two threads happened to reach the
// disk — and the newest state is always the last thing written.
void BibleEngine::PumpSlot(WriteSlot& slot, Snapshot snap, bool sidecar)
{
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (snap.seq > slot.pending.seq)
            slot.pending = std::move(snap);
        if (slot.inFlight)
            return;              // the drainer already running will pick it up
        slot.inFlight = true;
    }
    for (;;) {
        Snapshot take;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (slot.pending.empty() || slot.pending.seq <= slot.written) {
                slot.pending = Snapshot{};
                slot.inFlight = false;
                return;
            }
            take = std::move(slot.pending);
            slot.pending = Snapshot{};
        }
        // The PAL write runs with NO lock held: writeMutex_ guards the slot
        // only (see the header), so nothing here can park the interface thread
        // behind a disk write, and an older snapshot can no longer land last.
        const Result<void> written = sidecar ? WriteUserDataFile(take.json)
                                             : WriteStoreFile(take.json);
        if (!written.ok()) {
            Logger::Instance().Warning(
                sidecar ? ("BibleEngine: user-data store save failed: " + written.error().message)
                        : ("BibleEngine: bible store save failed: " + written.error().message),
                "BibleEngine");
            std::lock_guard<std::mutex> lock(writeMutex_);
            slot.inFlight = false;   // a later mark pumps whatever is parked
            return;
        }
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            slot.written = take.seq;
        }
        if (sidecar) {
            // The layout only turns over once the data is REALLY on disk.
            userDataFileWritten_.store(true);
            // One line per WRITE (a burst of marks coalesces into one, which is
            // the point), with the numbers the guarantee is about. The corpus is
            // not read, not parsed and not rewritten by this path at all, so the
            // two figures below ARE the whole cost of the click — and the click
            // is a QML call on the interface thread.
            const auto stats = LastUserDataWrite();
            Logger::Instance().Info(
                "BibleEngine: user-data sidecar written in " +
                    std::to_string(stats.lastWriteMs) + " ms (" +
                    std::to_string(take.json.size()) + " bytes, JSON built in " +
                    std::to_string(stats.lastBuildMs) + " ms; corpus untouched, write " +
                    std::to_string(stats.writes + 1) + ")",
                "BibleEngine");
            userDataWrites_.fetch_add(1);
        }
    }
}

// --- User-data <-> JSON (schema 2) -----------------------------------------

// One reference as a JSON object. BookName/Raw are display sugar resolved
// from the book table — the store keeps the canonical shape only (same rule
// the bibles obey), so only the id/verse fields persist.
json::Value BibleEngine::RefToJson(const PassageRef& ref) {
    using J = json::Value;
    J::Object o;
    o["bookId"] = J::String(ref.bookId);
    o["chapter"] = J::Number(ref.chapter);
    o["verseStart"] = J::Number(ref.verseStart);
    o["verseEnd"] = J::Number(ref.verseEnd);
    return J(std::move(o));
}

PassageRef BibleEngine::RefFromJson(const json::Value& node) {
    PassageRef ref;
    if (const json::Value* b = node.Find("bookId"); b) ref.bookId = std::string(b->asString());
    if (const json::Value* c = node.Find("chapter"); c) ref.chapter = static_cast<int>(c->asInt());
    if (const json::Value* vs = node.Find("verseStart"); vs) ref.verseStart = static_cast<int>(vs->asInt());
    if (const json::Value* ve = node.Find("verseEnd"); ve) ref.verseEnd = static_cast<int>(ve->asInt());
    return ref;
}

// Append the user-data arrays into the store's root OBJECT (bibles stay
// schema 1-compatible; the user-data keys are simply absent in schema-1
// stores). Takes the map, not the Value — operator[] lives on the object
// map in this JSON library.
void BibleEngine::WriteUserDataLocked(json::Value::Object& root) {
    using J = json::Value;
    J::Array notes;
    for (const auto& [key, list] : notes_) {
        const size_t sep = key.find('|');
        const std::string bibleId = sep == std::string::npos ? key : key.substr(0, sep);
        for (const UserNote& n : list) {
            J::Object no;
            no["bible"] = J::String(bibleId);
            no["id"] = J::String(n.id);
            no["ref"] = RefToJson(n.ref);
            no["text"] = J::String(n.text);
            no["createdMs"] = J::Number(static_cast<double>(n.createdMs));
            no["modifiedMs"] = J::Number(static_cast<double>(n.modifiedMs));
            notes.push_back(J(std::move(no)));
        }
    }
    J::Array highlights;
    for (const auto& [bibleId, refs] : highlights_)
        for (const PassageRef& r : refs) {
            J::Object ho;
            ho["bible"] = J::String(bibleId);
            ho["ref"] = RefToJson(r);
            highlights.push_back(J(std::move(ho)));
        }
    J::Array collections;
    for (const auto& [name, refs] : collections_) {
        J::Object co;
        co["name"] = J::String(name);
        J::Array cr;
        for (const PassageRef& r : refs) cr.push_back(RefToJson(r));
        co["refs"] = J(std::move(cr));
        collections.push_back(J(std::move(co)));
    }
    root["notes"] = J(std::move(notes));
    root["highlights"] = J(std::move(highlights));
    root["collections"] = J(std::move(collections));
}

void BibleEngine::ReadUserDataLocked(const json::Value& root) {
    notes_.clear();
    highlights_.clear();
    collections_.clear();
    if (const json::Value* arr = root.Find("notes"); arr && arr->asArray()) {
        for (const json::Value& nv : *arr->asArray()) {
            const json::Value* bible = nv.Find("bible");
            const json::Value* id = nv.Find("id");
            const json::Value* ref = nv.Find("ref");
            if (!bible || !id || !ref) continue;   // a damaged entry, not a lost store
            UserNote n;
            n.id = std::string(id->asString());
            n.ref = RefFromJson(*ref);
            if (n.id.empty() || !n.ref.Valid()) continue;
            if (const json::Value* t = nv.Find("text"); t) n.text = std::string(t->asString());
            if (const json::Value* c = nv.Find("createdMs"); c) n.createdMs = c->asInt();
            if (const json::Value* m = nv.Find("modifiedMs"); m) n.modifiedMs = m->asInt();
            notes_[std::string(bible->asString()) + "|" + RefKey(n.ref)].push_back(std::move(n));
        }
    }
    if (const json::Value* arr = root.Find("highlights"); arr && arr->asArray()) {
        for (const json::Value& hv : *arr->asArray()) {
            const json::Value* bible = hv.Find("bible");
            const json::Value* ref = hv.Find("ref");
            if (!bible || !ref) continue;
            PassageRef r = RefFromJson(*ref);
            if (!r.Valid()) continue;
            highlights_[std::string(bible->asString())].push_back(std::move(r));
        }
    }
    if (const json::Value* arr = root.Find("collections"); arr && arr->asArray()) {
        for (const json::Value& cv : *arr->asArray()) {
            const json::Value* name = cv.Find("name");
            if (!name) continue;
            auto& list = collections_[std::string(name->asString())];
            if (const json::Value* refs = cv.Find("refs"); refs && refs->asArray())
                for (const json::Value& rv : *refs->asArray()) {
                    PassageRef r = RefFromJson(rv);
                    if (r.Valid()) list.push_back(std::move(r));
                }
        }
    }
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
                                                        std::string_view bibleId,
                                                        std::string_view bookId,
                                                        int chapter) const {
    std::vector<BibleSearchHit> hits;
    // The chapter lock's test, in one place: is this verse inside the passage the
    // search is scoped to? (Empty bookId = the whole Bible, chapter 0 = the whole
    // book, so the unlocked search runs through the same code it always did.)
    const auto inScope = [&](std::string_view hitBible, std::string_view hitBook, int hitChapter) {
        if (!bookId.empty() && hitBook != bookId) return false;
        if (chapter > 0 && hitChapter != chapter) return false;
        if (!bibleId.empty() && hitBible != bibleId) return false;
        return true;
    };
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
                        // Locked, even a typed reference stays inside the open
                        // passage: the search box must not jump the lock.
                        if (!inScope(hit.bibleId, hit.bookId, hit.chapter))
                            continue;
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
    // Push the scope INTO the engine as the document id prefix the Bible
    // documents carry ("bible:<bible>:<book>:<chapter>:<verse>"), so the
    // candidate slice is drawn from inside the one open passage instead of being
    // filtered after the cap — which would answer a scoped query with whatever
    // the global top-N happened to be.
    if (!bookId.empty() && !bibleId.empty()) {
        filter.idPrefix = std::format("bible:{}:{}:", bibleId, bookId);
        if (chapter > 0) filter.idPrefix += std::format("{}:", chapter);
    }
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
            // (The id prefix above already narrowed the slice; this keeps the
            // scope honest when only a book was named, or no bible id was.)
            if (!inScope(hit.bibleId, hit.bookId, hit.chapter))
                continue;
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
// never stored inside Scripture. Persisted alongside the bibles in the store
// (schema 2): one JSON, the same write-per-change budget the bibles pay, so a
// highlight or note survives a restart exactly like an imported translation.
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
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notes_[std::string(bibleId) + "|" + RefKey(ref)].push_back(std::move(note));
        userJson = BuildUserDataJsonLocked();
    }
    // The write leaves the lock AND the corpus: a note is kilobytes, and the
    // click that made it is a QML call on the interface thread.
    PersistUserDataSidecar(std::move(userJson));
    return Ok();
}

Result<void> BibleEngine::RemoveNote(std::string_view bibleId, const PassageRef& ref) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notes_.erase(std::string(bibleId) + "|" + RefKey(ref));
        userJson = BuildUserDataJsonLocked();
    }
    PersistUserDataSidecar(std::move(userJson));
    return Ok();
}

Result<std::vector<UserNote>> BibleEngine::Notes(std::string_view bibleId,
                                                 const PassageRef& ref) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = notes_.find(std::string(bibleId) + "|" + RefKey(ref));
    return it == notes_.end() ? std::vector<UserNote>() : it->second;
}

Result<std::vector<UserNote>> BibleEngine::Notes(std::string_view bibleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserNote> out;
    // Only THIS bible's notes: the key is "<bibleId>|<ref>", so a plain
    // prefix match (the map is sorted) keeps the drawer from blending notes
    // across translations.
    const std::string prefix = std::string(bibleId) + "|";
    for (auto it = notes_.lower_bound(prefix); it != notes_.end() && it->first.starts_with(prefix); ++it)
        out.insert(out.end(), it->second.begin(), it->second.end());
    std::sort(out.begin(), out.end(),
              [](const UserNote& a, const UserNote& b) { return a.modifiedMs > b.modifiedMs; });
    return out;
}

Result<void> BibleEngine::SetHighlight(std::string_view bibleId, const PassageRef& ref,
                                       bool on) {
    if (!ref.Valid())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "invalid reference");
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& list = highlights_[std::string(bibleId)];
        auto it = std::find_if(list.begin(), list.end(), [&](const PassageRef& r) {
            return RefKey(r) == RefKey(ref);
        });
        if (on && it == list.end())
            list.push_back(ref);
        else if (!on && it != list.end())
            list.erase(it);
        userJson = BuildUserDataJsonLocked();
    }
    // One small sidecar write, off the lock: a highlight click is a QML call on
    // the interface thread and used to re-read, re-parse and rewrite the corpus.
    PersistUserDataSidecar(std::move(userJson));
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
    Snapshot userJson;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = collections_.find(std::string(collection));
        if (it == collections_.end())
            return Error::Make(Err::NotFound, "BibleEngine",
                               "collection not found: " + std::string(collection));
        it->second.push_back(ref);
        userJson = BuildUserDataJsonLocked();
    }
    PersistUserDataSidecar(std::move(userJson));
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
