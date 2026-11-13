#include "services/CallTrace.h"
#include "services/LiveOutputService.h"
#include "services/StepCost.hpp"

#include "services/FontService.h"
#include "services/ShowConverter.h"
#include "services/SettingsService.h"
#include "services/StyleSpecBuilder.h"
#include "services/DesignLibraryService.h"
#include "services/MediaLibraryService.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LineWalk.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/rendering/FlowScrollMath.hpp"
#include "modules/rendering/TextEngine.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "services/EventBus.h"
#include "models/OutputListModel.h"
#include "models/GroupListModel.h"
#include "models/BusListModel.h"
#include "models/VideoSourceListModel.h"

#include "modules/display/DisplayEngine.hpp"
#include "modules/display/Providers.hpp"
#include "modules/audio/AudioEngine.hpp"
#include "platform/PlatformAccessor.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "modules/production/ProductionEngine.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QThread>
#include <QAudioOutput>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QFont>
#include <QFontMetricsF>
#include <QTextLayout>
#include <QTextOption>
#include <QEasingCurve>
#include <QVariantAnimation>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <utility>

#include "services/SlidePreviewMap.h"

namespace pl = bps::presentation;
namespace pr = bps::rendering;

using services::preview::setSlideReadingFlow;

static int contentTextBlock(const QVariantMap &slide)
{
    const QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    int best = -1;
    int bestLength = 0;
    for (int i = 0; i < blocks.size(); ++i) {
        const QVariantMap block = blocks.at(i).toMap();
        if (block.value(QStringLiteral("kind"), QStringLiteral("text")).toString()
            != QLatin1String("text"))
            continue;
        const int length = block.value(QStringLiteral("text")).toString().size();
        if (length > bestLength) {
            best = i;
            bestLength = length;
        }
    }
    return best;
}

static double metaLineFactor(const QVariantMap &meta)
{
    bool ok = false;
    const double n = meta.value(QStringLiteral("lineHeight")).toDouble(&ok);
    return bps::presentation::template_text_metrics::lineHeightMultiplier(ok ? n : 0.0);
}

static double metaLineGap(const QVariantMap &meta)
{
    bool ok = false;
    const double n = meta.value(QStringLiteral("lineSpacing")).toDouble(&ok);
    return bps::presentation::template_text_metrics::lineSpacing(ok ? n : 0.0);
}

static double blockPadding(const QVariantMap &block)
{
    return block.value(QStringLiteral("style")).toMap()
        .value(QStringLiteral("padding")).toDouble();
}

// HOW MANY WHOLE LINES A TEXT AREA HOLDS: the ONE rule, so the solver that sizes the text, the line counter
// the monitors and the engine's outputs clip by, and the gap fill all agree. (A hair of slack, because the pitch
// is a floating-point sum and a box that holds exactly N lines must not read as N-1.)
static int wholeLinesIn(double height, double step)
{
    return step > 0.0 ? static_cast<int>(std::floor(height / step + 0.02)) : 0;
}

static QFont::Weight metaFontWeight(const QVariantMap &meta)
{
    if (meta.value(QStringLiteral("bold")).toBool())
        return QFont::Bold;
    const QString weight = meta.value(QStringLiteral("fontWeight")).toString();
    if (weight == QLatin1String("Regular"))
        return QFont::Normal;
    if (weight == QLatin1String("SemiBold"))
        return QFont::DemiBold;
    if (weight == QLatin1String("Bold"))
        return QFont::Bold;
    return QFont::Medium;
}

static QFont blockFontAt(const QVariantMap &block, double pixelSize)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    QFont font;
    const QString family = meta.value(QStringLiteral("fontFamilyResolved")).toBool()
        ? meta.value(QStringLiteral("resolvedFontFamily")).toString()
        : FontService::instance().resolve(meta.value(QStringLiteral("fontFamily")).toString());
    if (!family.isEmpty())
        font.setFamily(family);
    font.setPixelSize(qMax(1, static_cast<int>(pixelSize)));
    font.setWeight(metaFontWeight(meta));
    font.setItalic(meta.value(QStringLiteral("italic")).toBool());
    const double spacing = meta.value(QStringLiteral("letterSpacing")).toDouble();
    if (!qFuzzyIsNull(spacing))
        font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    const double wordSpacing = meta.value(QStringLiteral("wordSpacing")).toDouble();
    if (!qFuzzyIsNull(wordSpacing))
        font.setWordSpacing(wordSpacing);
    return font;
}

static QVector<VisualSpan> layoutSpansAt(const QString &text, const QVariantMap &block,
                                        double pixelSize)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    const double width = block.value(QStringLiteral("width"), 160.0).toDouble() - blockPadding(block) * 2.0;
    const bool oneLine = meta.value(QStringLiteral("oneLine")).toBool();
    const QFont font = blockFontAt(block, pixelSize);

    QVector<VisualSpan> spans;
    int paragraphAt = 0;
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString &paragraph : paragraphs) {
        if (paragraph.trimmed().isEmpty()) {
            spans.append({ paragraphAt, paragraphAt });
            paragraphAt += paragraph.size() + 1;
            continue;
        }
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(oneLine || width <= 0.0 ? QTextOption::NoWrap
                                                   : QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width > 0.0 ? width : 1.0e9);
            spans.append({ paragraphAt + line.textStart(),
                           paragraphAt + line.textStart() + line.textLength() });
        }
        layout.endLayout();
        paragraphAt += paragraph.size() + 1;
    }
    return spans;
}

static double fittedPixelSize(const QString &text, const QVariantMap &block)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    bool ok = false;
    const double declared = meta.value(QStringLiteral("fontSize")).toDouble(&ok);
    const double base = (ok && declared > 0.0) ? declared : 16.0;
    const double pad = blockPadding(block);
    const double width = block.value(QStringLiteral("width"), 160.0).toDouble() - pad * 2.0;
    const double height = block.value(QStringLiteral("height"), 40.0).toDouble() - pad * 2.0;
    const QString autoSize = meta.value(QStringLiteral("autoSize")).toString();
    const bool grow = autoSize.compare(QLatin1String("grow"), Qt::CaseInsensitive) == 0
                      || autoSize.compare(QLatin1String("growToFit"), Qt::CaseInsensitive) == 0;
    if (width <= 0.0 || height <= 0.0 || text.trimmed().isEmpty())
        return base;

    const double factor = metaLineFactor(meta);
    const double gap = metaLineGap(meta);
    // A block that does not grow never ends larger than its declared size (`qMin(base, lo)`
    // below), so every probe above it was a whole-text layout at a size that could not be the
    // answer — the first probe alone was 201 px, thousands of lines for a sermon window, and
    // each probe also loads a font face for that pixel size. That was the whole of the slow
    // FIRST pick after a start (1.1 s; later picks reuse the faces and take ~60 ms). Searching
    // only up to the declared size gives the same answer for the same box.
    double lo = 3.0, hi = grow ? 400.0 : qMax(lo + 1.0, base);
    const auto fitsAt = [&](double size) {
        const QFont font = blockFontAt(block, size);
        const QFontMetricsF metrics(font);
        const double step = bps::presentation::template_text_metrics::linePitch(
            metrics.height(), factor, gap);
        const QVector<VisualSpan> spans = layoutSpansAt(text, block, size);
        double widest = 0.0;
        // A line's INK, not its trailing space: the break leaves the space on the line it
        // ends, and counting it rejected sizes whose words fit (a 754px box refused a line
        // 760px wide that was 8px of space), so the solved size came out far too small and
        // the box showed far more than the max words.
        for (const VisualSpan &span : spans)
            widest = qMax(widest, metrics.horizontalAdvance(
                                       text.mid(span.start, span.end - span.start).trimmed()));
        return widest <= width + 0.01 && spans.size() * step <= height + 0.01;
    };
    if (!grow && fitsAt(hi))
        return qMin(base, hi);   // the declared size fits as it is
    for (int i = 0; i < 11; ++i) {          // the preview's own eleven steps
        const double mid = (lo + hi) / 2.0;
        if (fitsAt(mid))
            lo = mid;
        else
            hi = mid;
    }
    return grow ? lo : qMin(base, lo);
}

static QVector<VisualSpan> visualLinesOfTextAtSize(const QString &text, const QVariantMap &block,
                                                  double pixelSize)
{
    QVector<VisualSpan> lines = layoutSpansAt(text, block, pixelSize);

    while (!lines.isEmpty() && text.mid(lines.first().start,
                                       lines.first().end - lines.first().start)
                                        .trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && text.mid(lines.last().start,
                                       lines.last().end - lines.last().start)
                                        .trimmed().isEmpty())
        lines.removeLast();
    return lines;
}

static QVector<VisualSpan> visualLinesOfText(const QString &text, const QVariantMap &block)
{
    return visualLinesOfTextAtSize(text, block, fittedPixelSize(text, block));
}

// The slide's reading block: `score` ranks the text blocks and a score of 0 or less
// disqualifies one, so the first candidate on a tie wins.
static QVariantMap readingBlockScored(const QVariantMap &slide,
                                      double (*score)(const QVariantMap &))
{
    const QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    QVariantMap best;
    double bestScore = 0.0;
    for (const QVariant &entry : blocks) {
        const QVariantMap block = entry.toMap();
        if (block.value(QStringLiteral("kind"), QStringLiteral("text")).toString()
            != QLatin1String("text"))
            continue;
        const double candidate = score(block);
        if (candidate <= bestScore)
            continue;
        bestScore = candidate;
        best = block;
    }
    return best;
}

static double areaOfBlock(const QVariantMap &block)
{
    return block.value(QStringLiteral("width"), 0.0).toDouble()
           * block.value(QStringLiteral("height"), 0.0).toDouble();
}

// DesignPreview: only a NON-EMPTY text is a reading block, so length 0 disqualifies.
static double textLengthOfBlock(const QVariantMap &block)
{
    return static_cast<double>(block.value(QStringLiteral("text")).toString().size());
}

static QVariantMap readingBoxForWrapping(const QVariantMap &slide)
{
    return readingBlockScored(slide, areaOfBlock);
}

static QVariantMap readingBlockOf(const QVariantMap &slide)
{
    return readingBlockScored(slide, textLengthOfBlock);
}

static QString readingTextOf(const QVariantMap &slide)
{
    const QString body = slide.value(QStringLiteral("body")).toString();
    if (!body.trimmed().isEmpty())
        return body;
    const int content = contentTextBlock(slide);
    if (content >= 0)
        return slide.value(QStringLiteral("blocks")).toList().at(content).toMap()
            .value(QStringLiteral("text")).toString();
    return slide.value(QStringLiteral("line2")).toString();
}

static QVector<VisualSpan> newlineSpansOf(const QString &text)
{
    QVector<VisualSpan> spans;
    int start = 0;
    while (true) {
        const int br = text.indexOf(QLatin1Char('\n'), start);
        const int end = br < 0 ? static_cast<int>(text.size()) : br;
        spans.append({ start, end });
        if (br < 0)
            break;
        start = br + 1;
    }
    return spans;
}

//  1. WHICH TEXT. `body` first: that is where a FILL-built slide keeps its
//  2. THE WIDTH IT WRAPS AT. Measured on the COMPOSED picture
//  3. THE BREAKS THE READER SEES. Wrapped, not counted by newline: a verse or a
QStringList LiveOutputService::readingLines(const QVariantMap &slide) const
{
    const QString text = readingTextOf(slide);
    if (text.trimmed().isEmpty())
        return {};

    const bool bodyCarried = !slide.value(QStringLiteral("body")).toString().trimmed().isEmpty();
    const QVariantMap measured = bodyCarried ? previewWithActiveStyle(slide) : slide;
    const QVariantMap box = readingBoxForWrapping(measured);
    const QVector<VisualSpan> spans =
        box.isEmpty() ? newlineSpansOf(text) : visualLinesOfText(text, box);
    QStringList lines;
    lines.reserve(spans.size());
    for (const VisualSpan &span : spans)
        lines.append(text.mid(span.start, span.end - span.start));
    static std::mutex boxLogMutex;
    static std::set<QString> boxLogged;
    const QString boxKey = QStringLiteral("bodyCarried=%1 boxW=%2 boxH=%3")
                               .arg(bodyCarried ? 1 : 0)
                               .arg(box.value(QStringLiteral("width")).toDouble())
                               .arg(box.value(QStringLiteral("height")).toDouble());
    {
        std::lock_guard<std::mutex> lock(boxLogMutex);
        if (boxLogged.insert(boxKey).second)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadingFlowBox"),
                                QStringLiteral("%1 lines=%2 (first page measured on this box)")
                                    .arg(boxKey)
                                    .arg(lines.size()));
    }
    return lines;
}

QStringList LiveOutputService::readingLinesAt(int index) const
{
    if (index < 0 || index >= lineSetRaw_.size())
        return {};
    const auto it = readingLinesCache_.constFind(index);
    if (it != readingLinesCache_.constEnd())
        return it.value();
    const QStringList lines = readingLines(lineSetRaw_.at(index).toMap());
    readingLinesCache_.insert(index, lines);
    return lines;
}

constexpr int kReadingPrewarmRadius = 4;

void LiveOutputService::schedulePrewarmReadingLines()
{
    if (readingPrewarmQueued_)
        return;
    readingPrewarmQueued_ = true;
    schedulePrewarmReadingLines();
}

void LiveOutputService::prewarmReadingLines()
{
    readingPrewarmQueued_ = false;
    if (lineSetRaw_.isEmpty())
        return;
    const int last = static_cast<int>(lineSetRaw_.size()) - 1;
    int next = -1;
    for (int step = 1; step <= kReadingPrewarmRadius && next < 0; ++step) {
        const int ahead = lineSlideIndex_ + step;
        if (ahead <= last && !readingLinesCache_.contains(ahead))
            next = ahead;
    }
    for (int step = 1; step <= kReadingPrewarmRadius && next < 0; ++step) {
        const int behind = lineSlideIndex_ - step;
        if (behind >= 0 && !readingLinesCache_.contains(behind))
            next = behind;
    }
    if (next < 0)
        return;   // the whole radius is warm: nothing to do
    readingLinesAt(next);   // the cache is the point; nothing else reads the return here
    schedulePrewarmReadingLines();   // the next uncached one, on the next turn
}

