#include "services/SlideBuilder.h"

#include "services/DesignLibraryService.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>

namespace pf = bps::presentation;

namespace SlideBuilder {

std::vector<pf::ContentBlock> templateBlocks(const QString &id)
{
    // LAYOUT ONLY — deliberately no background. The design's background used to
    // be copied into every slide this produced, which made the template outrank
    // the output style (a transparent style still came out opaque). The
    // background is decided by the engine's style/slide precedence instead.
    std::vector<pf::ContentBlock> out;
    const QVariantMap design = TemplateLibraryService::instance().design(id);
    for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
        out.push_back(ShowConverter::blockFromVariant(b.toMap()));
    return out;
}

QVariantList blocksToVariants(const std::vector<pf::ContentBlock> &blocks)
{
    QVariantList out;
    for (const pf::ContentBlock &b : blocks)
        out.append(ShowConverter::blockToVariant(b));
    return out;
}

// The fill's whole placeholder vocabulary, under the EXACT meta keys the engine
// reads it back by (SceneBuilder::SlideValues). A slide built here therefore
// fills every token the preview pane fills — {table_code_paragraphs} included,
// which is code + THIS slide's paragraph numbers and needs both to be carried.
// Empties are INCLUDED on purpose: "not the last slide" is an answer, and a
// missing key would be read as "the fill never said".
QVariantMap valuesToMeta(const pf::BookValues &v)
{
    QVariantMap out{
        // WHICH FAMILY the slide is, carried with the values so the engine
        // answers family-bound tokens from the same name the fill did instead of
        // re-deriving it (BookFill::ContentFamily). Without this a slide's
        // {scripture_name} would resolve on an output whenever a service had put
        // anything in that field, which is exactly how the tab's brand string
        // reached a "Bible version" slot.
        { QStringLiteral("family"),       QString::fromLatin1(pf::Tag(v.family)) },
        { QStringLiteral("ref"),          QString::fromStdString(v.reference) },
        { QStringLiteral("refFull"),      QString::fromStdString(v.referenceFull) },
        { QStringLiteral("refLast"),      QString::fromStdString(v.referenceLast) },
        { QStringLiteral("tableCode"),    QString::fromStdString(v.code) },
        { QStringLiteral("tableYear"),    QString::fromStdString(v.year) },
        { QStringLiteral("tableTitle"),   QString::fromStdString(v.title) },
        { QStringLiteral("tableNumbers"), QString::fromStdString(v.numbers) },
        { QStringLiteral("book"),         QString::fromStdString(v.book) },
        { QStringLiteral("bookAbbr"),     QString::fromStdString(v.bookAbbr) },
        { QStringLiteral("bible"),        QString::fromStdString(v.name) },
        { QStringLiteral("chapter"),      QString::fromStdString(v.chapter) },
        { QStringLiteral("copyright"),    QString::fromStdString(v.copyright) },
    };
    // THE PARALLEL COLUMNS (BookValues::bibles[1..]): a design's own parallel slots
    // ({scripture2_text}…) and an OUTPUT STYLE's template both resolve from these on
    // air, exactly as the preview pane resolves them from the fill. The selected
    // Bible itself is already carried (the body, plus `bible`/`bookAbbr`), so only
    // the EXTRA versions ride here — see SceneBuilder::SlideValues for the read-back.
    for (std::size_t i = 1; i < v.bibles.size() && i < 4; ++i) {
        const QString n = QString::number(static_cast<int>(i) + 1);   // {scripture2_*} is column 2
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Name"), QString::fromStdString(v.bibles[i].name));
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Abbr"), QString::fromStdString(v.bibles[i].bookAbbr));
        out.insert(QStringLiteral("bible") + n + QStringLiteral("Text"), QString::fromStdString(v.bibles[i].text));
    }
    return out;
}

QVariantList partsToVariants(const std::vector<pf::BookPart> &parts)
{
    QVariantList out;
    out.reserve(static_cast<qsizetype>(parts.size()));
    for (const pf::BookPart &part : parts) {
        out.append(QVariantMap{
            { QStringLiteral("number"), part.number },
            { QStringLiteral("partIndex"), part.partIndex },
            { QStringLiteral("text"), QString::fromStdString(part.text) },
            { QStringLiteral("continuation"), part.continuation },
        });
    }
    return out;
}

QString templateId(const Profile &profile)
{
    const QString chosen = SettingsService::instance().value(QString::fromLatin1(profile.templateSetting)).toString();
    if (!chosen.isEmpty() && !TemplateLibraryService::instance().design(chosen).isEmpty())
        return chosen;
    return QString::fromLatin1(profile.defaultTemplate);
}

QVariantList templates(const Profile &profile)
{
    QVariantList out;
    for (const QVariant &d : TemplateLibraryService::instance().designs()) {
        const QVariantMap m = d.toMap();
        if (m.value(QStringLiteral("contentType")).toString() == QLatin1String(profile.contentType))
            out.append(QVariantMap{ { QStringLiteral("id"), m.value(QStringLiteral("id")) }, { QStringLiteral("name"), m.value(QStringLiteral("name")) },
                                    { QStringLiteral("color"), m.value(QStringLiteral("color")) } });
    }
    return out;
}

QString templateName(const QString &id)
{
    return TemplateLibraryService::instance().design(id).value(QStringLiteral("name")).toString();
}

bool templateHasValues(const QString &id)
{
    return pf::HasPlaceholders(templateBlocks(id));
}

QVariantMap previewOf(const std::vector<pf::BookSlide> &slides, bool hasValues)
{
    QVariantMap out{ { QStringLiteral("blocks"), QVariantList() }, { QStringLiteral("background"), QString() },
                     { QStringLiteral("reference"), QString() }, { QStringLiteral("hasValues"), hasValues },
                     { QStringLiteral("slideCount"), 0 }, { QStringLiteral("slides"), QVariantList() } };
    if (slides.empty())
        return out;
    out[QStringLiteral("blocks")] = blocksToVariants(slides.front().blocks);
    // (background stays the empty "no background" from the shape above — the
    // template is layout only; the style/slide precedence supplies the fill.)
    out[QStringLiteral("reference")] = QString::fromStdString(slides.front().reference);
    QVariantList slideGroups;
    slideGroups.reserve(static_cast<qsizetype>(slides.size()));
    for (const pf::BookSlide &slide : slides) {
        QVariantList verses;
        verses.reserve(static_cast<qsizetype>(slide.numbers.size()));
        for (int verse : slide.numbers)
            verses.append(verse);
        slideGroups.append(QVariantMap{
            { QStringLiteral("title"), QString::fromStdString(slide.title) },
            { QStringLiteral("reference"), QString::fromStdString(slide.reference) },
            { QStringLiteral("verses"), verses },
            { QStringLiteral("parts"), partsToVariants(slide.parts) },
            { QStringLiteral("blocks"), blocksToVariants(slide.blocks) },
        });
    }
    out[QStringLiteral("slides")] = slideGroups;
    out[QStringLiteral("slideCount")] = static_cast<int>(slides.size());
    return out;
}

QVariantMap preview(const QString &templateId, const pf::BookSource &source, const pf::BookFillOptions &options)
{
    if (source.items.empty())
        return previewOf({}, true);   // the empty shape, hasValues as it always was
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId);
    return previewOf(pf::FillBookSlides(tmpl, source, options), pf::HasPlaceholders(tmpl));
}

