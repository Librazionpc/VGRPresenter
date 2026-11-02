#include "services/CallTrace.h"
#include "services/LiveOutputService.h"

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
    double lo = 3.0, hi = 400.0;
    for (int i = 0; i < 11; ++i) {          // the preview's own eleven steps
        const double mid = (lo + hi) / 2.0;
        const QFont font = blockFontAt(block, mid);
        const QFontMetricsF metrics(font);
        const double natural = metrics.height();
        const double step = bps::presentation::template_text_metrics::linePitch(
            natural, factor, gap);
        const QVector<VisualSpan> spans = layoutSpansAt(text, block, mid);
        double widest = 0.0;
        // A line's INK, not its trailing space: the break leaves the space on the line it
        // ends, and counting it rejected sizes whose words fit (a 754px box refused a line
        // 760px wide that was 8px of space), so the solved size came out far too small and
        // the box showed far more than the max words.
        for (const VisualSpan &span : spans)
            widest = qMax(widest, metrics.horizontalAdvance(
                                       text.mid(span.start, span.end - span.start).trimmed()));
        if (widest <= width + 0.01 && spans.size() * step <= height + 0.01)
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
    emit readingScrollChanged();
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
        emit stagedChanged();             // not a stale staged copy over it
        announceReadingPage();           // ...and only now is the page announced
        return;
    }
    stagedName_ = name;
    stagedSlidesRaw_ = pick;
    stagedSlide_ = pick.isEmpty() ? QVariantMap{}
                 : (!styledPreview.isEmpty() ? styledPreview
                                             : previewWithActiveStyle(pick.first().toMap()));
    emit stagedChanged();
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