void LiveOutputService::stageSlidesWithPreview(const QString &name, const QVariantList &slides,
                                                const QVariantMap &styledPreview)
{
    pendingReadingSteps_ = 0;
    ++readingStageSerial_;
    emit readingStageChanged();
    lineSetName_ = name;
    lineSetRaw_ = slides;
    ClearReadingTextSource();
    lineSlideIndex_ = 0;
    readingPickBase_ = 0;      // a new pick: the origin is ITS first page
    lineOffset_ = 0;
    readingLinesCache_.clear();
    ResetReadingPick();
    rebuildReadingRun(0);
    readingFlowText_ = readingRunText_;
    AnnounceReadingScroll();
    QVariantList pick = slides;
    if (!pick.isEmpty()) {
        QVariantMap first = pick.first().toMap();
        setSlideReadingFlow(first, scrollMode_, readingFlowText_, 0.0);
        pick[0] = first;
    }
    if (live_) {
        goLiveWithSlides(name, pick);   // commit the pick to the engine now
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};     // tiles must read the NEW onAirSlide,
        BumpStaged();             // not a stale staged copy over it
        announceReadingPage();           // ...and only now is the page announced
        return;
    }
    stagedName_ = name;
    stagedSlidesRaw_ = pick;
    stagedSlide_ = pick.isEmpty() ? QVariantMap{}
                 : (!styledPreview.isEmpty() ? styledPreview
                                             : previewWithActiveStyle(pick.first().toMap()));
    BumpStaged();
    announceReadingPage();
}

// ---- the line stepper (Alt+Down / Alt+Up) --------------------------------
void LiveOutputService::setScrollMode(bool on)
{
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadScrollMode"),
                        QStringLiteral("on=%1 was=%2 live=%3 pending=%4 textOnly=%5 pick=%6")
                            .arg(on)
                            .arg(scrollMode_)
                            .arg(live_)
                            .arg(pendingTextSourcePages_.size())
                            .arg(lineSourceTextOnly_)
                            .arg(lineSetRaw_.size()));
    if (on == scrollMode_)
        return;
    scrollMode_ = on;
    if (!on) {
        pendingReadingSteps_ = 0;
        bps::live::LiveOutputController::Instance().SetFlowChar(0.0f);
        bps::live::LiveOutputController::Instance().SetFlowSpan(-1.0f, -1.0f);
        readingSpanFrom_ = readingSpanTo_ = -1.0;
    }
    emit scrollModeChanged();
    QTimer::singleShot(0, this, []() {
        if (auto *outputs = OutputListModel::instance())
            outputs->repushEngineStyles();
    });

    if (on && !pendingTextSourcePages_.isEmpty()) {
        activatePendingTextSource();
        republishReadingFlow();   // Slide mode had taken the passage off the slide
        return;
    }
    if (!on && lineSourceTextOnly_ && !readingTextSourcePages_.isEmpty()) {
        const int page = qBound(0, readingPage(),
                                static_cast<int>(readingTextSourcePages_.size()) - 1);
        lineSetRaw_ = readingTextSourcePages_;
        lineSourceTextOnly_ = false;
        readingBudgetFitActive_ = false;
        readingTextSourcePages_.clear();
        readingTextSourcePageEnds_.clear();
        readingTextSourceText_.clear();
        readingTextSourceStartChar_ = -1;
        readingTextSourceInitialPositionPending_ = false;
        lineSlideIndex_ = page;
        readingScroll_ = 0.0;
        lineOffset_ = 0;
        readingLinesCache_.clear();
        resetReadingRun();
        rebuildReadingRun(lineSlideIndex_);
        readingFlowText_ = readingRunText_;
    }
    if (!lineSetRaw_.isEmpty())
        applyReadingRestage(true, /*forceRebuild=*/ false, /*forceScrollNotify=*/ true);
    if (on)
        republishReadingFlow();
}

void LiveOutputService::scheduleReadingRepublish()
{
    if (!republishTimer_) {
        republishTimer_ = new QTimer(this);
        republishTimer_->setSingleShot(true);
        connect(republishTimer_, &QTimer::timeout, this, &LiveOutputService::republishReadingFlow);
    }
    republishTimer_->start(60);   // restarts: a burst of seats/measures costs one hand-off
}

void LiveOutputService::republishReadingFlow()
{
    if (!live_ || !scrollMode_ || readingFlowText_.isEmpty())
        return;
    const QVariantList current = onAirSlidesRaw();
    if (current.isEmpty())
        return;
    QVariantMap shown = current.first().toMap();
    attachReadingFlow(shown, readingScroll_);
    // NOTHING NEW, NOTHING SENT: the engine already holds this passage, these line breaks and this
    // position (the republish used to run 900+ times in a session, each a full re-layout and a scene
    // rebuild for nothing).
    const QVariantMap breaks = shown.value(QStringLiteral("flowBreaks")).toMap();
    QString sig = QString::number(qHash(shown.value(QStringLiteral("flowText")).toString()), 16)
                  + QLatin1Char('|') + QString::number(shown.value(QStringLiteral("flowChar")).toDouble(), 'f', 1);
    for (auto it = breaks.constBegin(); it != breaks.constEnd(); ++it) {
        // (the starts themselves, not just how many: anchoring moves the lines without changing their number)
        const QVariantList list = it.value().toList();
        size_t hash = list.size();
        for (const QVariant &start : list)
            hash = hash * 1000003u + static_cast<size_t>(start.toInt());
        sig += QLatin1Char('|') + it.key() + QLatin1Char(':') + QString::number(list.size())
               + QLatin1Char('/') + QString::number(static_cast<qulonglong>(hash), 16);
    }
    if (sig == lastRepublishSig_)
        return;
    lastRepublishSig_ = sig;
    readingEngineFlowText_ = shown.value(QStringLiteral("flowText")).toString();
    updateReadingPick(onAirTitle_.isEmpty() ? lineSetName_ : onAirTitle_, shown);
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFlowRepublish"),
                        QStringLiteral("staged=%1 stagedFlow=%2 onAir=%3 flow=%4 live=%5 scroll=%6")
                            .arg(stagedSlidesRaw_.size())
                            .arg(stagedSlide_.value(QStringLiteral("flowText")).toString().size())
                            .arg(current.size())
                            .arg(readingFlowText_.size())
                            .arg(live_)
                            .arg(scrollMode_));
}

void LiveOutputService::setReadingWordBudget(int words)
{
    const int clamped = qBound(3, words, 1000);
    if (clamped == readingWordBudget_)
        return;
    readingWordBudget_ = clamped;
    emit readingWordBudgetChanged();
}

void LiveOutputService::noteOperatorJump()
{
    ++readingJumpSerial_;
    emit readingStageChanged();
}

// The slide's reference label: the meta "ref" the style's label block binds to, plus the text of the block bound to "ref"
// the slide itself carries. True when anything changed.
static bool setReferenceLabel(QVariantMap &slide, const QString &reference)
{
    bool changed = false;
    QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    for (int i = 0; i < blocks.size(); ++i) {
        QVariantMap block = blocks.at(i).toMap();
        if (block.value(QStringLiteral("bind")).toString() != QLatin1String("ref")
                || block.value(QStringLiteral("text")).toString() == reference)
            continue;
        block.insert(QStringLiteral("text"), reference);
        blocks[i] = block;
        changed = true;
    }
    if (changed)
        slide.insert(QStringLiteral("blocks"), blocks);
    // THE LABEL'S REAL SOURCE: a style's label block is bound to "ref", which composes to the slide's meta "ref" — and,
    // when the slide has none, to its TITLE (the pick's name, fixed). So the walk's reference has to be that meta value.
    if (slide.value(QStringLiteral("ref")).toString() != reference) {
        slide.insert(QStringLiteral("ref"), reference);
        changed = true;
    }
    // ...and the slide's "fill" (the meta fields a later composition re-reads — see ShowConverter::fillFromMeta). It is
    // inserted into the meta AFTER the slide's own "ref", so a stale "ref" in it silently wins: keep its reference
    // fields in step. (One verse or paragraph: the full and last references are the same text.)
    QVariantMap fill = slide.value(QStringLiteral("fill")).toMap();
    bool fillChanged = false;
    for (const char *key : { "ref", "refFull", "refLast" }) {
        const QString k = QString::fromLatin1(key);
        if (fill.contains(k) && fill.value(k).toString() != reference) {
            fill.insert(k, reference);
            fillChanged = true;
        }
    }
    if (fillChanged) {
        slide.insert(QStringLiteral("fill"), fill);
        changed = true;
    }
    return changed;
}

void LiveOutputService::setWalkReference(const QString &reference)
{
    if (reference.isEmpty() || reference == walkReference_ || !scrollMode_ || !lineSourceTextOnly_)
        return;
    if (live_) {
        const QVariantList current = onAirSlidesRaw();
        if (current.isEmpty())
            return;
        QVariantMap shown = current.first().toMap();
        walkReference_ = reference;
        if (!setReferenceLabel(shown, reference))
            return;
        attachReadingFlow(shown, readingScroll_);
        updateReadingPick(onAirTitle_, shown);   // the engine keeps the pick and moves only the label
        // The surfaces compose from this copy until the engine's snapshot comes back (within a poll tick): move it now,
        // and let them know, so a tile does not keep the old label.
        setReferenceLabel(onAirSlideRaw_, reference);
        // ...and the staged copy of the SAME pick, when there is one: the surfaces draw the staged slide in preference to
        // the on-air one, so a stale label there would be what they show.
        if (!stagedSlidesRaw_.isEmpty()) {
            QVariantMap staged = stagedSlidesRaw_.first().toMap();
            if (setReferenceLabel(staged, reference)) {
                stagedSlidesRaw_ = QVariantList{staged};
                stagedSlide_ = readingStagedPreview(staged);
            }
        }
        BumpStaged();
    } else if (!stagedSlidesRaw_.isEmpty()) {
        QVariantMap shown = stagedSlidesRaw_.first().toMap();
        walkReference_ = reference;
        if (!setReferenceLabel(shown, reference))
            return;
        stagedSlidesRaw_ = QVariantList{shown};
        stagedSlide_ = readingStagedPreview(shown);
        BumpStaged();
    }
}

void LiveOutputService::stageSlideSet(const QString &name, const QVariantList &slides, int index)
{
    VGR_CALL_TRACE("LiveOutputService.stageSlideSet");
    pendingReadingSteps_ = 0;
    if (slides.isEmpty()) {
        lineSetRaw_.clear();
        ClearReadingTextSource();
        lineSlideIndex_ = 0;
        readingPickBase_ = 0;
        announceReadingPage();
        lineOffset_ = 0;
        readingLinesCache_.clear();
        resetReadingRun();
        stageSlides(name, slides);
        return;
    }
    ++readingStageSerial_;
    walkReference_.clear();   // a new pick: its own label again
    if (!quietStage_)
        emit readingStageChanged();
    const int at = qBound(0, index, static_cast<int>(slides.size()) - 1);
    lineSetName_ = name;
    lineSetRaw_ = slides;
    ClearReadingTextSource();
    lineSlideIndex_ = at;
    readingPickBase_ = 0;         // a new pick: the origin is the page it starts on
    lineOffset_ = 0;
    readingLinesCache_.clear();   // a new pick: see stageSlidesWithPreview's own comment
    ResetReadingPick();
    rebuildReadingRun(at);
    readingFlowText_ = readingRunText_;
    applyReadingRestage(true, /*forceRebuild=*/ false, /*forceScrollNotify=*/ true);
}

void LiveOutputService::setReadingTextSource(const QString &name, const QVariantList &pages, int index)
{
    if (pages.isEmpty())
        return;
    pendingTextSourceName_ = name;
    pendingTextSourcePages_ = pages;
    pendingTextSourceIndex_ = qBound(0, index, static_cast<int>(pages.size()) - 1);
    if (scrollMode_)
        activatePendingTextSource();
}

void LiveOutputService::stagePassagePick(const QString &name, const QVariantList &slides, int index,
                                         const QVariantList &pages, int pageIndex)
{
    // The passage follows the slide in the next call, which tells everyone once; until then, stay quiet.
    const bool passageFollows = scrollMode_ && !pages.isEmpty() && !slides.isEmpty();
    if (!live_ || !passageFollows) {
        quietStage_ = passageFollows;
        stageSlideSet(name, slides, index);
        quietStage_ = false;
        setReadingTextSource(name, pages, pageIndex);
        return;
    }
    holdingPassagePick_ = true;
    heldPickSent_ = false;
    heldPickSlide_.clear();
    QElapsedTimer pickClock;
    pickClock.start();
    quietStage_ = true;
    stageSlideSet(name, slides, index);        // state only: the commit is held back
    quietStage_ = false;
    const qint64 stageMs = pickClock.elapsed();
    setReadingTextSource(name, pages, pageIndex);   // publishes the held slide + passage once
    if (pickClock.elapsed() >= 40)
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                            QStringLiteral("stagePassagePick stageSlideSet=%1ms setReadingTextSource=%2ms")
                                .arg(stageMs).arg(pickClock.elapsed() - stageMs));
    if (!heldPickSent_ && !heldPickSlide_.isEmpty())
        (void)updateReadingPick(name, heldPickSlide_);   // the passage never activated: send the slide
    holdingPassagePick_ = false;
    heldPickSent_ = false;
    heldPickSlide_.clear();
}