QVariantList slidesOf(const std::vector<pf::BookSlide> &slides, const QString &contentType)
{
    QVariantList out;
    for (const pf::BookSlide &s : slides)
        // `contentType` rides along ("scripture" | "table"): the OUTPUT STYLE's
        // own engine template must not steamroll a DIFFERENT tab's layout —
        // the style's template only applies to its own content family.
        // "" background: the template is layout only (see templateBlocks).
        out.append(QVariantMap{ { QStringLiteral("title"), QString::fromStdString(s.title) }, { QStringLiteral("background"), QString() },
                                { QStringLiteral("blocks"), blocksToVariants(s.blocks) },
                                { QStringLiteral("parts"), partsToVariants(s.parts) },
                                // The BODY, beside the blocks that carry it. Once the
                                // template's slots are filled, the block list no longer
                                // says which one is the content (a table design's date
                                // code is just as text-bearing, and may come first), so
                                // the fill hands the body on and ShowConverter puts it on
                                // the slide's own `text`. Without it the engine guessed
                                // from block order and a code slot made every bound
                                // block render the CODE where the sermon text belonged.
                                { QStringLiteral("body"), QString::fromStdString(s.body) },
                                // ...and the rest of the vocabulary, so an output
                                // style's own template resolves the same tokens
                                // the preview does.
                                { QStringLiteral("fill"), valuesToMeta(s.values) },
                                { QStringLiteral("contentType"), contentType } });
    return out;
}