void LiveOutputService::republishReadingFlow()
{
    if (!live_ || !scrollMode_ || readingFlowText_.isEmpty())
        return;
    const QVariantList current = onAirSlidesRaw();
    if (current.isEmpty())
        return;
    QVariantMap shown = current.first().toMap();
    setSlideReadingFlow(shown, /*enabled=*/ true, readingFlowText_,
                        readingFlowCharForScroll(readingScroll_));
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
    if (!live_ || !scrollMode_ || pages.isEmpty() || slides.isEmpty()) {
        stageSlideSet(name, slides, index);
        setReadingTextSource(name, pages, pageIndex);
        return;
    }
    holdingPassagePick_ = true;
    heldPickSent_ = false;
    heldPickSlide_.clear();
    stageSlideSet(name, slides, index);        // state only: the commit is held back
    setReadingTextSource(name, pages, pageIndex);   // publishes the held slide + passage once
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
    lineSlideIndex_ = 0;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
    readingPickBase_ = 0;
    lineOffset_ = 0;
    readingLinesCache_.clear();
    readingRunLinesCache_.clear();
    readingRunLinesLru_.clear();
    ResetReadingPick();
    readingTextWindowCharStart_ = 0;
    readingFlowText_.clear();
    refreshReadingTextWindow(selectedChar);
    rebuildReadingRun(lineSlideIndex_, true);
    const auto *model = OutputListModel::instance();
    const QString referenceStyle = model ? model->activeStyleId() : QString();
    const QVector<int> starts = styleRunLinesForText(referenceStyle, readingRunText_).charStarts;
    const int localChar = qMax(0, selectedChar - readingTextWindowCharStart_);
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

    const int desiredStart = qBound(0, passageChar, qMax(0, size - kWindowChars));
    int start = desiredStart;
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
        if (column == scrollColumns_.constEnd() || column->size <= 0.0)
            return none;
        const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                       + QString::number(qHash(text), 16);
        if (const StyleRunLines *hit = CachedStyleRunLines(columnCacheKey))
            return *hit;
        QueueScrollColumnMeasure(columnKey, columnCacheKey, text,
                                 column->size, column->block, column->sig);
        return none;
    }
    if (readingBudgetFitActive_) {
        const QString columnKey = ScrollColumnKey(styleId);
        const auto column = scrollColumns_.constFind(columnKey);
        if (column != scrollColumns_.constEnd() && column->size > 0.0) {
            const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                           + QString::number(qHash(text), 16);
            if (const StyleRunLines *hit = CachedStyleRunLines(columnCacheKey))
                return *hit;
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
                    if (currentColumn == self->scrollColumns_.constEnd()
                            || currentColumn->sig != columnSig)
                        return;
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
                        QTimer::singleShot(0, self, &LiveOutputService::drainPendingReadingSteps);
                        // The table the engine's outputs draw is ready: hand it over.
                        QTimer::singleShot(0, self, []() {
                            if (auto *outputs = OutputListModel::instance())
                                outputs->repushEngineStyles();
                        });
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

    QVariantMap shape;
    shape.insert(QStringLiteral("w"), block.value(QStringLiteral("width")));
    shape.insert(QStringLiteral("h"), block.value(QStringLiteral("height")));
    shape.insert(QStringLiteral("pad"), blockPadding(block));
    shape.insert(QStringLiteral("meta"), block.value(QStringLiteral("meta")));
    const QString sig = lineSetName_ + QLatin1Char('#') + QString::number(budget) + QLatin1Char('#')
        + QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(shape))
                                .toJson(QJsonDocument::Compact));

    qreal size = scrollSizeBySig_.value(sig, 0.0);
    if (size <= 0.0) {
        QStringList words;
        for (int i = 0, n = flow.size(); i < n && words.size() < budget; ) {
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
        grown.insert(QStringLiteral("meta"), meta);
        size = fittedPixelSize(probe.trimmed(), grown);
        if (scrollSizeBySig_.size() >= 64)
            scrollSizeBySig_.clear();
        scrollSizeBySig_.insert(sig, size);
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScrollColumn"),
                            QStringLiteral("style=%1 pick='%2' budget=%3 size=%4 box=%5x%6 pad=%7 "
                                           "(solved once: the max-words budget into the text area)")
                                .arg(rawStyleId.isEmpty() ? QStringLiteral("(ref)") : rawStyleId.left(8))
                                .arg(lineSetName_.left(24))
                                .arg(budget)
                                .arg(size, 0, 'f', 2)
                                .arg(block.value(QStringLiteral("width")).toDouble())
                                .arg(block.value(QStringLiteral("height")).toDouble())
                                .arg(blockPadding(block)));
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
        QTimer::singleShot(0, this, []() {
            if (auto *outputs = OutputListModel::instance())
                outputs->repushEngineStyles();
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

bool LiveOutputService::readingLineTableFor(const QString &styleId, std::vector<int> &starts,
                                            uint64_t &textHash) const
{
    starts.clear();
    textHash = 0;
    if (!scrollMode_ || !readingBudgetFitActive_ || readingFlowText_.isEmpty() || styleId.isEmpty())
        return false;
    const QVector<int> table = styleRunLines(styleId).charStarts;   // none while still measuring
    if (table.isEmpty())
        return false;
    starts.assign(table.constBegin(), table.constEnd());
    textHash = bps::rendering::TextLayout::TextHash(readingFlowText_.toUtf8().toStdString());
    return true;
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

qreal LiveOutputService::readingLineForStyle(const QString &styleId, qreal charAt) const
{
    const QVector<int> starts = styleRunLines(styleId).charStarts;
    if (starts.isEmpty())
        return lineSourceTextOnly_ ? readingScroll_ : charAt;
    if (charAt < 0.0)
        return charAt;
    if (lineSourceTextOnly_)
        charAt = qMax(0.0, charAt - readingTextWindowCharStart_);
    // charAt is already the true character (readingCharForScroll); this style's line is
    // read off ITS table by the function the engine's renderers use too.
    return bps::rendering::flowscroll::LineForChar(starts, charAt);
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
    }
}

void LiveOutputService::applyReadingScroll(qreal lines)
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
    if (live_ && scrollMode_ && !readingFlowText_.isEmpty()
            && readingEngineFlowText_ != readingFlowText_) {
        const QVariantList current = onAirSlidesRaw();
        if (!current.isEmpty()) {
            QVariantMap shown = current.first().toMap();
            setSlideReadingFlow(shown, /*enabled=*/ true, readingFlowText_,
                                readingFlowCharForScroll(lines));
            updateReadingPick(onAirTitle_, shown);
            readingEngineFlowText_ = readingFlowText_;
        }
    }
    if (windowChanged) {
        if (live_) {
            QVariantList current = onAirSlidesRaw();
            if (!current.isEmpty()) {
                QVariantMap shown = current.first().toMap();
                setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                                    readingFlowCharForScroll(lines));
                updateReadingPick(onAirTitle_, shown);
            }
        } else if (!stagedSlidesRaw_.isEmpty()) {
            QVariantMap shown = stagedSlidesRaw_.first().toMap();
            setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                                readingFlowCharForScroll(lines));                stagedSlidesRaw_ = QVariantList{shown};
            stagedSlide_ = readingStagedPreview(shown);
            emit stagedChanged();
        }
    }
    emit readingScrollChanged();
}

bool LiveOutputService::updateReadingPick(const QString &name, const QVariantMap &shown)
{
    auto &controller = bps::live::LiveOutputController::Instance();
    const auto taken = controller.UpdateReadingSlide(
        name.toStdString(), ShowConverter::slideFromVariant(shown));
    if (taken.ok())
        return true;
    goLiveWithSlides(name, QVariantList{shown});
    return false;
}