void LiveOutputService::activatePendingTextSource()
{
    if (pendingTextSourcePages_.isEmpty())
        return;
    QElapsedTimer costClock;
    costClock.start();
    qint64 costBuilt = 0, costWindow = 0, costRun = 0, costTable = 0;
    pendingReadingSteps_ = 0;
    QVariantMap visualPage;
    const QVariantList visualSlides = holdingPassagePick_ ? QVariantList{heldPickSlide_}
                                      : live_ ? onAirSlidesRaw() : stagedSlidesRaw_;
    if (!visualSlides.isEmpty())
        visualPage = visualSlides.first().toMap();
    QVariantList readingPages;
    readingPages.reserve(pendingTextSourcePages_.size());
    for (const QVariant &value : pendingTextSourcePages_) {
        QVariantMap page = value.toMap();
        if (page.value(QStringLiteral("contentType")).toString().isEmpty())
            page[QStringLiteral("contentType")] = visualPage.value(QStringLiteral("contentType"));
        readingPages.append(page);
    }
    const int selectedPage = qBound(0, pendingTextSourceIndex_,
                                    static_cast<int>(readingPages.size()) - 1);
    int selectedChar = 0;
    QString passage;
    readingTextSourcePageEnds_.clear();
    readingTextSourcePageEnds_.reserve(readingPages.size());
    for (int i = 0; i < readingPages.size(); ++i) {
        if (i == selectedPage)
            selectedChar = passage.size();
        passage += readingTextOf(readingPages.at(i).toMap());
        readingTextSourcePageEnds_.append(passage.size());
    }
    readingTextSourceText_ = passage;
    QVariantMap passagePage = readingPages.first().toMap();
    passagePage[QStringLiteral("body")] = passage;

    lineSetName_ = pendingTextSourceName_;
    lineSetRaw_ = QVariantList{passagePage};
    readingTextSourcePages_ = readingPages;
    readingTextSourceStartChar_ = selectedChar;
    readingTextSourceInitialPositionPending_ = true;
    lineSourceTextOnly_ = true;
    readingBudgetFitActive_ = true;
    walkReference_.clear();   // a new pick: its own label again
    lineSlideIndex_ = 0;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
    readingPickBase_ = 0;
    lineOffset_ = 0;
    readingLinesCache_.clear();
    readingRunLinesCache_.clear();
    readingRunLinesLru_.clear();
    ResetReadingPick();
    costBuilt = costClock.elapsed();
    readingTextWindowCharStart_ = 0;
    readingFlowText_.clear();
    refreshReadingTextWindow(selectedChar);
    costWindow = costClock.elapsed();
    rebuildReadingRun(lineSlideIndex_, true);
    costRun = costClock.elapsed();
    const auto *model = OutputListModel::instance();
    const QString referenceStyle = model ? model->activeStyleId() : QString();
    const QVector<int> starts = styleRunLinesForText(referenceStyle, readingRunText_).charStarts;
    const int localChar = qMax(0, selectedChar - readingTextWindowCharStart_);
    costTable = costClock.elapsed();
    if (!starts.isEmpty()) {
        const auto at = std::upper_bound(starts.constBegin(), starts.constEnd(), localChar);
        lineOffset_ = qMax(0, static_cast<int>(at - starts.constBegin()) - 1);
        readingLayoutTargetChar_ = -1;
    } else {
        readingLayoutTargetChar_ = selectedChar;
    }
    readingScroll_ = lineOffset_;
    emit readingStageChanged();
    applyReadingRestage(false, false, true);
    if (costClock.elapsed() >= 40)
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                            QStringLiteral("activate total=%1ms build=%2 window=%3 run=%4 table=%5 restage=%6 chars=%7")
                                .arg(costClock.elapsed()).arg(costBuilt).arg(costWindow - costBuilt)
                                .arg(costRun - costWindow).arg(costTable - costRun)
                                .arg(costClock.elapsed() - costTable).arg(readingTextSourceText_.size()));
}

int LiveOutputService::readingPage() const
{
    return lineSourceTextOnly_ && !readingTextSourcePages_.isEmpty()
        ? readingTextSourcePageForChar(readingCharOffset()) : lineSlideIndex_;
}

int LiveOutputService::readingTextSourcePageForChar(int charOffset) const
{
    if (readingTextSourcePages_.isEmpty())
        return lineSlideIndex_;
    const auto it = std::upper_bound(readingTextSourcePageEnds_.cbegin(),
                                     readingTextSourcePageEnds_.cend(), charOffset);
    return it == readingTextSourcePageEnds_.cend()
        ? readingTextSourcePages_.size() - 1
        : static_cast<int>(it - readingTextSourcePageEnds_.cbegin());
}

LiveOutputService::StyleRunLines LiveOutputService::SpansToRunLines(const QString &text,
                                                                    const QVector<VisualSpan> &spans)
{
    StyleRunLines entry;
    entry.lines.reserve(spans.size());
    entry.charStarts.reserve(spans.size());
    for (const VisualSpan &span : spans) {
        entry.lines.append(text.mid(span.start, span.end - span.start));
        entry.charStarts.append(span.start);
    }
    return entry;
}

void LiveOutputService::ClearReadingTextSource()
{
    lineSourceTextOnly_ = false;
    readingTextSourcePages_.clear();
    readingTextSourcePageEnds_.clear();
    readingTextSourceText_.clear();
    readingTextSourceStartChar_ = -1;
    readingTextSourceInitialPositionPending_ = false;
    readingBudgetFitActive_ = false;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
}

void LiveOutputService::ResetReadingPick()
{
    resetReadingRun();
    readingRebaseSlide_ = -1;
    readingScroll_ = 0.0;
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
}

void LiveOutputService::resetReadingRun()
{
    readingRunFirst_ = -1;
    readingRunLast_ = -1;
    readingRunText_.clear();
    readingRunBaseLine_ = 0;
    readingRunCharBase_ = 0;
    readingRunSizeForStyle_.clear();
    readingRunPageStart_.clear();
    readingRunPageLines_.clear();
    readingSpanFrom_ = readingSpanTo_ = -1.0;   // a new run: no step is in flight
}

int LiveOutputService::readingCharSpan(int from, int to) const
{
    int total = 0;
    const int count = static_cast<int>(lineSetRaw_.size());
    for (int i = qMax(0, from); i < qMin(to, count); ++i) {
        const QString page = readingTextOf(lineSetRaw_.at(i).toMap());
        if (page.trimmed().isEmpty())
            continue;
        total += page.size() + (lineSourceTextOnly_ ? 0 : 1);
    }
    return total;
}

const LiveOutputService::StyleRunLines &LiveOutputService::styleRunLines(const QString &styleId) const
{
    return styleRunLinesForText(styleId, readingFlowText_);
}

bool LiveOutputService::refreshReadingTextWindow(int passageChar)
{
    if (!lineSourceTextOnly_ || readingTextSourceText_.isEmpty())
        return false;
    constexpr int kWindowChars = 6000;
    constexpr int kWindowMargin = 1200;
    const int size = readingTextSourceText_.size();
    const int currentStart = readingTextWindowCharStart_;
    const int currentEnd = currentStart + readingFlowText_.size();
    if (!readingFlowText_.isEmpty() && passageChar >= currentStart + kWindowMargin
            && passageChar <= currentEnd - kWindowMargin)
        return false;

    // THE POSITION GOES IN THE MIDDLE OF THE NEW WINDOW. It used to be the window's FIRST character, which is inside
    // the 1200-character edge margin, so the very next press found the position "near the edge" again and cut a new
    // window — on EVERY press, for any pick deep in a long text: a 6000-character layout of every column (30-45 ms of
    // DWrite on the interface thread), a stall of 100-190 ms, and the outputs told "resting at 0" each time, which is
    // how Main Output ended up a line behind the reference pane. Centred, the next cut is ~1800 characters of walking
    // away. (A passage near the start of a text never hit this, because the window already begins at 0 there.)
    const int desiredStart = qBound(0, passageChar - kWindowChars / 2, qMax(0, size - kWindowChars));
    int start = desiredStart;
    // Begin on a word, not inside one (the window's first line is read by nobody, but it must lay out like text).
    while (start > 0 && start < passageChar && !readingTextSourceText_.at(start - 1).isSpace())
        ++start;
    int end = qMin(size, desiredStart + kWindowChars);
    while (end < size && end > start && !readingTextSourceText_.at(end - 1).isSpace())
        --end;
    if (end <= start)
        end = qMin(size, start + kWindowChars);
    const QString windowText = readingTextSourceText_.mid(start, end - start);
    if (start == currentStart && windowText == readingFlowText_)
        return false;
    readingTextWindowCharStart_ = start;
    readingFlowText_ = windowText;
    readingRunText_ = readingFlowText_;
    return true;
}

qreal LiveOutputService::readingFlowCharForScroll(qreal lines) const
{
    // A FRESH PICK RESTS WHERE IT WAS MADE. The line numbers come from the reference style's table, which
    // is measured off-thread for a new passage and takes a moment; until it lands `lines` is just 0, and
    // the engine was told "resting at the passage's start" — NDI showed verse 1 after a double-click on
    // verse 3 until the first step. The pick knows the exact character it wants (the same one the table
    // will resolve to a line), so that is what the outputs are told in the meantime.
    if (lineSourceTextOnly_ && readingLayoutTargetChar_ >= 0)
        return qBound(0.0, static_cast<qreal>(readingLayoutTargetChar_ - readingTextWindowCharStart_),
                      static_cast<qreal>(readingFlowText_.size()));
    return qBound(0.0, readingCharForScroll(lines), static_cast<qreal>(readingFlowText_.size()));
}

const LiveOutputService::StyleRunLines &LiveOutputService::styleRunLinesForText(const QString &styleId,
                                                                              const QString &text) const
{
    static const StyleRunLines none;
    if (text.trimmed().isEmpty() || lineSetRaw_.isEmpty()
            || lineSlideIndex_ < 0 || lineSlideIndex_ >= lineSetRaw_.size())
        return none;
    const QString key = styleId + QLatin1Char('#') + QString::number(lineSlideIndex_)
                        + QLatin1Char('#') + QString::number(qHash(text), 16);
    if (lineSourceTextOnly_ && readingBudgetFitActive_) {
        const QString columnKey = ScrollColumnKey(styleId);
        const auto column = scrollColumns_.constFind(columnKey);
        if (column != scrollColumns_.constEnd() && column->size > 0.0) {
            const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                           + QString::number(qHash(text), 16);
            if (const StyleRunLines *hit = CachedStyleRunLines(columnCacheKey)) {
                return *hit;
            }
            QueueScrollColumnMeasure(columnKey, columnCacheKey, text,
                                     column->size, column->block, column->sig);
        }
        // NEVER "none". This branch used to answer an empty table while the budget column was
        // unseated or being measured, and that empty table is what the run stored — the step
        // then had nothing to move through (`lines=0`, `abs` stuck: measured on a real passage,
        // 121 presses, no movement) and no measure was even queued without a seat, so it was
        // permanent. Fall through to the reading block's own grid instead: it is the layout the
        // surfaces already draw, and the seated column's measured table replaces it when it lands.
    }
    if (readingBudgetFitActive_) {
        const QString columnKey = ScrollColumnKey(styleId);
        const auto column = scrollColumns_.constFind(columnKey);
        if (column != scrollColumns_.constEnd() && column->size > 0.0) {
            const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                           + QString::number(qHash(text), 16);
            if (const StyleRunLines *hit = CachedStyleRunLines(columnCacheKey))
                return *hit;
            // ONLY THE ACTIVE (REFERENCE) STYLE'S TABLE IS NEEDED THIS INSTANT: the walk, the step and the restage
            // read it. Every other output's table is only read by surfaces that cope with "not yet" (a tile shows the
            // whole text, the engine wraps itself, both re-cut the moment it lands), and the measure for it is already
            // queued off-thread above — measuring it here as well, on the interface thread, doubled the work and was
            // ~50-100 ms per extra style inside the first pick of every passage.
            if (lineSourceTextOnly_ && columnKey != ScrollColumnKey(QString()))
                return none;
            const QVector<VisualSpan> spans = visualLinesOfTextAtSize(text, column->block, column->size);
            return CacheStyleRunLines(columnCacheKey, SpansToRunLines(text, spans));
        }
    }
    if (const StyleRunLines *cached = CachedStyleRunLines(key))
        return *cached;
    QString fitText;
    const QVariantMap box = ReadingBoxForPage(styleId, &fitText);
    const double pageSize = ReadingPageSizeFor(styleId, box, fitText);
    const QVector<VisualSpan> spans = box.isEmpty() ? newlineSpansOf(text)
                                                   : visualLinesOfTextAtSize(text, box, pageSize);
    return CacheStyleRunLines(key, SpansToRunLines(text, spans));
}

QString LiveOutputService::ScrollColumnKey(const QString &styleId) const
{
    if (!styleId.isEmpty())
        return styleId;
    const auto *model = OutputListModel::instance();
    return model ? model->activeStyleId() : QString();
}

const LiveOutputService::StyleRunLines *LiveOutputService::CachedStyleRunLines(const QString &key) const
{
    const auto hit = readingRunLinesCache_.constFind(key);
    if (hit == readingRunLinesCache_.constEnd())
        return nullptr;
    readingRunLinesLru_.removeOne(key);
    readingRunLinesLru_.append(key);
    return &hit.value();
}

const LiveOutputService::StyleRunLines &LiveOutputService::CacheStyleRunLines(const QString &key,
                                                                             StyleRunLines entry) const
{
    constexpr int kReadingRunLinesCap = 16;
    while (readingRunLinesCache_.size() >= kReadingRunLinesCap && !readingRunLinesLru_.isEmpty())
        readingRunLinesCache_.remove(readingRunLinesLru_.takeFirst());
    readingRunLinesLru_.append(key);
    return *readingRunLinesCache_.insert(key, entry);
}

QVariantMap LiveOutputService::ReadingBoxForPage(const QString &styleId, QString *fitText) const
{
    const QVariantMap page = lineSetRaw_.at(lineSlideIndex_).toMap();
    const QVariantMap staged = stagedSlideRaw();
    const QVariantMap source = !staged.isEmpty() ? staged : page;
    const QVariantMap measured = previewWithStyle(source, styleId);
    const QVariantMap readingBlock = readingBlockOf(measured);
    QVariantMap box = readingBlock.isEmpty() ? readingBoxForWrapping(measured) : readingBlock;
    if (fitText) {
        *fitText = readingBlock.isEmpty()
            ? readingTextOf(page)
            : readingBlock.value(QStringLiteral("text")).toString();
    }
    const QString reportKey = ScrollColumnKey(styleId);
    auto reported = readingFitReported_.constFind(reportKey);
    if (reported == readingFitReported_.constEnd() && reportKey != styleId)
        reported = readingFitReported_.constFind(styleId);
    if (box.isEmpty() || reported == readingFitReported_.constEnd()
            || reported->width <= 0.0 || reported->height <= 0.0)
        return box;
    QVariantMap surfaceBox = box;
    surfaceBox.insert(QStringLiteral("width"), reported->width);
    surfaceBox.insert(QStringLiteral("height"), reported->height);
    QVariantMap style = box.value(QStringLiteral("style")).toMap();
    style.insert(QStringLiteral("padding"), reported->pad);
    surfaceBox.insert(QStringLiteral("style"), style);
    return surfaceBox;
}