QVariantList slides(const QString &templateId, const pf::BookSource &source, const pf::BookFillOptions &options,
                    const QString &contentType)
{
    if (source.items.empty())
        return {};
    const std::vector<pf::ContentBlock> tmpl = templateBlocks(templateId);
    return slidesOf(pf::FillBookSlides(tmpl, source, options), contentType);
}

// ---------------------------------------------------------------------------
// The split held for the next click (see the header)
// ---------------------------------------------------------------------------
namespace {

QMutex g_warmMutex;

struct WarmChapter {
    bool valid = false;
    // The chapter ON SCREEN is pinned: it is the one a pick must be served from, and
    // evicting it to make room for a neighbour would put the fill back on the interface
    // thread at the exact moment it exists to spare.
    bool current = false;
    // Monotonic, refreshed on every insert/refresh: the eviction order among the
    // UNPINNED chapters (oldest first).
    quint64 seq = 0;
    ChapterKey key;
    bool hasPlaceholders = false;
    QHash<int, std::vector<pf::BookSlide>> items;
    // The DIVIDED items of the chapter, already in the shape the verse list renders
    // (see itemRows): one entry per item number, built while the cut was in hand. The
    // list is split from this at open, so opening a passage costs the interface thread
    // no fill at all — see attachSplit.
    QVariantMap splitRows;
    // The chapter's own rows, as they were read (no `split` written on them: that is
    // written at attachSplit, on the caller's own copy). Held so a step into this chapter
    // needs no engine read — see warmChapter/warmedRows.
    QVariantList rows;
};

// ONE ITEM'S ROWS AS THE VERSE LIST SHOWS THEM: one per slide the item's OWN cut
// produced, each carrying what its row needs — where it starts in the item
// (`partIndex`, the row's "2-1" label) and whether it is a continuation. The text is
// the slide's pieces joined, which is the same block the output slide carries, so a
// row and the slide a click on it serves can never disagree.
//
// EMPTY when the item was not divided: an undivided item's row shows the chapter's own
// text, which the pane already has, so nothing is carried for it (a chapter of short
// verses therefore holds no second copy of itself). The test is more than one PIECE —
// the pane's own test — because a divided item can still land on a single slide (three
// pieces under a budget that groups them): that item reads as undivided in the list,
// yet choosing it must go through the piece it starts at, exactly as it always has.
QVariantList itemRows(const std::vector<pf::BookSlide> &slides)
{
    std::size_t pieces = 0;
    for (const pf::BookSlide &slide : slides)
        pieces += slide.parts.size();
    if (pieces <= 1)
        return {};
    QVariantList out;
    out.reserve(static_cast<qsizetype>(slides.size()));
    for (const pf::BookSlide &slide : slides) {
        if (slide.parts.empty())
            continue;
        QString text;
        for (const pf::BookPart &part : slide.parts) {
            if (!text.isEmpty())
                text += QLatin1Char(' ');
            text += QString::fromStdString(part.text);
        }
        out.append(QVariantMap{
            { QStringLiteral("partIndex"), slide.parts.front().partIndex },
            { QStringLiteral("text"), text },
            { QStringLiteral("continuation"), slide.parts.front().continuation },
        });
    }
    return out;
}

// Guarded by g_warmMutex. Each tab's chapter, plus the passages a step would reach next
// (see warmNeighbour) — a fixed, small set, so this can never grow with the session.
// Three chapters PER CONTENT FAMILY is what stepping needs (the one on screen and one on
// each side of it), and the two tabs both have a passage on screen at once (see
// evictBeyondCap for what that does to the cap).
QList<WarmChapter> g_warmChapters;
quint64 g_warmSeq = 0;
// Read by the worker of a load that is still reading (see WarmRequest::epoch), so it is
// atomic: it is written by dropWarm on the interface thread while a worker compares it.
std::atomic<quint64> g_warmEpoch{0};

// Beyond any real book or sermon: a chapter this large is not held at all rather
// than turned into a permanent second copy of the library.
constexpr int kMaxWarmItems = 600;
// The pinned chapter plus one neighbour on each side, PER CONTENT FAMILY. A reader that
// walks further forward than this re-reads, which is the correct answer rather than an
// unbounded cache: three passages is a service's worth of stepping ahead.
constexpr int kMaxWarmChapters = 3;

bool sameChapter(const ChapterKey &a, const ChapterKey &b)
{
    return a.contentType == b.contentType && a.templateId == b.templateId
           && a.sourceId == b.sourceId && a.bookId == b.bookId && a.chapter == b.chapter
           && a.parallelIds == b.parallelIds
           && a.options.numbers == b.options.numbers
           && a.options.splitLongItems == b.options.splitLongItems
           && a.options.splitSuffix == b.options.splitSuffix
           && a.options.wordsPerSlide == b.options.wordsPerSlide
           && a.options.maxPerSlide == b.options.maxPerSlide;
}

// The held chapter for a key, or nullptr. Guarded by g_warmMutex.
WarmChapter *heldChapter(const ChapterKey &key)
{
    for (WarmChapter &held : g_warmChapters)
        if (held.valid && sameChapter(held.key, key))
            return &held;
    return nullptr;
}

// Pins a chapter as the one on screen, PER CONTENT FAMILY. Scripture and The Table each
// have a chapter on screen at the same time, so each has its own pin: one tab's walk must
// not evict the passage the other is presenting. Guarded by g_warmMutex.
void pinCurrent(const QString &contentType, WarmChapter *current)
{
    for (WarmChapter &held : g_warmChapters)
        if (held.key.contentType == contentType)
            held.current = false;
    if (current != nullptr)
        current->current = true;
}

// Keeps one family's set small — the pinned chapter of THAT family is never a candidate,
// and among the rest the least recently published goes first. Per family because the two
// tabs are two walks: a shared cap let the chapter one tab was presenting be evicted by
// the other tab's read-ahead (measured: a step that should have been served from the read
// ahead found nothing and read the chapter again). Guarded by g_warmMutex.
void evictBeyondCap(const QString &contentType)
{
    for (;;) {
        int heldOfFamily = 0;
        int oldest = -1;
        for (int i = 0; i < g_warmChapters.size(); ++i) {
            const WarmChapter &held = g_warmChapters.at(i);
            if (held.key.contentType != contentType)
                continue;
            ++heldOfFamily;
            if (held.current)
                continue;
            if (oldest < 0 || held.seq < g_warmChapters.at(oldest).seq)
                oldest = i;
        }
        if (heldOfFamily <= kMaxWarmChapters || oldest < 0)
            return;   // nothing left to drop: every held chapter of this family is the current one
        g_warmChapters.removeAt(oldest);
    }
}

// HOW MUCH OF A CHAPTER THE OPEN CUTS (see the header's head/remainder block): the
// list's own rows up to four screenfuls, and never more than this many items — a chapter
// whose items do not divide contributes one row each, and a budget counted only in rows
// would cut the whole of such a book before it had covered the screen it was filling.
constexpr int kWarmHeadItems = 48;

// ONE ITEM'S CUT, in the two shapes it is held in: its slides (what a pick is served
// from) and, when it divided, the rows the LIST reads it as (see itemRows). Shared by the
// head cut and the remainder so a half-cut chapter is never built two ways.
void cutItem(const WarmRequest &request, const pf::BookItem &item, WarmChapter *out)
{
    std::vector<pf::BookSlide> slides =
        pf::FillBookSlides(request.blocks, pf::ItemOf(request.chapter, item.number), request.key.options);
    // ...and the same answer written in the shape the LIST needs, while it is in
    // hand: this is what lets the chapter reach the screen already divided.
    if (QVariantList rows = itemRows(slides); !rows.isEmpty())
        out->splitRows.insert(QString::number(item.number), rows);
    out->items.insert(item.number, std::move(slides));
}

// Cuts the chapter OUTSIDE the lock — the rule this repo's lock-scope guard exists for:
// guard the publish, never the work. ONE read of the chapter is narrowed per item
// (BookFill::ItemOf): the engine read is the expensive half, and it is the same read for
// every item of it. Returns nothing to hold when the chapter is not one to keep.
//
// `headOnly` stops at kWarmHeadRows list rows / kWarmHeadItems items, which is what makes
// an open cost the same for a 20-item passage and a 500-item one; the caller's own
// warmChapterMore cuts the rest once the rows are on screen. The budget bounds the CUT,
// never the read: `out->rows` is the whole chapter either way (the list scrolls all of
// it), and it is the cut the list divides its rows by.
bool cutChapter(const WarmRequest &request, WarmChapter *out, bool headOnly)
{
    const std::vector<pf::ContentBlock> &blocks = request.blocks;
    if (request.chapter.items.empty()
        || request.chapter.items.size() > static_cast<size_t>(kMaxWarmItems))
        return false;
    out->key = request.key;
    out->hasPlaceholders = pf::HasPlaceholders(blocks);
    out->rows = request.rows;
    out->items.reserve(static_cast<qsizetype>(request.chapter.items.size()));
    int rowsCut = 0;
    int itemsCut = 0;
    for (const pf::BookItem &item : request.chapter.items) {
        cutItem(request, item, out);
        ++itemsCut;
        // The rows this item's cut divided into (0 when it did not divide: its row is the
        // chapter's own text, which the pane already holds).
        const auto split = out->splitRows.constFind(QString::number(item.number));
        rowsCut += split == out->splitRows.constEnd() ? 1 : split->toList().size();
        if (headOnly && itemsCut >= kWarmHeadItems)
            break;
        if (headOnly && rowsCut >= kWarmHeadRows)
            break;
    }
    out->valid = true;
    return true;
}

// Publishes a cut as the chapter on screen (`current`) or as a passage read ahead. Both
// paths drop a request whose epoch moved — the text it was read from has been replaced.
void hold(const WarmRequest &request, bool asCurrent)
{
    if (request.epoch != g_warmEpoch.load())
        return;
    // ALREADY HELD, same key: the same answer. Nothing is cut again — that is what makes
    // a step into a chapter read ahead a lookup — and the held chapter is simply made the
    // current one (or refreshed as a neighbour, so it is not evicted first). A chapter
    // held at its head counts as held here, deliberately: a load that finds it must not
    // cut the head a second time, and its own warmChapterMore cuts the rest.
    {
        QMutexLocker lock(&g_warmMutex);
        if (WarmChapter *existing = heldChapter(request.key)) {
            if (asCurrent)
                pinCurrent(request.key.contentType, existing);
            existing->seq = ++g_warmSeq;
            return;
        }
    }
    WarmChapter next;
    // THE HEAD ONLY, on the very same path a neighbour takes: the caller hands the rows
    // over as soon as this returns, and asks for the rest with warmChapterMore once they
    // are on screen (see the header).
    if (!cutChapter(request, &next, /*headOnly=*/true))
        return;
    next.current = asCurrent;
    QMutexLocker lock(&g_warmMutex);
    if (request.epoch != g_warmEpoch.load())
        return;                          // dropped while it was being cut
    next.seq = ++g_warmSeq;
    if (asCurrent)
        pinCurrent(request.key.contentType, &next);
    g_warmChapters.append(std::move(next));
    evictBeyondCap(request.key.contentType);
}

} // namespace

