#pragma once

// What the Scripture tab and The Table tab share: both fill a chosen scripture-style template with picked passages (verses, or a sermon's
// paragraphs) through the ENGINE's slide builder (bps::presentation::FillBookSlides). Only the source of the words, the tab's own
// options and the kind of template differ, so everything else lives here once.

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "modules/presentation/BookFill.hpp"

#include <vector>

namespace SlideBuilder {

// One tab's identity in the shared template library and in the settings.
struct Profile {
    const char *contentType;       // the template type it lists: "scripture" | "table"
    const char *templateSetting;   // the setting that keeps its choice: "scripture.template" | "table.template"
    const char *defaultTemplate;   // the id of its plain layout
};

// The blocks of a template from the engine's template library. LAYOUT ONLY:
// a template supplies no background. Baking the design's own background into
// every generated slide made the template's colour outrank the OUTPUT STYLE's
// (an explicitly transparent style still rendered opaque), so the background
// is the style's / slide's business alone — see SceneBuilder's precedence.
std::vector<bps::presentation::ContentBlock> templateBlocks(const QString &id);

QVariantList blocksToVariants(const std::vector<bps::presentation::ContentBlock> &blocks);

// The chosen template - or the default when nothing is chosen or the chosen one has been deleted.
QString templateId(const Profile &profile);
QVariantList templates(const Profile &profile);          // [{ id, name, color }] of this tab's type only
QString templateName(const QString &id);
// false: the template names no placeholder of the ONE vocabulary the fill knows —
// {scripture_*}, {table_*} or {meta_*} (BookFill::HasPlaceholders). The wording
// here used to say {scripture_*} alone, which is how a layout written entirely
// in the table_ spelling came to be read as a plain "old style" one.
bool templateHasValues(const QString &id);

// The first slide the template makes of the source, in the Edit screen's block shape:
// { blocks, background, reference, hasValues, slideCount }. Empty blocks when there is nothing to show.
QVariantMap preview(const QString &templateId, const bps::presentation::BookSource &source,
                    const bps::presentation::BookFillOptions &options);

// Every slide the template makes of the source: [{ title, background, blocks,
// body, fill, contentType }]. `body` and `fill` are the SLIDE'S OWN values —
// the body text, and the whole placeholder vocabulary it was filled with —
// because once a template's slots are filled they no longer say which block was
// the content or what each one used to ask for (see BookFill::BookSlide); the
// engine resolves an output style's own slots from them. `contentType`
// ("scripture" | "table") rides on each slide so
// the output style's own engine template only restyles ITS content family —
// the style's template must not steamroll the other tab's layout.
QVariantList slides(const QString &templateId, const bps::presentation::BookSource &source,
                    const bps::presentation::BookFillOptions &options,
                    const QString &contentType = QString());

// ============================================================================
// THE SPLIT, DONE BEFORE THE CLICK
// ============================================================================
//
// Where a long item is cut, and which pieces share a slide, is decided by the engine's
// fill — work an operator's click would otherwise pay for at the exact moment the
// machine is busiest (a service, presenting to a congregation). It is also work whose
// answer does not depend on the click: the chapter's items and the tab's options are
// the same either way. So the chapter's OWN background load cuts every item ahead of
// time and the result is held here; choosing one is then a lookup.
//
// The identity below is what "the same answer" means: which Bible / which sermon, the
// book, the chapter, the template, and every option that decides where an item is
// divided. A change to any of them is a different answer, and that request is served
// by filling exactly as it always was — never by a stale cache.
struct ChapterKey {
    QString contentType;      // "scripture" | "table"
    QString templateId;
    QString sourceId;         // the Bible id (the sermon library's id for The Table)
    QString bookId;
    int chapter = 0;
    // The PARALLEL VERSIONS on show, as the tab has them (empty for The Table): they are
    // part of the words a cut lays out, and they live in the settings rather than in the
    // chapter — so without this a version added to the row would leave a cut that still
    // held the old columns looking valid.
    QString parallelIds;
    bps::presentation::BookFillOptions options;
};

// Two keys name the same split. Used to ask the question a settings change poses: has
// anything the cut depends on moved since the chapter on screen was cut?
bool operator==(const ChapterKey &a, const ChapterKey &b);

// ONE BACKGROUND LOAD'S ANSWER, held (see warmChapter): the chapter's own rows as they
// were read, the cut made from them, and the generation they were read under.
//
// The ROWS are held with the cut because the identity is the same one: a step to the
// next passage needs the words AND the cut, and a chapter read ahead of a step that is
// then served without the read is only sound while BOTH are known to belong to the same
// chapter and the same key. Holding them apart would let a stale pair be served.
struct WarmRequest {
    // The generation this chapter was read under (SlideBuilder::warmEpoch(), captured
    // BEFORE the read). A request whose epoch has moved on is dropped: the text under it
    // was replaced while it was being read, so what it holds is not an answer to anything
    // (see dropWarm).
    quint64 epoch = 0;
    ChapterKey key;
    std::vector<bps::presentation::ContentBlock> blocks;
    bps::presentation::BookSource chapter;
    QVariantList rows;
};

// The generation of the held chapters. Bumped by dropWarm(); a background load captures
// it before it reads anything and hands it back in its WarmRequest.
quint64 warmEpoch();

// ============================================================================
// THE HEAD FIRST, THE REST ONCE IT IS ON SCREEN
// ============================================================================
//
// A cut is priced per item (~0.2 ms for a sermon paragraph), so cutting a whole book
// or sermon BEFORE the operator can see it makes an open cost scale with the passage's
// length: the real library's median sermon is 171 paragraphs and its longest are 500,
// i.e. 35–110 ms of fill for a list that shows the first screenful. So the open cuts
// only a HEAD — the rows the pane can paint, which is a fixed budget, not a fraction —
// and hands the rows over immediately; the rest of the chapter follows in the same
// worker (warmChapterMore) and is handed over again once it is cut. An open therefore
// costs the same whether the passage is 20 items or 500.
//
// Nothing is dropped to do it: the head is held in the very same entry the remainder
// extends, so a pick is served from the warm split wherever it lands, and the row list
// is divided (attachSplit) for the WHOLE chapter once the remainder is in — the
// guarantee the head exists for is "what the operator can see is already divided",
// never "only the top of the list ever divides".

// The head's budget, in LIST ROWS rather than items: the pane shows ~18 of its 38px
// rows at once, and this is four screenfuls, so a wheel-flick or an arrow-key walk
// lands on rows that are already divided. In items it is ~20 sermon paragraphs — the
// part of a cut an operator can actually see before it is paid for.
constexpr int kWarmHeadRows = 72;

// Cuts the HEAD of a chapter and holds it, as THE CHAPTER ON SCREEN. CALL THIS OFF THE
// GUI THREAD — being off it is the whole point (the chapter's own background load calls
// it, after the rows are read). `blocks` is read by the CALLER on the GUI thread: the
// fill must use exactly the template a later request would, and the template library is
// not a worker-thread surface. A chapter larger than any real book or sermon is not
// warmed at all rather than held.
//
// Held already (the same key, e.g. a step INTO a chapter that was read ahead): nothing is
// cut again — the same key is the same answer — and the held chapter simply becomes the
// current one. That is what makes a step cheap: the rows it needs are already here.
void warmChapter(const WarmRequest &request);

// Cuts whatever the held chapter still lacks — the rest of it, after the rows are on
// screen — and extends the entry the head made. Returns true when it actually added
// something, which is the caller's answer to "hand the rows over again": the newly cut
// items are what the deep rows of the list divide by.
//
// A no-op (false) when the chapter is not held — it moved on, or was never one to hold —
// when it is already cut whole, and when the generation under it has been replaced. The
// rows it was read from are the caller's, so it touches no service state; call it off the
// GUI thread, in the worker that read the chapter.
bool warmChapterMore(const WarmRequest &request);

// The same as warmChapter, for a NEIGHBOUR of the chapter on screen — the passage a step
// would reach next. Identical in every respect (the head only) except that it is
// evictable: the chapter on screen is never dropped to make room, so the pages being
// presented are the ones held. Call it off the GUI thread, after the current chapter's own
// load has been handed over (the services do, in the same worker: the operator gets the
// chapter they asked for first).
//
// A neighbour is deliberately left at its head: a step into it needs its words and the
// cut of the item it lands on, both of which the head holds — and the step's own load then
// cuts its remainder, exactly as an open does. Cutting the whole of two neighbours ahead
// of the step was up to three whole passages of fill for one screenful of reading.
void warmNeighbour(const WarmRequest &request);

// The chapter's own rows, when they are held (`key` — the chapter, the template and
// every option that decides the cut). EMPTY means "not held": a chapter with no rows is
// never held at all, so an empty answer always means a read is needed. Called by the
// background load BEFORE it reads, which is how a step skips the read entirely.
// (Held rows are the whole chapter's — always: the rows are the read's own answer, and
// the cut is what the head budget bounds, never the list the operator scrolls.)
QVariantList warmedRows(const ChapterKey &key);

// What warmChapter left for ONE item. `found` false means this chapter/options/template
// is not held (never warmed, dropped, or evicted as the operator moved on), and the
// caller fills as before.
struct WarmSlides {
    bool found = false;
    bool hasPlaceholders = false;          // the template's own answer (HasPlaceholders)
    std::vector<bps::presentation::BookSlide> slides;
};
WarmSlides warmedItem(const ChapterKey &key, int number);

// How many items the HELD cut for this chapter has — the head while the remainder is
// still being cut, the whole chapter after it; 0 when that chapter is not held. For the
// one-shot diagnostic the services log per chapter.
int warmedItemCount(const ChapterKey &key);

// How many chapters of one content family ("scripture" | "table") are held at all: one on
// screen plus its neighbours. The pick-miss diagnostic asks this rather than "is this
// chapter held", which is the very question that just came back no — anything held for
// this tab while a pick missed means the key moved.
int warmedChapterCount(const QString &contentType);

// Forgets every held chapter, and bumps the generation: a warm that outlived its text
// would answer a click with the previous wording. Called when the content changes (a
// Bible reinstalled, sermons imported, a library reloaded) — and because it bumps the
// epoch, a chapter being read by a load that started before the change is dropped rather
// than published afterwards (see WarmRequest).
void dropWarm();

// THE CHAPTER'S OWN ROWS, WITH THE CUT ALREADY WRITTEN ON THEM. `rows` is a chapter
// read (and so is `key`); each row of an item the cut DIVIDED gains a `split` list —
// one entry per slide that item produced, carrying `partIndex`, the slide's text and
// `continuation`, in the order the list shows them. An item the cut did not divide, and
// every row of a chapter that was never cut, simply carries no `split`, and the pane
// renders it as it always has.
//
// THIS IS WHY THE LIST IS DIVIDED BEFORE IT IS CLICKED: the pane builds its rows from
// the chapter it was handed, so opening a book shows every long paragraph's
// continuations at once instead of the operator discovering them by clicking, and the
// interface thread never fills anything to do it. Call it on the thread that did the
// cut (see warmChapter), immediately after it — a lookup in the warm split, no engine
// read, no fill.
void attachSplit(QVariantList &rows, const ChapterKey &key);

// The shapes QML receives, shared so a slide served from the warm split and one that
// was just filled are converted in exactly one place. `previewOf({}, true)` is the
// empty shape a preview answers with when there is nothing picked.
QVariantMap previewOf(const std::vector<bps::presentation::BookSlide> &slides, bool hasValues);
QVariantList slidesOf(const std::vector<bps::presentation::BookSlide> &slides, const QString &contentType);

} // namespace SlideBuilder