double LiveOutputService::ReadingPageSizeFor(const QString &styleId, const QVariantMap &box,
                                             const QString &fitText) const
{
    const QString reportKey = ScrollColumnKey(styleId);
    auto reported = readingFitReported_.constFind(reportKey);
    if (reported == readingFitReported_.constEnd() && reportKey != styleId)
        reported = readingFitReported_.constFind(styleId);
    double pageSize = (reported != readingFitReported_.constEnd()) ? reported->size : 0.0;
    if (pageSize <= 0.0)
        pageSize = readingRunSizeForStyle_.value(reportKey, 0.0);
    if (pageSize > 0.0)
        return pageSize;
    const QVariantMap fitMeta = box.value(QStringLiteral("meta")).toMap();
    bool fontSizeOk = false;
    const double declaredFontSize = fitMeta.value(QStringLiteral("fontSize")).toDouble(&fontSizeOk);
    pageSize = (fontSizeOk && declaredFontSize > 0.0) ? declaredFontSize : 16.0;
    if (!box.isEmpty())
        readingRunSizeForStyle_.insert(reportKey, pageSize);
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFit"),
                        QStringLiteral("style=%1 size=%2 box=%3x%4 pad=%5 "
                                       "fitChars=%6 font=%7 (solved from the reading block)")
                            .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                            .arg(pageSize, 0, 'f', 2)
                            .arg(box.value(QStringLiteral("width")).toDouble())
                            .arg(box.value(QStringLiteral("height")).toDouble())
                            .arg(blockPadding(box))
                            .arg(fitText.size())
                            .arg(blockFontAt(box, pageSize).family()));
    return pageSize;
}

void LiveOutputService::QueueScrollColumnMeasure(const QString &columnKey, const QString &columnCacheKey,
                                                 const QString &text, qreal size,
                                                 const QVariantMap &block, const QString &sig) const
{
    if (readingRunLinesPending_.contains(columnCacheKey))
        return;
    readingRunLinesPending_.insert(columnCacheKey);
    const QString columnSig = sig;
    QVariantMap workerBlock = block;
    QVariantMap meta = workerBlock.value(QStringLiteral("meta")).toMap();
    meta.insert(QStringLiteral("resolvedFontFamily"),
                FontService::instance().resolve(meta.value(QStringLiteral("fontFamily")).toString()));
    meta.insert(QStringLiteral("fontFamilyResolved"), true);
    workerBlock.insert(QStringLiteral("meta"), meta);
    const double pixelSize = size;
    const QString workerText = text;
    QPointer<LiveOutputService> guard(const_cast<LiveOutputService *>(this));
    QThreadPool::globalInstance()->start(
        [guard, columnCacheKey, columnKey, columnSig, workerText, workerBlock, pixelSize]() {
            const QVector<VisualSpan> spans = visualLinesOfTextAtSize(
                workerText, workerBlock, pixelSize);
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard.data(),
                [guard, columnCacheKey, columnKey, columnSig, workerText, spans]() {
                    auto *self = guard.data();
                    if (!self)
                        return;
                    self->readingRunLinesPending_.remove(columnCacheKey);
                    const auto currentColumn = self->scrollColumns_.constFind(columnKey);
                    if (currentColumn == self->scrollColumns_.constEnd())
                        return;
                    if (currentColumn->sig != columnSig) {
                        if (self->readingFlowText_ == workerText
                                || self->readingRunText_ == workerText) {
                            self->QueueScrollColumnMeasure(columnKey, columnCacheKey, workerText,
                                                           currentColumn->size, currentColumn->block,
                                                           currentColumn->sig);
                        }
                        return;
                    }
                    StyleRunLines entry;
                    entry.lines.reserve(spans.size());
                    entry.charStarts.reserve(spans.size());
                    for (const VisualSpan &span : spans) {
                        entry.lines.append(workerText.mid(span.start, span.end - span.start));
                        entry.charStarts.append(span.start);
                    }
                    self->CacheStyleRunLines(columnCacheKey, entry);
                    if (self->lineSourceTextOnly_ && self->readingFlowText_ == workerText) {
                        const auto *model = OutputListModel::instance();
                        if (model && model->activeStyleId() == columnKey) {
                            self->readingRunPageLines_.insert(0, spans.size());
                            if (self->readingLayoutTargetChar_ >= 0 && !entry.charStarts.isEmpty()) {
                                const int localChar = qBound(0,
                                    self->readingLayoutTargetChar_ - self->readingTextWindowCharStart_,
                                    workerText.size());
                                const auto at = std::upper_bound(entry.charStarts.constBegin(),
                                    entry.charStarts.constEnd(), localChar);
                                const int line = qMax(0, static_cast<int>(
                                    at - entry.charStarts.constBegin()) - 1);
                                self->readingScroll_ = line;
                                self->lineOffset_ = line;
                                self->readingLayoutTargetChar_ = -1;
                            }
                        }
                        emit self->readingScrollChanged();
                        // The reference table is measured: the exact word window can now be cut,
                        // so the screens are handed it (they had the whole passage until now).
                        if (self->live_)
                            self->scheduleReadingRepublish();
                        QTimer::singleShot(0, self, &LiveOutputService::drainPendingReadingSteps);
                    }
                }, Qt::QueuedConnection);
        });
}

QStringList LiveOutputService::readingRunLinesForStyle(const QString &styleId) const
{
    return styleRunLines(styleId).lines;
}

void LiveOutputService::noteReadingFit(const QString &rawStyleId, qreal size,
                                      qreal boxWidth, qreal boxHeight, qreal pad)
{
    if (size <= 0.0)
        return;
    if (readingBudgetFitActive_)
        return;
    const QString styleId = (rawStyleId == QLatin1String("undefined")
                              || rawStyleId == QLatin1String("null"))
        ? QString() : rawStyleId;
    ReadingFit report;
    report.size = size;
    report.width = boxWidth;
    report.height = boxHeight;
    report.pad = pad;
    const auto sameColumn = [](const ReadingFit &a, const ReadingFit &b) {
        return qAbs(a.size - b.size) < 0.05
            && qAbs(a.width - b.width) < 0.25
            && qAbs(a.height - b.height) < 0.25
            && qAbs(a.pad - b.pad) < 0.05;
    };
    const auto previous = readingFitReported_.constFind(styleId);
    if (previous != readingFitReported_.constEnd() && sameColumn(*previous, report))
        return;                       // the same column: nothing to re-measure
    const bool havePrevious = previous != readingFitReported_.constEnd();
    const double previousSize = havePrevious ? previous->size : 0.0;
    const ReadingFit previousFit = havePrevious ? *previous : ReadingFit{};
    if (ReadingFitKeepsWalkColumn(styleId, report, previousFit, havePrevious)) {
        readingFitReported_.insert(styleId, report);
        LogReadingFitHeld(styleId, report, previousSize);
        return;
    }
    readingFitAcceptedJump_.insert(styleId, readingJumpSerial_);
    readingFitReported_.insert(styleId, report);
    readingRunSizeForStyle_.remove(styleId);
    PurgeReadingTablesForStyle(styleId);
    const auto *model = OutputListModel::instance();
    const QString reference = model ? model->activeStyleId() : QString();
    const bool resets = (styleId == reference);
    LogReadingFitAccepted(styleId, report, previousSize, resets);
    if (resets)
        applyReadingRestage(true, /*forceRebuild=*/ true);
}

bool LiveOutputService::ReadingFitKeepsWalkColumn(const QString &styleId, const ReadingFit &report,
                                                 const ReadingFit &previous, bool havePrevious)
{
    const bool walkLive = readingRunFirst_ >= 0 && readingRunLast_ >= readingRunFirst_;
    const bool sameBox = havePrevious
        && qAbs(previous.width - report.width) < 0.25
        && qAbs(previous.height - report.height) < 0.25
        && qAbs(previous.pad - report.pad) < 0.05;
    return walkLive && sameBox
        && readingFitAcceptedJump_.value(styleId, -1) == readingJumpSerial_;
}

void LiveOutputService::PurgeReadingTablesForStyle(const QString &styleId)
{
    QStringList purgePrefixes{ styleId + QLatin1Char('#') };
    if (styleId.isEmpty()) {
        const auto *model = OutputListModel::instance();
        const QString active = model ? model->activeStyleId() : QString();
        if (!active.isEmpty())
            purgePrefixes.append(active + QLatin1Char('#'));
    }
    for (const QString &prefix : purgePrefixes) {
        readingRunSizeForStyle_.remove(prefix.left(prefix.size() - 1));
        for (auto it = readingRunLinesCache_.begin(); it != readingRunLinesCache_.end(); ) {
            if (it.key().startsWith(prefix))
                it = readingRunLinesCache_.erase(it);
            else
                ++it;
        }
        readingRunLinesLru_.removeIf([&prefix](const QString &k) { return k.startsWith(prefix); });
    }
}

void LiveOutputService::LogReadingFitHeld(const QString &styleId, const ReadingFit &report,
                                         double previousSize)
{
    const auto *model = OutputListModel::instance();
    const QString reference = model ? model->activeStyleId() : QString();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFitNote"),
                        QStringLiteral("style='%1' pageFit=%2 columnSize=%3 box=%4x%5 "
                                       "pad=%6 reference='%7' held=YES "
                                       "(the walk keeps its column; page answer recorded)")
                            .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                            .arg(report.size, 0, 'f', 2)
                            .arg(previousSize, 0, 'f', 2)
                            .arg(report.width, 0, 'f', 2)
                            .arg(report.height, 0, 'f', 2)
                            .arg(report.pad, 0, 'f', 2)
                            .arg(reference.isEmpty() ? QStringLiteral("(ref)") : reference.left(8)));
}

void LiveOutputService::LogReadingFitAccepted(const QString &styleId, const ReadingFit &report,
                                              double previousSize, bool resets)
{
    const auto *model = OutputListModel::instance();
    const QString reference = model ? model->activeStyleId() : QString();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFitNote"),
                        QStringLiteral("style='%1' previousSize=%2 reportedSize=%3 "
                                       "box=%4x%5 pad=%6 reference='%7' resetsRun=%8 "
                                       "run=[%9..%10] jump=%11 acceptedJump=%12")
                            .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                            .arg(previousSize, 0, 'f', 2)
                            .arg(report.size, 0, 'f', 2)
                            .arg(report.width, 0, 'f', 2)
                            .arg(report.height, 0, 'f', 2)
                            .arg(report.pad, 0, 'f', 2)
                            .arg(reference.isEmpty() ? QStringLiteral("(ref)") : reference.left(8))
                            .arg(resets ? QStringLiteral("YES") : QStringLiteral("no"))
                            .arg(readingRunFirst_)
                            .arg(readingRunLast_)
                            .arg(readingJumpSerial_)
                            .arg(readingFitAcceptedJump_.value(styleId, -1)));
}