quint64 warmEpoch()
{
    return g_warmEpoch.load();
}

void warmChapter(const WarmRequest &request)
{
    hold(request, true);
}

bool warmChapterMore(const WarmRequest &request)
{
    // WHAT THE HELD CHAPTER ALREADY HAS, and whether it is still the one this request
    // describes. Read under the lock, cut without it: the cut is the work, and the entry a
    // worker may meanwhile have replaced is checked again before anything is merged into
    // it.
    QHash<int, bool> have;
    {
        QMutexLocker lock(&g_warmMutex);
        const WarmChapter *held = heldChapter(request.key);
        if (held == nullptr
            || held->items.size() >= static_cast<qsizetype>(request.chapter.items.size()))
            return false;   // moved on, never held, or already cut whole
        for (auto it = held->items.constBegin(); it != held->items.constEnd(); ++it)
            have.insert(it.key(), true);
    }
    if (request.epoch != g_warmEpoch.load())
        return false;       // the text under it was replaced while we looked
    WarmChapter more;
    for (const pf::BookItem &item : request.chapter.items)
        if (!have.contains(item.number))
            cutItem(request, item, &more);
    if (more.items.isEmpty())
        return false;
    // MERGE, if the entry is still there and still short of the whole chapter. A chapter
    // that was replaced while this was cut is dropped, not resurrected: what it would
    // extend is now some other passage's entry.
    QMutexLocker lock(&g_warmMutex);
    WarmChapter *held = heldChapter(request.key);
    if (held == nullptr || request.epoch != g_warmEpoch.load())
        return false;
    for (auto it = more.splitRows.constBegin(); it != more.splitRows.constEnd(); ++it)
        held->splitRows.insert(it.key(), it.value());
    int added = 0;
    for (auto it = more.items.begin(); it != more.items.end(); ++it) {
        if (held->items.contains(it.key()))
            continue;
        held->items.insert(it.key(), it.value());
        ++added;
    }
    return added > 0;
}