qreal LiveOutputService::readingCharForScroll(qreal lines) const
{
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    const QVector<int> starts = readingRunCharStarts(ref);
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
                    if (readingWindowRefreshPending_) {
                        readingWindowRefreshPending_ = false;
                        applyReadingScroll(readingScroll_);
                    }
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
    applyReadingScroll(from);   // starts where the picture already is
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
    const int passageBefore = readingPassageChar();
    RebuildReadingRunUnderCommit(forceRebuild);
    const QString previousFlow = readingFlowText_;
    const qreal previousScroll = readingScroll_;
    DeriveReadingScrollFromRun();
    const bool flowChanged = readingFlowText_ != previousFlow;
    LogReadingContinuity(flowChanged, passageBefore, previousScroll);
    const bool scrollChanged = !readingStepGlide_
        && (forceScrollNotify
            || !qFuzzyCompare(previousScroll + 1.0, readingScroll_ + 1.0));
    LogReadingRestage(commitPage, flowChanged, scrollChanged);

    schedulePrewarmReadingLines();
    const int page = readingPage();
    const bool pageChanged = (readingPageAnnounced_ != page);
    if (!readingStepGlide_)
        readingPageAnnounced_ = page;

    if (lineSourceTextOnly_) {
        PublishReadingFlowOnly(flowChanged, scrollChanged, pageChanged);
        return;
    }
    if (live_ && !commitPage) {
        if (!readingStepGlide_ && (flowChanged || scrollChanged))
            emit readingScrollChanged();
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
    if (live_) {
        if (holdingPassagePick_ && !heldPickSlide_.isEmpty()) {
            // The pick's one commit: the held slide, carrying the passage it scrolls.
            QVariantMap shown = heldPickSlide_;
            setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                                readingFlowCharForScroll(readingScroll_));
            updateReadingPick(lineSetName_, shown);
            readingEngineFlowText_ = readingFlowText_;   // the engine holds this passage now
            heldPickSent_ = true;
        } else if (flowChanged) {
            QVariantList current = onAirSlidesRaw();
            if (!current.isEmpty()) {
                QVariantMap shown = current.first().toMap();
                setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                                    readingFlowCharForScroll(readingScroll_));
                updateReadingPick(onAirTitle_, shown);
            }
        }
    } else if (!stagedSlidesRaw_.isEmpty()
               // "changed" is measured against the previous passage, which a fresh text
               // source has just replaced; what matters is whether the STAGED SLIDE
               // already carries this passage — GO LIVE sends the slide as it stands.
               && (flowChanged
                   || (scrollMode_ && !readingFlowText_.isEmpty()
                       && stagedSlidesRaw_.first().toMap().value(QStringLiteral("flowText")).toString()
                          != readingFlowText_))) {
        QVariantMap shown = stagedSlidesRaw_.first().toMap();
        setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                            readingFlowCharForScroll(readingScroll_));
        stagedSlidesRaw_ = QVariantList{shown};
        stagedSlide_ = readingStagedPreview(shown);
        emit stagedChanged();
    }
    if (pageChanged && !readingStepGlide_)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        emit readingScrollChanged();
}

void LiveOutputService::CommitReadingPageOnAir(bool flowChanged, bool scrollChanged, bool pageChanged)
{
    QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
    setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                        readingFlowCharForScroll(readingScroll_));
    if (holdingPassagePick_)
        heldPickSlide_ = shown;   // stagePassagePick sends it once, with the passage
    else
        (void)updateReadingPick(lineSetName_, shown);
    stagedName_.clear();
    stagedSlidesRaw_.clear();
    stagedSlide_ = QVariantMap{};
    emit stagedChanged();
    if (pageChanged)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        emit readingScrollChanged();
}

void LiveOutputService::StageReadingPageOffAir(bool flowChanged, bool scrollChanged, bool pageChanged)
{
    QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
    setSlideReadingFlow(shown, scrollMode_, readingFlowText_,
                        lineSourceTextOnly_ ? readingFlowCharForScroll(readingScroll_)
                                            : readingCharForScroll(readingScroll_));
    stagedName_ = lineSetName_;
    stagedSlidesRaw_ = QVariantList{shown};
    stagedSlide_ = readingStagedPreview(shown);
    emit stagedChanged();
    if (pageChanged)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        emit readingScrollChanged();
}bool LiveOutputService::lineStep(int direction)
{
    if (direction != 0)
        emit lineStepPending(direction);
    LogLineStepRequest(direction);

    if (lineSetRaw_.isEmpty() || direction == 0)
        return false;

    if (scrollMode_)
        activatePendingTextSource();

    commitPendingRebase();

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
    const pl::LineWalk::Step step = walk.step(direction, ownLines, setSize, previousLines);
    if (!step.moved)
        return false;
    if (!readingBudgetFitActive_) {
        readingBudgetFitActive_ = true;
        emit readingStageChanged();
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

void LiveOutputService::MoveLineStepWithinPage(int offset, qreal absolute, qreal absoluteFrom)
{
    lineOffset_ = offset;
    readingScroll_ = absolute;
    readingStepGlide_ = true;
    applyReadingRestage(false);
    readingStepGlide_ = false;
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