qreal LiveOutputService::scrollColumnSize(const QString &rawStyleId, const QVariantMap &block)
{
    if (!readingBudgetFitActive_ || block.isEmpty())
        return 0.0;
    const QString flow = lineSourceTextOnly_ ? readingTextSourceText_ : readingFlowText_;
    if (flow.trimmed().isEmpty())
        return 0.0;
    const int budget = qMax(3, readingWordBudget_);
    // THE WORDS THAT ARE SERVED ARE THE WORDS THAT GET FITTED: the budget's worth of words from the PICK on
    // (the page the reader is about to see), not the first words of the whole passage. A pick at verse 3 was
    // fitted on the opening of the chapter, so its words (longer or shorter ones) did not fill the box the
    // size had been solved for. The pick is part of the answer's key: another pick, another fit.
    const int pickChar = (lineSourceTextOnly_ && readingTextSourceStartChar_ >= 0)
        ? qBound(0, readingTextSourceStartChar_, static_cast<int>(flow.size())) : 0;

    QVariantMap shape;
    shape.insert(QStringLiteral("w"), block.value(QStringLiteral("width")));
    shape.insert(QStringLiteral("h"), block.value(QStringLiteral("height")));
    shape.insert(QStringLiteral("pad"), blockPadding(block));
    shape.insert(QStringLiteral("meta"), block.value(QStringLiteral("meta")));
    const QString sig = lineSetName_ + QLatin1Char('#') + QString::number(budget) + QLatin1Char('#')
        + QString::number(pickChar) + QLatin1Char('#')
        + QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(shape))
                                .toJson(QJsonDocument::Compact));

    qreal size = scrollSizeBySig_.value(sig, 0.0);
    if (size <= 0.0) {
        // SOLVED OFF THE INTERFACE THREAD. The fit is a bisection of eleven text layouts; run inline from a
        // QML binding it blocked the thread once per tile on every Max-words / template / size change.
        // The answer is 0 ("not yet") now — DesignPreview already treats that as pending and asks again
        // when readingScrollChanged fires — and the worker posts the size back.
        if (scrollSizePending_.contains(sig))
            return 0.0;
        QStringList words;
        for (int i = pickChar, n = flow.size(); i < n && words.size() < budget; ) {
            const int start = i;
            while (i < n && !flow.at(i).isSpace())
                ++i;
            while (i < n && flow.at(i).isSpace())
                ++i;
            if (i > start)
                words.append(flow.mid(start, i - start));
        }
        if (words.isEmpty())
            return 0.0;
        QString probe;
        for (int i = 0; i < budget; ++i)
            probe += words.at(i % words.size());
        QVariantMap grown = block;
        QVariantMap meta = grown.value(QStringLiteral("meta")).toMap();
        meta.insert(QStringLiteral("autoSize"), QStringLiteral("grow"));
        // The family is resolved HERE (the font database is the interface thread's): the worker only
        // measures, exactly as QueueScrollColumnMeasure does.
        meta.insert(QStringLiteral("resolvedFontFamily"),
                    FontService::instance().resolve(meta.value(QStringLiteral("fontFamily")).toString()));
        meta.insert(QStringLiteral("fontFamilyResolved"), true);
        grown.insert(QStringLiteral("meta"), meta);
        const QString probeText = probe.trimmed();
        const QString styleLabel = rawStyleId.isEmpty() ? QStringLiteral("(ref)") : rawStyleId.left(8);
        const QString pickLabel = lineSetName_.left(24);
        const double boxW = block.value(QStringLiteral("width")).toDouble();
        const double boxH = block.value(QStringLiteral("height")).toDouble();
        const double pad = blockPadding(block);
        // THE SOLVE RUNS RIGHT HERE, ON THE INTERFACE THREAD, unless this style has proved slow. A worker thread has cold
        // font caches (Qt keeps them per thread): off-thread, every pick waited 0.3-0.8 s for its size (the passage hidden
        // or at a stale size meanwhile), while on the interface thread — whose fonts the tiles already drew with — the
        // same bisection measured 9-27 ms per style. The size is therefore exact from the first frame of every pick.
        // The answer is cached by (pick, budget, box shape), so N surfaces of one style cost one solve; a style whose
        // inline solve ever took longer than kInlineSlowMs (a huge box or an exotic font) goes back to the worker.
        const auto *solveModel = OutputListModel::instance();
        const QString solveKey = !rawStyleId.isEmpty() && rawStyleId != QLatin1String("undefined")
                                 && rawStyleId != QLatin1String("null")
            ? rawStyleId : (solveModel ? solveModel->activeStyleId() : QString());
        constexpr qint64 kInlineSlowMs = 120;
        if (!inlineSlowStyles_.contains(solveKey)) {
            QElapsedTimer inlineClock;
            inlineClock.start();
            const double solved = fittedPixelSize(probeText, grown);
            if (inlineClock.elapsed() > kInlineSlowMs)
                inlineSlowStyles_.insert(solveKey);
            if (scrollSizeBySig_.size() >= 64)
                scrollSizeBySig_.clear();
            scrollSizeBySig_.insert(sig, solved);
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScrollColumn"),
                                QStringLiteral("style=%1 pick='%2' budget=%3 size=%4 box=%5x%6 pad=%7 "
                                               "(solved inline in %8 ms)")
                                    .arg(styleLabel).arg(pickLabel).arg(budget)
                                    .arg(solved, 0, 'f', 2).arg(boxW).arg(boxH).arg(pad)
                                    .arg(inlineClock.elapsed()));
            size = solved;
        } else {
        scrollSizePending_.insert(sig);
        QPointer<LiveOutputService> guard(this);
        QThreadPool::globalInstance()->start(
            [guard, sig, probeText, grown, budget, styleLabel, pickLabel, boxW, boxH, pad]() {
                const double solved = fittedPixelSize(probeText, grown);
                if (!guard)
                    return;
                QMetaObject::invokeMethod(guard.data(), [guard, sig, solved, budget, styleLabel,
                                                         pickLabel, boxW, boxH, pad]() {
                    auto *self = guard.data();
                    if (!self)
                        return;
                    self->scrollSizePending_.remove(sig);
                    if (self->scrollSizeBySig_.size() >= 64)
                        self->scrollSizeBySig_.clear();
                    self->scrollSizeBySig_.insert(sig, solved);
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScrollColumn"),
                                        QStringLiteral("style=%1 pick='%2' budget=%3 size=%4 box=%5x%6 pad=%7 "
                                                       "(solved once: the max-words budget into the text area)")
                                            .arg(styleLabel).arg(pickLabel).arg(budget)
                                            .arg(solved, 0, 'f', 2).arg(boxW).arg(boxH).arg(pad));
                    emit self->readingScrollChanged();   // the surfaces that asked get to ask again
                }, Qt::QueuedConnection);
            });
        return 0.0;
        }
    }

    const auto *styleModel = OutputListModel::instance();
    const bool named = !rawStyleId.isEmpty() && rawStyleId != QLatin1String("undefined")
                       && rawStyleId != QLatin1String("null");
    const QString active = styleModel ? styleModel->activeStyleId() : QString();
    const QString key = named ? rawStyleId : active;
    if (key.isEmpty())
        return size;
    const auto seated = scrollColumns_.constFind(key);
    const bool vacant = seated == scrollColumns_.constEnd();
    // AN UNNAMED ASK DOES NOT SEAT A VACANT COLUMN AT ONCE. The reference pane's preview asks without naming a
    // style, and it asks first (its box is the pane's, not the output's): seating the active style's column from
    // it gave that style a table wrapped at the wrong width for as long as its own surface took to solve its
    // size — and every output was handed those forced breaks (a flash of wrong line breaks after a pick).
    // The style's own surface names itself within a moment; only if none does (no tile or output shows this
    // style) does the unnamed ask seat it, after a grace period.
    if (!named && vacant && !unnamedSeatOpen_) {
        if (!unnamedSeatArmed_) {
            unnamedSeatArmed_ = true;
            QTimer::singleShot(1500, this, [this]() {
                unnamedSeatArmed_ = false;
                unnamedSeatOpen_ = true;
                emit readingScrollChanged();   // the surfaces ask again; a vacant column is seated then
            });
        }
        return size;
    }
    if ((named || vacant) && (vacant || seated->sig != sig)) {
        const qreal previousChar = readingCharForScroll(readingScroll_);
        const int preserveChar = lineSourceTextOnly_
            ? (readingTextSourceInitialPositionPending_
                   ? readingTextSourceStartChar_
                   : readingTextWindowCharStart_ + static_cast<int>(std::floor(previousChar)))
            : static_cast<int>(std::floor(previousChar));
        ScrollColumn column;
        column.block = block;
        column.size = size;
        column.sig = sig;
        scrollColumns_.insert(key, column);
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScrollSeat"),
                            QStringLiteral("column=%1 asked-as=%2 size=%3 box=%4x%5 reseated=%6")
                                .arg(key.left(8), named ? rawStyleId.left(8) : QStringLiteral("(unnamed)"))
                                .arg(size, 0, 'f', 2)
                                .arg(block.value(QStringLiteral("width")).toDouble())
                                .arg(block.value(QStringLiteral("height")).toDouble())
                                .arg(!vacant));
        QTimer::singleShot(0, this, [this]() {
            if (auto *outputs = OutputListModel::instance())
                outputs->repushEngineStyles();
            if (live_)
                scheduleReadingRepublish();   // this column's line breaks join the slide's
            // THE RUN THAT WAS DERIVED BEFORE THIS COLUMN EXISTED. Seating the column is what
            // makes the reference style's line table derivable at all, and nothing else asked
            // for it again — the run kept the empty table it stored while the column was still
            // being solved. Deferred to this turn, so the rebuild never happens inside the
            // binding that asked for the size.
            RepairReadingRunWithoutLines();
        });
        const QString prefix = QStringLiteral("c#") + key + QLatin1Char('#');
        for (auto it = readingRunLinesCache_.begin(); it != readingRunLinesCache_.end(); ) {
            if (it.key().startsWith(prefix))
                it = readingRunLinesCache_.erase(it);
            else
                ++it;
        }
        readingRunLinesLru_.removeIf([&prefix](const QString &k) { return k.startsWith(prefix); });
        if (key == active && lineSourceTextOnly_) {
            const QVector<int> starts = styleRunLinesForText(key, readingRunText_).charStarts;
            if (!starts.isEmpty()) {
                const int localChar = lineSourceTextOnly_
                    ? qMax(0, preserveChar - readingTextWindowCharStart_) : preserveChar;
                const auto at = std::upper_bound(starts.constBegin(), starts.constEnd(), localChar);
                lineOffset_ = qMax(0, static_cast<int>(at - starts.constBegin()) - 1);
                readingScroll_ = lineOffset_;
            }
            readingTextSourceInitialPositionPending_ = false;
            applyReadingRestage(true, /*forceRebuild=*/ true);
        } else if (key == active) {
            applyReadingRestage(true, /*forceRebuild=*/ true);
        }
    } else if (key == active && lineSourceTextOnly_) {
        readingTextSourceInitialPositionPending_ = false;
    }
    unnamedSeatOpen_ = false;   // the grace period served this ask; the next vacancy waits again
    return size;
}

qreal LiveOutputService::readingColumnPxFor(const QString &styleId) const
{
    if (!scrollMode_ || !readingBudgetFitActive_)
        return 0.0;
    const auto *model = OutputListModel::instance();
    const QString key = styleId.isEmpty() ? (model ? model->activeStyleId() : QString()) : styleId;
    const auto it = scrollColumns_.constFind(key);
    // WHOLE PIXELS, as the QML surfaces draw it: Qt.font({ pixelSize }) takes an integer, so the
    // monitor sets 31.88 as 31 — and an engine scene at the exact 31.88 is 3% larger, wraps a few
    // characters earlier and puts different words on each line ("the words are not the same").
    return it == scrollColumns_.constEnd() ? 0.0 : std::floor(it->size);
}

// ---- WHAT THE OUTPUTS DRAW OF THE PASSAGE -------------------------------------------
QVariantMap LiveOutputService::readingFlowSliceFor(const QString &styleId, int firstLine, int lineCount) const
{
    // A SURFACE LAYS OUT WHAT IT CAN SHOW, NOT THE WHOLE PASSAGE. Every tile used to lay out the full 3-6k
    // characters (justified) in a QML Text on the interface thread whenever the passage changed — on a pick
    // that was ~0.25-0.5 s for the set of tiles. A slice that STARTS AT A LINE START of this style's table
    // breaks exactly as the full text does (line breaking is greedy: a line depends only on where it
    // starts), so the surface gets the same lines for a fraction of the layout.
    QVariantMap out;
    const QVector<int> &starts = styleRunLines(styleId).charStarts;
    const int total = static_cast<int>(starts.size());
    // NO TABLE YET FOR THIS STYLE (its measure is still running off-thread): the surface used to be handed the WHOLE
    // passage and lay all of it out, justified — four to six surfaces at 3-6k characters each, ~50 ms apiece, which was
    // most of the stall after a pick — only to cut it down the moment the table landed. A bounded stretch from the
    // walk's own position is enough to draw the screen it can show until then (the table's tell re-cuts it exactly).
    constexpr int kProvisionalChars = 1500;
    if (total == 0 && lineSourceTextOnly_ && readingBudgetFitActive_ && lineCount > 0
            && readingFlowText_.size() > 2 * kProvisionalChars) {
        const int size = static_cast<int>(readingFlowText_.size());
        const int at = qBound(0, static_cast<int>(std::floor(readingCharForScroll(readingScroll_))), size - 1);
        int from = at;
        while (from > 0 && !readingFlowText_.at(from - 1).isSpace())
            --from;                       // begin on a word
        int to = qMin(size, from + kProvisionalChars);
        while (to < size && !readingFlowText_.at(to - 1).isSpace())
            ++to;                         // end on a word
        out.insert(QStringLiteral("text"), readingFlowText_.mid(from, to - from));
        out.insert(QStringLiteral("first"), 0);
        out.insert(QStringLiteral("lines"), 0);
        out.insert(QStringLiteral("whole"), false);
        out.insert(QStringLiteral("atEnd"), to >= size);
        out.insert(QStringLiteral("provisional"), true);
        return out;
    }
    if (total == 0 || readingFlowText_.isEmpty() || lineCount <= 0) {
        out.insert(QStringLiteral("text"), readingFlowText_);
        out.insert(QStringLiteral("first"), 0);
        out.insert(QStringLiteral("lines"), total);
        out.insert(QStringLiteral("whole"), true);
        return out;
    }
    const int a = qBound(0, firstLine, total - 1);
    const int b = qMin(total, a + lineCount);
    const int from = starts.at(a);
    const int to = b < total ? starts.at(b) : static_cast<int>(readingFlowText_.size());
    const QString sliceText = readingFlowText_.mid(from, to - from);
    out.insert(QStringLiteral("text"), sliceText);
    out.insert(QStringLiteral("first"), a);
    out.insert(QStringLiteral("lines"), b - a);
    out.insert(QStringLiteral("whole"), a == 0 && b == total);
    out.insert(QStringLiteral("atEnd"), b == total);
    return out;
}

int LiveOutputService::readingVisibleLinesFor(const QString &styleId) const
{
    const auto *model = OutputListModel::instance();
    const QString key = styleId.isEmpty() ? (model ? model->activeStyleId() : QString()) : styleId;
    const auto column = scrollColumns_.constFind(key);
    if (column == scrollColumns_.constEnd() || column->size <= 0.0)
        return 0;
    // HOW MANY WHOLE LINES THE TEXT AREA HOLDS — by the very rule the font solver fits the budget with
    // and the engine's outputs clip by: the line pitch of the (whole-pixel) column size against the box's
    // inner height. The monitors used their own measured pitch, a hair larger, so a box that held three
    // lines on NDI read as two on the monitor tile.
    const QVariantMap meta = column->block.value(QStringLiteral("meta")).toMap();
    const QFont font = blockFontAt(column->block, column->size);
    const double step = bps::presentation::template_text_metrics::linePitch(
        QFontMetricsF(font).height(), metaLineFactor(meta), metaLineGap(meta));
    const double height = column->block.value(QStringLiteral("height"), 40.0).toDouble()
                          - blockPadding(column->block) * 2.0;
    return step > 0.0 ? qMax(1, wholeLinesIn(height, step)) : 0;
}