void warmNeighbour(const WarmRequest &request)
{
    hold(request, false);
}

QVariantList warmedRows(const ChapterKey &key)
{
    QMutexLocker lock(&g_warmMutex);
    const WarmChapter *held = heldChapter(key);
    return held != nullptr ? held->rows : QVariantList{};
}

void attachSplit(QVariantList &rows, const ChapterKey &key)
{
    QVariantMap split;
    {
        QMutexLocker lock(&g_warmMutex);
        if (const WarmChapter *held = heldChapter(key))
            split = held->splitRows;   // implicitly shared: not one word is copied here
    }
    if (split.isEmpty())
        return;                         // nothing was cut: the rows are the read's own
    for (QVariant &row : rows) {
        QVariantMap m = row.toMap();
        const auto it = split.constFind(QString::number(m.value(QStringLiteral("number")).toInt()));
        if (it == split.constEnd())
            continue;
        m.insert(QStringLiteral("split"), it.value());
        row = m;
    }
}

WarmSlides warmedItem(const ChapterKey &key, int number)
{
    QMutexLocker lock(&g_warmMutex);
    WarmSlides out;
    const WarmChapter *held = heldChapter(key);
    if (held == nullptr)
        return out;
    const auto it = held->items.constFind(number);
    if (it == held->items.constEnd())
        return out;
    out.found = true;
    out.hasPlaceholders = held->hasPlaceholders;
    out.slides = it.value();   // copied out, so the caller holds no lock and no cache entry
    return out;
}

int warmedItemCount(const ChapterKey &key)
{
    QMutexLocker lock(&g_warmMutex);
    const WarmChapter *held = heldChapter(key);
    return held != nullptr ? static_cast<int>(held->items.size()) : 0;
}

int warmedChapterCount(const QString &contentType)
{
    QMutexLocker lock(&g_warmMutex);
    int held = 0;
    for (const WarmChapter &chapter : g_warmChapters)
        if (chapter.key.contentType == contentType)
            ++held;
    return held;
}

void dropWarm()
{
    QMutexLocker lock(&g_warmMutex);
    g_warmChapters.clear();
    g_warmEpoch.fetch_add(1);   // any load still reading is reading replaced text now
}

bool operator==(const ChapterKey &a, const ChapterKey &b)
{
    return sameChapter(a, b);
}

} // namespace SlideBuilder