QVariantMap LiveOutputService::readingBreaksFor(const QString &text) const
{
    QVariantMap breaks;
    if (text.isEmpty())
        return breaks;
    QElapsedTimer breaksClock;
    breaksClock.start();
    int laidOut = 0;
    const auto logCost = qScopeGuard([&]() {
        if (breaksClock.elapsed() >= 30)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                                QStringLiteral("readingBreaksFor %1ms columns=%2 laidOut=%3 chars=%4")
                                    .arg(breaksClock.elapsed()).arg(scrollColumns_.size()).arg(laidOut)
                                    .arg(text.size()));
    });
    // One table per output style, cut at the stage's own size from the style's own column — the same cut
    // the monitors' tiles make, handed to the engine so its outputs draw those lines.
    for (auto it = scrollColumns_.constBegin(); it != scrollColumns_.constEnd(); ++it) {
        if (it->size <= 0.0)
            continue;
        // CUT ONCE PER (column, size, text): the same passage is attached on every hand-off, and laying out
        // a few thousand characters is the expensive part. A bounded cache makes a repeat free.
        const QString cacheKey = it.key() + QLatin1Char('#') + it->sig + QLatin1Char('#')
                                 + QString::number(it->size, 'f', 2) + QLatin1Char('#')
                                 + QString::number(qHash(text), 16);
        const auto hit = readingBreaksCache_.constFind(cacheKey);
        if (hit != readingBreaksCache_.constEnd()) {
            breaks.insert(it.key(), hit.value());
            continue;
        }
        ++laidOut;
        const QVector<VisualSpan> spans = visualLinesOfTextAtSize(text, it->block, it->size);
        QVariantList starts;
        starts.reserve(spans.size());
        for (const VisualSpan &span : spans)
            starts.append(span.start);
        if (readingBreaksCache_.size() >= 48)
            readingBreaksCache_.clear();
        readingBreaksCache_.insert(cacheKey, starts);
        breaks.insert(it.key(), starts);
    }
    return breaks;
}

void LiveOutputService::attachReadingFlow(QVariantMap &slide, qreal lines) const
{
    setSlideReadingFlow(slide, scrollMode_, readingFlowText_, readingFlowCharForScroll(lines));
    // The long column: every output draws the line breaks cut for its style at the stage's size (the very
    // lines the monitors' tiles scroll through), so NDI/HDMI put the same words on each line.
    if (scrollMode_ && lineSourceTextOnly_ && readingBudgetFitActive_ && !readingFlowText_.isEmpty())
        slide.insert(QStringLiteral("flowBreaks"), readingBreaksFor(readingFlowText_));
    else
        slide.remove(QStringLiteral("flowBreaks"));
}

qreal LiveOutputService::readingRunSizeForStyle(const QString &styleId) const
{
    QString key = styleId;
    if (key.isEmpty()) {
        const auto *model = OutputListModel::instance();
        key = model ? model->activeStyleId() : QString();
    }
    const auto it = readingRunSizeForStyle_.constFind(key);
    return it == readingRunSizeForStyle_.constEnd() ? -1.0 : it.value();
}

QVariantList LiveOutputService::readingRunLineStarts(const QString &styleId) const
{
    QVariantList out;
    const QVector<int> starts = styleRunLines(styleId).charStarts;
    out.reserve(starts.size());
    for (int start : starts)
        out.append(start);
    return out;
}

int LiveOutputService::readingRunLineCount(const QString &styleId) const
{
    return static_cast<int>(styleRunLines(styleId).charStarts.size());   // no table copy, no QVariantList
}

bool LiveOutputService::readingColumnTableMeasured(const QString &styleId) const
{
    if (!readingBudgetFitActive_ || readingFlowText_.isEmpty())
        return false;
    const QString columnKey = ScrollColumnKey(styleId);
    const auto column = scrollColumns_.constFind(columnKey);
    if (column == scrollColumns_.constEnd() || column->size <= 0.0)
        return false;
    const QString cacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                             + QString::number(qHash(readingFlowText_), 16);
    return readingRunLinesCache_.contains(cacheKey);   // (no LRU touch: this is a question, not a use)
}

qreal LiveOutputService::readingLineForStyle(const QString &styleId, qreal charAt) const
{
    const QVector<int> &starts = styleRunLines(styleId).charStarts;
    if (starts.isEmpty())
        return lineSourceTextOnly_ ? readingScroll_ : charAt;
    if (charAt < 0.0)
        return charAt;
    if (lineSourceTextOnly_)
        charAt = qMax(0.0, charAt - readingTextWindowCharStart_);
    // charAt is already the true character (readingCharForScroll); this style's line is
    // read off ITS table by the function the engine's renderers use too: whole lines of its own
    // wrap, glided between the step's two lines (see SnappedLineForChar).
    return bps::rendering::flowscroll::SnappedLineForChar(starts, charAt, readingSpanFrom_, readingSpanTo_);
}

int LiveOutputService::readingCharOffset() const
{
    if (readingRunText_.isEmpty() || readingRunFirst_ < 0)
        return -1;
    const qreal at = readingCharForScroll(readingScroll_);
    const int local = static_cast<int>(std::floor(at));
    return lineSourceTextOnly_ ? readingTextWindowCharStart_ + local : local;
}

int LiveOutputService::readingPassageChar() const
{
    const int local = readingCharOffset();
    if (local < 0)
        return -1;
    return lineSourceTextOnly_ ? local : readingRunCharBase_ + local;
}

int LiveOutputService::readingPageLineCount(int index) const
{
    const auto it = readingRunPageLines_.constFind(index);
    if (it != readingRunPageLines_.constEnd())
        return it.value();
    if (lineSourceTextOnly_)
        return 0;
    return static_cast<int>(readingLinesAt(index).size());
}

int LiveOutputService::readingBaseForSlide(int index) const
{
    if (readingRunFirst_ < 0 || index <= readingRunFirst_)
        return 0;
    const auto inRun = readingRunPageStart_.constFind(index);
    if (inRun != readingRunPageStart_.constEnd()) {
        const auto *model = OutputListModel::instance();
        const QString ref = model ? model->activeStyleId() : QString();
        const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
        const int at = inRun.value();
        int count = 0;
        for (int s : starts) {
            if (s < at)
                ++count;
            else
                break;
        }
        return count;
    }
    int base = 0;
    for (int i = readingRunFirst_; i < index && i < static_cast<int>(lineSetRaw_.size()); ++i)
        base += readingPageLineCount(i);
    return base;
}

void LiveOutputService::shiftPickBase(int from, int to)
{
    if (to > from) {
        for (int i = from; i < to; ++i)
            readingPickBase_ += readingPageLineCount(i);
    } else if (to < from) {
        for (int i = to; i < from; ++i)
            readingPickBase_ -= readingPageLineCount(i);
    }
}

qreal LiveOutputService::readingAbsoluteLine() const
{
    const qreal inside = (readingRunFirst_ >= 0)
        ? (readingScroll_ - readingRunBaseLine_)
        : static_cast<qreal>(lineOffset_);
    return static_cast<qreal>(readingPickBase_) + inside;
}

qreal LiveOutputService::readingWindowOffset(qreal absolute) const
{
    const qreal inside = absolute - static_cast<qreal>(readingPickBase_);
    return (readingRunFirst_ >= 0)
        ? (static_cast<qreal>(readingRunBaseLine_) + inside)
        : inside;
}

void LiveOutputService::announceReadingPage()
{
    const int page = readingPage();
    if (readingPageAnnounced_ == page)
        return;
    readingPageAnnounced_ = page;
    emit readingPageChanged();
}

void LiveOutputService::rebuildReadingRun(int anchor, bool force)
{
    const int count = static_cast<int>(lineSetRaw_.size());
    if (count == 0) {
        resetReadingRun();
        return;
    }
    constexpr int kReadingRunWholePick = 2 * LiveOutputService::kReadingRunRadius * 2 + 1;
    const bool wholePick = !lineSourceTextOnly_ && count <= kReadingRunWholePick;
    const int first = wholePick ? 0 : qMax(0, anchor - kReadingRunRadius);
    const int last = wholePick ? count - 1 : qMin(count - 1, anchor + kReadingRunRadius);
    if (!force && first == readingRunFirst_ && last == readingRunLast_)
        return;                        // same window: the column is unchanged
    const int previousBase = readingRunFirst_ >= 0
        ? readingBaseForSlide(lineSlideIndex_) : -1;
    const int previousFirst = readingRunFirst_;
    readingRunFirst_ = first;
    readingRunLast_ = last;
    if (previousFirst < 0)
        readingRunCharBase_ = readingCharSpan(0, first);
    else if (first > previousFirst)
        readingRunCharBase_ += readingCharSpan(previousFirst, first);
    else if (first < previousFirst)
        readingRunCharBase_ -= readingCharSpan(first, previousFirst);

    BuildReadingRunText(first, last);
    CountReadingRunPageLines();
    readingRunBaseLine_ = readingBaseForSlide(lineSlideIndex_);
    LogReadingRunShape(first, last);
    RebaseReadingWindow(previousBase, first, last);
}

void LiveOutputService::BuildReadingRunText(int first, int last)
{
    QString run;
    readingRunPageStart_.clear();
    readingRunPageLines_.clear();
    for (int i = first; i <= last; ++i) {
        QString page = lineSourceTextOnly_
            ? readingFlowText_ : readingTextOf(lineSetRaw_.at(i).toMap());
        if (page.trimmed().isEmpty())
            continue;
        if (!run.isEmpty()) {
            if (lineSourceTextOnly_) {
                readingRunPageStart_.insert(i, static_cast<int>(run.size()));
                run += page;
                continue;
            }
            while (page.startsWith(QLatin1Char('\n')) || page.startsWith(QLatin1Char('\r')))
                page.remove(0, 1);
            while (run.endsWith(QLatin1Char('\n')) || run.endsWith(QLatin1Char('\r')))
                run.chop(1);
            run += QLatin1Char('\n');
        } else {
            while (page.endsWith(QLatin1Char('\n')) || page.endsWith(QLatin1Char('\r')))
                page.chop(1);
        }
        readingRunPageStart_.insert(i, static_cast<int>(run.size()));
        run += page;
    }
    readingRunText_ = run;
}

void LiveOutputService::CountReadingRunPageLines()
{
    const auto *model = OutputListModel::instance();
    const QString refStyle = model ? model->activeStyleId() : QString();
    const QVector<int> starts = styleRunLinesForText(refStyle, readingRunText_).charStarts;
    if (starts.isEmpty())
        return;
    QList<int> pages = readingRunPageStart_.keys();
    std::sort(pages.begin(), pages.end());
    for (int k = 0; k < pages.size(); ++k) {
        const int from = readingRunPageStart_.value(pages.at(k));
        const int to = (k + 1 < pages.size()) ? readingRunPageStart_.value(pages.at(k + 1))
                                              : static_cast<int>(readingRunText_.size()) + 1;
        int count = 0;
        for (int s : starts) {
            if (s < from)
                continue;
            if (s >= to)
                break;
            ++count;
        }
        readingRunPageLines_.insert(pages.at(k), count);
    }
}

void LiveOutputService::LogReadingRunShape(int first, int last)
{
    const auto *model = OutputListModel::instance();
    const QString refStyle = model ? model->activeStyleId() : QString();
    int pageSum = 0;
    int pageOwnSum = 0;
    QString pageLines;
    for (int i = first; i <= last; ++i) {
        const int n = readingPageLineCount(i);
        const int own = lineSourceTextOnly_
            ? 0 : static_cast<int>(readingLinesAt(i).size());
        pageSum += n;
        pageOwnSum += own;
        if (i > first)
            pageLines += QLatin1Char(',');
        pageLines += QString::number(n);
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRunShape"),
                        QStringLiteral("window=[%1..%2] runChars=%3 tableLines=%4 "
                                       "stepLines=%5 pageOwnSum=%6 pageLines=[%7] "
                                       "(stepLines == tableLines is the requirement; "
                                       "pageOwnSum is the per-page fitted grid)")
                            .arg(first)
                            .arg(last)
                            .arg(readingRunText_.size())
                            .arg(styleRunLinesForText(refStyle, readingRunText_).lines.size())
                            .arg(pageSum)
                            .arg(pageOwnSum)
                            .arg(pageLines));
}

void LiveOutputService::RebaseReadingWindow(int previousBase, int first, int last)
{
    if (previousBase < 0)
        return;
    const int shift = readingRunBaseLine_ - previousBase;
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRun"),
                        QStringLiteral("window=[%1..%2] base=%3 rebase=%4 scroll=%5")
                            .arg(first)
                            .arg(last)
                            .arg(readingRunBaseLine_)
                            .arg(shift)
                            .arg(readingScroll_, 0, 'f', 2));
    if (shift != 0)
        readingScroll_ += shift;
}

void LiveOutputService::reseadReadingWalk()
{
    if (lineSetRaw_.isEmpty() || lineSlideIndex_ < 0
            || lineSlideIndex_ >= lineSetRaw_.size())
        return;
    readingLinesCache_.clear();   // every count belongs to the OLD picture
    resetReadingRun();            // ...and so does the run built from them
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    readingRebaseSlide_ = -1;     // no page change owed by the old numbering
    const int ownLines = readingPageLineCount(lineSlideIndex_);
    lineOffset_ = qBound(0, lineOffset_, qMax(0, ownLines - 1));
    readingPickBase_ = 0;         // the walk's origin is the page it is reading
    applyReadingRestage();        // rebuild the run, re-stage, announce the page
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadReseat"),
                        QStringLiteral("set=%1 page=%2 lines=%3 offset=%4 scroll=%5 "
                                       "(the measurement basis changed: style/roster)")
                            .arg(lineSetName_)
                            .arg(lineSlideIndex_)
                            .arg(ownLines)
                            .arg(lineOffset_)
                            .arg(readingScroll_, 0, 'f', 2));
}

void LiveOutputService::connectReadingBasis()
{
    if (readingBasisConnected_)
        return;
    if (auto *model = OutputListModel::instance()) {
        readingBasisConnected_ = true;
        connect(model, &OutputListModel::activeStyleChanged,
                this, &LiveOutputService::reseadReadingWalk);
        // READY BEFORE THE FIRST PICK: see prewarmReadingFonts.
        QTimer::singleShot(2500, this, &LiveOutputService::prewarmReadingFonts);
    }
}

// THE READING FONTS, LOADED AT BOOT INSTEAD OF AT THE FIRST DOUBLE-CLICK. The first pick of a session paid for the
// first-ever layout of the output styles' reading fonts (the font files opened, their tables read, the shaping caches
// filled): measured 0.6 s of the interface thread inside that pick, against 0.05-0.2 s for every later one. A short
// passage laid out once per output style, right after the interface is up, moves that cost to idle time. One style per
// turn of the event loop, so no frame waits on more than one font.
void LiveOutputService::prewarmReadingFonts()
{
    if (readingPrewarmQueue_.isEmpty()) {
        if (readingPrewarmed_)
            return;
        readingPrewarmed_ = true;
        const auto *outputs = OutputListModel::instance();
        if (!outputs)
            return;
        QStringList ids;
        for (int i = 0; i < outputs->rowCount(); ++i) {
            const QString id = outputs->getOutput(i).value(QStringLiteral("styleId")).toString();
            if (!id.isEmpty() && !ids.contains(id))
                ids.append(id);
        }
        const QString active = outputs->activeStyleId();
        if (!active.isEmpty() && !ids.contains(active))
            ids.append(active);
        readingPrewarmQueue_ = ids;
        readingPrewarmClock_.start();
    }
    if (readingPrewarmQueue_.isEmpty())
        return;
    const QString styleId = readingPrewarmQueue_.takeFirst();
    QElapsedTimer clock;
    clock.start();
    static const QString sample = QStringLiteral(
        "5 And God called the light Day, and the darkness He called Night. “Let there be light”—and there "
        "was light. 1 2 3 4 5 6 7 8 9 0 [Brother Bosworth gives a solo—Ed.] My, that was very fine.");
    // A passage-shaped slide, composed by the style: its text blocks carry the fonts, sizes and boxes the picks will use.
    QVariantMap slide;
    slide.insert(QStringLiteral("contentType"), QStringLiteral("scripture"));
    slide.insert(QStringLiteral("line1"), QStringLiteral("Genesis 1:5"));
    slide.insert(QStringLiteral("line2"), sample);
    slide.insert(QStringLiteral("body"), sample);
    const QVariantMap composed = previewWithStyle(slide, styleId);
    int blocks = 0;
    for (const QVariant &value : composed.value(QStringLiteral("blocks")).toList()) {
        const QVariantMap block = value.toMap();
        const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
        if (block.value(QStringLiteral("kind")).toString() != QLatin1String("text")
                || meta.value(QStringLiteral("fontFamily")).toString().isEmpty())
            continue;
        for (const double size : { 24.0, 44.0 })
            (void)visualLinesOfTextAtSize(sample, block, size);
        ++blocks;
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadingWarm"),
                        QStringLiteral("style=%1 blocks=%2 text blocks warmed=%3 in %4 ms (fonts loaded before the first pick)")
                            .arg(styleId.left(8)).arg(composed.value(QStringLiteral("blocks")).toList().size())
                            .arg(blocks).arg(clock.elapsed()));
    if (!readingPrewarmQueue_.isEmpty())
        QTimer::singleShot(40, this, &LiveOutputService::prewarmReadingFonts);   // the next style: its own turn
    else
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadingWarm"),
                            QStringLiteral("done in %1 ms").arg(readingPrewarmClock_.elapsed()));
}

void LiveOutputService::applyReadingScroll(qreal lines, bool notify)
{
    readingScroll_ = lines;
    bool windowChanged = false;
    if (lineSourceTextOnly_) {
        const int passageChar = readingTextWindowCharStart_
            + static_cast<int>(std::floor(readingCharForScroll(lines)));
        const int windowEnd = readingTextWindowCharStart_ + readingFlowText_.size();
        const bool nearEdge = passageChar < readingTextWindowCharStart_ + 1200
                              || passageChar > windowEnd - 1200;
        if (nearEdge && (readingScrollPreparing_ || (readingScrollAnim_
                && readingScrollAnim_->state() == QAbstractAnimation::Running))) {
            readingWindowRefreshPending_ = true;
        } else if (nearEdge) {
            windowChanged = refreshReadingTextWindow(passageChar);
            readingWindowRefreshPending_ = false;
            if (windowChanged) {
                const auto *model = OutputListModel::instance();
                const QString ref = model ? model->activeStyleId() : QString();
                const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
                readingRunPageLines_.insert(0, starts.size());
                readingLayoutTargetChar_ = passageChar;
                const qreal localLine = starts.isEmpty()
                    ? 0.0 : readingLineForStyle(ref, passageChar);
                lineOffset_ = localLine;
                readingScroll_ = localLine;
                lines = localLine;
                if (!starts.isEmpty())
                    readingLayoutTargetChar_ = -1;
            }
        }
    }
    if (lineSourceTextOnly_) {
        const int page = readingPage();
        if (page != readingPageAnnounced_) {
            readingPageAnnounced_ = page;
            emit readingPageChanged();
        }
    }
    bps::live::LiveOutputController::Instance().SetFlowChar(
        lineSourceTextOnly_ ? readingFlowCharForScroll(lines) : readingCharForScroll(lines));
    const QString &sentFlow = readingFlowText_;
    if (live_ && scrollMode_ && !sentFlow.isEmpty() && readingEngineFlowText_ != sentFlow) {
        const QVariantList current = onAirSlidesRaw();
        if (!current.isEmpty()) {
            QVariantMap shown = current.first().toMap();
            attachReadingFlow(shown, lines);
            updateReadingPick(onAirTitle_, shown);
            readingEngineFlowText_ = sentFlow;
        }
    }
    if (windowChanged) {
        if (live_) {
            QVariantList current = onAirSlidesRaw();
            if (!current.isEmpty()) {
                QVariantMap shown = current.first().toMap();
                attachReadingFlow(shown, lines);
                updateReadingPick(onAirTitle_, shown);
            }
        } else if (!stagedSlidesRaw_.isEmpty()) {
            QVariantMap shown = stagedSlidesRaw_.first().toMap();
            attachReadingFlow(shown, lines);
            stagedSlidesRaw_ = QVariantList{shown};
            stagedSlide_ = readingStagedPreview(shown);
            BumpStaged();
        }
    }
    // A glide calls this 60 times a second, and every surface answers by rebuilding its
    // slice and its line table on THIS thread (DesignPreview.onReadingScrollChanged). The
    // engine gets every position regardless (SetFlowChar above); the surfaces get a new one
    // only as fast as they can lay one out — the budget IS the last tell's price, so a
    // 300 ms layout buys a 600 ms gap instead of a queue that never drains. The glide's own
    // last frame is forced (see the animation's finished handler), so the picture still
    // ends on the line the walk says it is on.
    const bool gliding = readingScrollAnim_
        && readingScrollAnim_->state() == QAbstractAnimation::Running;
    // The gap follows the SMOOTHED price of a tell (a moving average), not the last one: a single slow tell used to
    // double the gap on the spot, and every monitor tile then froze for that long in the middle of a glide (the
    // "sometimes it jitters" next to the engine output, which never waits). A layout that is slow EVERY time still
    // earns its long gap — the average climbs to it within a few tells — so the protection is kept.
    const qint64 budget = qMax<qint64>(33, static_cast<qint64>(readingScrollNotifyAvgMs_ * 2.0));
    const bool moved = !qFuzzyCompare(readingScroll_ + 1.0, readingScrollNotified_ + 1.0);
    if (notify && (moved || windowChanged)
            && (!gliding || !readingScrollNotifyClock_.isValid()
                || readingScrollNotifyClock_.hasExpired(budget))) {
        QElapsedTimer cost;
        cost.start();
        emit readingScrollChanged();
        readingScrollNotified_ = readingScroll_;
        readingScrollNotifyCostMs_ = qBound<qint64>(qint64(0), cost.elapsed(), qint64(400));
        readingScrollNotifyAvgMs_ = readingScrollNotifyAvgMs_ * 0.7 + static_cast<double>(readingScrollNotifyCostMs_) * 0.3;
        readingScrollNotifyClock_.restart();
        if (readingScrollNotifyCostMs_ >= 30)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("StepCost"),
                                QStringLiteral("notify readingScrollChanged=%1ms gap=%2ms scroll=%3")
                                    .arg(readingScrollNotifyCostMs_).arg(budget)
                                    .arg(readingScroll_, 0, 'f', 2));
    }
}

void LiveOutputService::AnnounceReadingScroll()
{
    readingScrollNotified_ = readingScroll_;
    emit readingScrollChanged();
}

bool LiveOutputService::updateReadingPick(const QString &name, const QVariantMap &shown)
{
    auto &controller = bps::live::LiveOutputController::Instance();
    QElapsedTimer updateClock;
    updateClock.start();
    const auto converted = ShowConverter::slideFromVariant(shown);
    const qint64 convertMs = updateClock.elapsed();
    const auto taken = controller.UpdateReadingSlide(name.toStdString(), converted);
    const qint64 handMs = updateClock.elapsed() - convertMs;
    if (taken.ok()) {
        if (updateClock.elapsed() >= 30)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                                QStringLiteral("updateReadingPick convert=%1ms handOff=%2ms flowChars=%3")
                                    .arg(convertMs).arg(handMs).arg(shown.value(QStringLiteral("flowText")).toString().size()));
        return true;
    }
    QElapsedTimer fallbackClock;
    fallbackClock.start();
    goLiveWithSlides(name, QVariantList{shown});
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                        QStringLiteral("updateReadingPick REFUSED -> goLiveWithSlides %1ms (convert=%2ms)")
                            .arg(fallbackClock.elapsed()).arg(convertMs));
    return false;
}

qreal LiveOutputService::readingCharForScroll(qreal lines) const
{
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    const QVector<int> &starts = readingRunCharStarts(ref);
    // The TRUE character the reference line has reached (not "line start + a fraction"):
    // the one position every surface, the engine's included, turns into its own line
    // with bps::rendering::flowscroll::LineForChar. No table: the number is lines.
    return bps::rendering::flowscroll::CharForLine(starts, lines);
}

void LiveOutputService::commitPendingRebase()
{
    if (readingRebaseSlide_ < 0)
        return;   // nothing owed: the words are already where they belong
    const int slide = readingRebaseSlide_;
    readingRebaseSlide_ = -1;
    shiftPickBase(lineSlideIndex_, slide);   // the page moved: its origin moves too
    lineSlideIndex_ = slide;
    lineOffset_ = 0;
    applyReadingRestage();                       // commits the page inside the run
    applyReadingScroll(readingScroll_);          // ...and tells the engine the same
}

void LiveOutputService::animateReadingScroll(qreal from, qreal to)
{
    if (!readingScrollAnim_) {
        readingScrollAnim_ = new QVariantAnimation(this);
        connect(readingScrollAnim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) { applyReadingScroll(v.toReal()); });
        connect(readingScrollAnim_, &QVariantAnimation::finished, this,
                [this]() {
                    commitPendingRebase();
                    readingWindowRefreshPending_ = false;
                    // The landing line, told whatever the budget decided mid-glide.
                    applyReadingScroll(readingScroll_, true);
                    drainPendingReadingSteps();
                });
    }
    const QVariant speed = SettingsService::instance().value(QStringLiteral("reading.stepMs"));
    const int ms = qBound(60, speed.isValid() ? speed.toInt() : 190, 600);
    const QString curve = SettingsService::instance()
                              .value(QStringLiteral("reading.stepEasing"))
                              .toString();
    readingScrollAnim_->stop();
    readingScrollPreparing_ = true;
    applyReadingScroll(from, false);   // starts where the picture already is: nothing to tell
    readingScrollPreparing_ = false;
    {
        const QSignalBlocker quiet(readingScrollAnim_);
        readingScrollAnim_->setStartValue(from);
        readingScrollAnim_->setEndValue(to);
        readingScrollAnim_->setDuration(ms);
        readingScrollAnim_->setEasingCurve(curve == QLatin1String("even") ? QEasingCurve::Linear
                                            : curve == QLatin1String("settle") ? QEasingCurve::OutCubic
                                                                               : QEasingCurve::InOutCubic);
    }
    readingScrollAnim_->start();
}

void LiveOutputService::drainPendingReadingSteps()
{
    if (pendingReadingSteps_ == 0 || !lineSourceTextOnly_ || !scrollMode_)
        return;
    if (readingScrollAnim_ && readingScrollAnim_->state() == QAbstractAnimation::Running)
        return;

    const int direction = pendingReadingSteps_ > 0 ? 1 : -1;
    pendingReadingSteps_ -= direction;
    if (!lineStep(direction))
        pendingReadingSteps_ = 0;
}

void LiveOutputService::applyReadingRestage(bool commitPage, bool forceRebuild,
                                            bool forceScrollNotify)
{
    if (lineSetRaw_.isEmpty() || lineSlideIndex_ < 0 || lineSlideIndex_ >= lineSetRaw_.size())
        return;
    QElapsedTimer restageClock;
    restageClock.start();
    const int passageBefore = readingPassageChar();
    RebuildReadingRunUnderCommit(forceRebuild);
    const QString previousFlow = readingFlowText_;
    const qreal previousScroll = readingScroll_;
    const qint64 tRebuilt = restageClock.elapsed();
    DeriveReadingScrollFromRun();
    const qint64 tDerived = restageClock.elapsed();
    const bool flowChanged = readingFlowText_ != previousFlow;
    LogReadingContinuity(flowChanged, passageBefore, previousScroll);
    const bool scrollChanged = !readingStepGlide_
        && (forceScrollNotify
            || !qFuzzyCompare(previousScroll + 1.0, readingScroll_ + 1.0));
    LogReadingRestage(commitPage, flowChanged, scrollChanged);

    const qint64 tLogged = restageClock.elapsed();
    schedulePrewarmReadingLines();
    const int page = readingPage();
    const bool pageChanged = (readingPageAnnounced_ != page);
    if (!readingStepGlide_)
        readingPageAnnounced_ = page;

    if (lineSourceTextOnly_) {
        const qint64 tPre = restageClock.elapsed();
        PublishReadingFlowOnly(flowChanged, scrollChanged, pageChanged);
        if (restageClock.elapsed() >= 40)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                                QStringLiteral("restage total=%1ms rebuild=%2 derive=%3 log+scroll=%4 prewarm+page=%5 publish=%6")
                                    .arg(restageClock.elapsed()).arg(tRebuilt).arg(tDerived - tRebuilt)
                                    .arg(tLogged - tDerived).arg(tPre - tLogged).arg(restageClock.elapsed() - tPre));
        return;
    }
    if (live_ && !commitPage) {
        if (!readingStepGlide_ && (flowChanged || scrollChanged))
            AnnounceReadingScroll();
        return;
    }
    if (live_)
        CommitReadingPageOnAir(flowChanged, scrollChanged, pageChanged);
    else
        StageReadingPageOffAir(flowChanged, scrollChanged, pageChanged);
}

void LiveOutputService::RebuildReadingRunUnderCommit(bool forceRebuild)
{
    rebuildReadingRun(lineSlideIndex_, forceRebuild);
    readingRunBaseLine_ = readingBaseForSlide(lineSlideIndex_);
}

void LiveOutputService::DeriveReadingScrollFromRun()
{
    readingScroll_ = readingRunBaseLine_ + lineOffset_;
    if (!lineSourceTextOnly_) {
        readingFlowText_ = readingRunText_;
        return;
    }
    const int passageChar = readingTextWindowCharStart_
        + static_cast<int>(std::floor(readingCharForScroll(readingScroll_)));
    if (!refreshReadingTextWindow(passageChar))
        return;
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
    readingRunPageLines_.insert(0, starts.size());
    readingScroll_ = readingLineForStyle(ref, passageChar);
    lineOffset_ = readingScroll_;
    if (!starts.isEmpty())
        readingLayoutTargetChar_ = -1;
}

void LiveOutputService::LogReadingContinuity(bool flowChanged, int passageBefore, qreal previousScroll)
{
    if (!flowChanged || passageBefore < 0)
        return;
    const int passageAfter = readingPassageChar();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadContinuity"),
                        QStringLiteral("slide=%1 passageBefore=%2 passageAfter=%3 delta=%4 "
                                       "linesBefore=%5 linesAfter=%6 (delta != 0: the text moved)")
                            .arg(lineSlideIndex_)
                            .arg(passageBefore).arg(passageAfter)
                            .arg(passageAfter - passageBefore)
                            .arg(previousScroll, 0, 'f', 2)
                            .arg(readingScroll_, 0, 'f', 2));
}

void LiveOutputService::LogReadingRestage(bool commitPage, bool flowChanged, bool scrollChanged)
{
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRestage"),
                        QStringLiteral("slide=%1 base=%2 lineOffset=%3 scroll=%4 "
                                       "flowChanged=%5 commitPage=%6 live=%7 "
                                       "scrollChanged=%8 (true = the new position was announced)")
                            .arg(lineSlideIndex_)
                            .arg(static_cast<double>(readingRunBaseLine_), 0, 'f', 2)
                            .arg(static_cast<double>(lineOffset_), 0, 'f', 2)
                            .arg(readingScroll_, 0, 'f', 2)
                            .arg(flowChanged ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(commitPage ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(live_ ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(scrollChanged ? QStringLiteral("yes") : QStringLiteral("no")));
}

void LiveOutputService::PublishReadingFlowOnly(bool flowChanged, bool scrollChanged, bool pageChanged)
{
    // What the screens are to carry: the passage.
    const QString &display = readingFlowText_;
    if (live_) {
        if (holdingPassagePick_ && !heldPickSlide_.isEmpty()) {
            // The pick's one commit: the held slide, carrying the passage it scrolls.
            QVariantMap shown = heldPickSlide_;
            QElapsedTimer publishClock;
            publishClock.start();
            attachReadingFlow(shown, readingScroll_);
            const qint64 attachMs = publishClock.elapsed();
            updateReadingPick(lineSetName_, shown);
            if (publishClock.elapsed() >= 40)
                EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                                    QStringLiteral("publish attachReadingFlow=%1ms updateReadingPick=%2ms")
                                        .arg(attachMs).arg(publishClock.elapsed() - attachMs));
            readingEngineFlowText_ = display;   // the engine holds this passage now
            heldPickSent_ = true;
        } else if (flowChanged || (!display.isEmpty() && display != readingEngineFlowText_)) {
            QVariantList current = onAirSlidesRaw();
            if (!current.isEmpty()) {
                QVariantMap shown = current.first().toMap();
                attachReadingFlow(shown, readingScroll_);
                updateReadingPick(onAirTitle_, shown);
                readingEngineFlowText_ = display;
            }
        }
    } else if (!stagedSlidesRaw_.isEmpty()
               // "changed" is measured against the previous passage, which a fresh text
               // source has just replaced; what matters is whether the STAGED SLIDE
               // already carries this passage — GO LIVE sends the slide as it stands.
               && (flowChanged
                   || (scrollMode_ && !display.isEmpty()
                       && stagedSlidesRaw_.first().toMap().value(QStringLiteral("flowText")).toString()
                          != display))) {
        QElapsedTimer stagedClock;
        stagedClock.start();
        QVariantMap shown = stagedSlidesRaw_.first().toMap();
        attachReadingFlow(shown, readingScroll_);
        const qint64 attachMs = stagedClock.elapsed();
        stagedSlidesRaw_ = QVariantList{shown};
        stagedSlide_ = readingStagedPreview(shown);
        const qint64 previewMs = stagedClock.elapsed() - attachMs;
        BumpStaged();
        if (stagedClock.elapsed() >= 30)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("PickCostSvc"),
                                QStringLiteral("staged publish attach=%1ms readingStagedPreview=%2ms stagedChanged=%3ms")
                                    .arg(attachMs).arg(previewMs).arg(stagedClock.elapsed() - attachMs - previewMs));
    }
    if (pageChanged && !readingStepGlide_)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        AnnounceReadingScroll();
}

void LiveOutputService::CommitReadingPageOnAir(bool flowChanged, bool scrollChanged, bool pageChanged)
{
    QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
    attachReadingFlow(shown, readingScroll_);
    if (holdingPassagePick_)
        heldPickSlide_ = shown;   // stagePassagePick sends it once, with the passage
    else
        (void)updateReadingPick(lineSetName_, shown);
    stagedName_.clear();
    stagedSlidesRaw_.clear();
    stagedSlide_ = QVariantMap{};
    if (quietStage_)
        return;
    BumpStaged();
    if (pageChanged)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        AnnounceReadingScroll();
}

void LiveOutputService::StageReadingPageOffAir(bool flowChanged, bool scrollChanged, bool pageChanged)
{
    QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
    if (lineSourceTextOnly_)
        attachReadingFlow(shown, readingScroll_);
    else
        setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                            readingCharForScroll(readingScroll_));
    stagedName_ = lineSetName_;
    stagedSlidesRaw_ = QVariantList{shown};
    if (quietStage_)
        return;   // (the final state composes the preview and tells everyone, once)
    stagedSlide_ = readingStagedPreview(shown);
    BumpStaged();
    if (pageChanged)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        AnnounceReadingScroll();
}

bool LiveOutputService::lineStep(int direction)
{
    // See StepCost.hpp: the same probe stepPassage carries, so a press on either path is
    // one comparable line (`path=scroll` against `path=slide`).
    services::diagnostics::StepCost stepCost("scroll", direction);
    if (direction != 0)
        emit lineStepPending(direction);
    LogLineStepRequest(direction);

    if (lineSetRaw_.isEmpty() || direction == 0)
        return false;

    if (scrollMode_)
        activatePendingTextSource();

    commitPendingRebase();

    // A run derived before its column was solved holds no line table, and the solve only
    // re-asks the surfaces — so every press would answer "owed" for ever and the walk could
    // never move (measured: 121 presses on a real passage, none of them moved). The seat
    // repairs it on its own now; this is the belt for any other ordering.
    RepairReadingRunWithoutLines();

    const int setSize = static_cast<int>(lineSetRaw_.size());
    const int ownLines = LineStepOwnLines();
    const int owed = OwedLineStepAnswer(direction, ownLines);
    if (owed >= 0)
        return owed == 1;

    const qreal absoluteFrom = readingAbsoluteLine();
    pl::LineWalk walk;
    walk.begin(lineSlideIndex_, lineOffset_, static_cast<int>(readingScroll_));
    const int previousLines = (direction < 0 && lineSlideIndex_ > 0)
        ? readingPageLineCount(lineSlideIndex_ - 1) : 0;
    pl::LineWalk::Step step = walk.step(direction, ownLines, setSize, previousLines);
    if (!step.moved)
        return false;
    // CURSOR FREQUENCY (Reading options): one press may move several lines. The extra lines are only taken while the
    // walk stays on this page — a page turn has its own handling (the next page's window, the rebase) and is always
    // taken on its own, so a press that reaches the end of a page stops there and the next press turns it.
    const int perPress = qBound(1, SettingsService::instance().value(QStringLiteral("reading.stepLines")).toInt(), 20);
    for (int extra = 1; extra < perPress && !step.crossedPage; ++extra) {
        pl::LineWalk probe = walk;
        const pl::LineWalk::Step next = probe.step(direction, ownLines, setSize, previousLines);
        if (!next.moved || next.crossedPage)
            break;
        walk = probe;
        step = next;
    }
    if (!readingBudgetFitActive_) {
        readingBudgetFitActive_ = true;
        emit readingStageChanged();
    }

    // EVERY SCREEN KEEPS ITS OWN WRAP AND MOVES BY WHOLE LINES OF IT. A screen of another width cannot begin a
    // line on the word the walk goes to without re-wrapping (the old anchoring did: every press re-flowed the
    // wider screens and left short cut lines scrolling past). So the tables never change during the walk; the
    // step only says WHICH TWO CHARACTERS the glide runs between, and each screen glides between the whole
    // lines those characters fall on (SnappedLineForChar) — the engine is handed the same pair.
    if (lineSourceTextOnly_ && !step.crossedPage) {
        readingSpanFrom_ = std::floor(readingCharForScroll(readingScroll_));
        readingSpanTo_ = std::floor(readingCharForScroll(step.absolute));
        bps::live::LiveOutputController::Instance().SetFlowSpan(
            static_cast<float>(readingSpanFrom_), static_cast<float>(readingSpanTo_));
        if (live_)
            scheduleReadingRepublish();
    }
    if (!step.crossedPage)
        MoveLineStepWithinPage(step.offset, step.absolute, absoluteFrom);
    else if (direction > 0)
        MoveLineStepToNextPage(step.page, ownLines, step.absolute, absoluteFrom);
    else
        MoveLineStepBackPage(step.page, step.offset, absoluteFrom);

    lineStepDirection_ = direction > 0 ? 1 : -1;
    ++lineStepSerial_;
    emit lineStepChanged();
    return true;
}

void LiveOutputService::LogLineStepRequest(int direction)
{
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("LineStep"),
                        QStringLiteral("dir=%1 set=%2 slide=%3 offset=%4 lines=%5")
                            .arg(direction)
                            .arg(lineSetRaw_.size())
                            .arg(lineSlideIndex_)
                            .arg(lineOffset_)
                            .arg(lineSetRaw_.isEmpty()
                                     ? 0
                                     : readingPageLineCount(lineSlideIndex_)));
}

int LiveOutputService::LineStepOwnLines() const
{
    const int ownLines = readingPageLineCount(lineSlideIndex_);
    if (ownLines > 0 || !lineSourceTextOnly_)
        return ownLines;
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    return static_cast<int>(styleRunLinesForText(ref, readingRunText_).lines.size());
}

int LiveOutputService::OwedLineStepAnswer(int direction, int ownLines)
{
    if (!lineSourceTextOnly_ || !readingBudgetFitActive_ || ownLines > 0)
        return -1;
    if (direction < 0 && lineSlideIndex_ == 0 && readingAbsoluteLine() <= 0.0)
        return 0;
    if (pendingReadingSteps_ != 0 && ((pendingReadingSteps_ > 0) != (direction > 0)))
        pendingReadingSteps_ = 0;
    pendingReadingSteps_ = qBound(-24, pendingReadingSteps_ + direction, 24);
    return 1;
}

void LiveOutputService::RepairReadingRunWithoutLines()
{
    if (!lineSourceTextOnly_ || readingRunText_.isEmpty())
        return;                       // Slide-mode text and a run with no words have no table
    if (LineStepOwnLines() > 0)
        return;                       // the table is there: a healthy run costs one cached lookup
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    if (styleRunLinesForText(ref, readingRunText_).lines.isEmpty())
        return;                       // still being solved: there is no table to derive yet
    // FORCED: the repair runs with an unchanged window, and rebuildReadingRun short-circuits
    // on that — the plain restage is what leaves the empty table in place.
    applyReadingRestage(false, /*forceRebuild=*/ true);
}

void LiveOutputService::MoveLineStepWithinPage(int offset, qreal absolute, qreal absoluteFrom)
{
    // A step inside the page moves ONE number. The passage, its line table, the pick and the
    // staged picture are all the same picture — the restage that used to run here re-derived
    // every one of them per press. The window still follows: applyReadingScroll moves it when
    // the walk nears an edge.
    lineOffset_ = offset;
    readingScroll_ = absolute;
    if (!lineSourceTextOnly_) {
        readingStepGlide_ = true;
        applyReadingRestage(false);
        readingStepGlide_ = false;
    }
    animateReadingScroll(readingWindowOffset(absoluteFrom), readingScroll_);
}

void LiveOutputService::MoveLineStepToNextPage(int page, int ownLines, qreal absolute,
                                              qreal absoluteFrom)
{
    lineOffset_ = ownLines;
    readingRebaseSlide_ = page;
    animateReadingScroll(readingWindowOffset(absoluteFrom), absolute);
}

void LiveOutputService::MoveLineStepBackPage(int page, int offset, qreal absoluteFrom)
{
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    shiftPickBase(lineSlideIndex_, page);
    lineSlideIndex_ = page;
    lineOffset_ = offset;
    readingStepGlide_ = true;
    applyReadingRestage();
    readingStepGlide_ = false;
    animateReadingScroll(readingWindowOffset(absoluteFrom), readingScroll_);
}
