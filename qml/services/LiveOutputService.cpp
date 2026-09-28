#include "services/LiveOutputService.h"

#include "services/ShowConverter.h"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include "services/EngineBridge.h"

#include <QCoreApplication>
#include <QTimer>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {

// The slide's CONTENT text — what a bound "text" block shows. Mirrors
// SceneBuilder's SlideContentText: slide.text first; when empty (content
// tabs go live with the verse riding in their OWN template's blocks and
// only the reference in `title`), the first text-bearing slide block's
// text; else the title.
std::string slideContentText(const pl::Slide *slide)
{
    if (!slide->text.empty())
        return slide->text;
    for (const pl::ContentBlock &b : slide->blocks)
        if (b.kind == "text" && !b.text.empty() && b.text.front() != '{')
            return b.text;
    return slide->title;
}

// The slide's content family ("scripture" | "table" | …), as tagged by
// ShowConverter in the slide's meta — mirrors SceneBuilder's
// slideContentType. Empty = untagged (the style's template then applies).
std::string slideContentType(const pl::Slide *slide)
{
    if (auto meta = bps::json::Parse(slide->metaJson); meta.ok())
        if (const bps::json::Value *v = meta.value().Find("contentType"); v && v->type() == bps::json::Value::Type::String)
            return std::string(v->asString());
    return {};
}

// One slide as the QML preview shape: { valid, title, blocks, background } —
// the same { blocks, background } maps DesignPreview draws everywhere else
// (ShowConverter::blockToVariant is the single QML<->engine block mapping).
//
// STYLE COMPOSITION, mirrored from SceneBuilder::BuildSlideScene so the
// monitor tile shows what the OUTPUT shows: when the active output's style
// wears an engine template (baked blocks in the spec), the preview composes
// THE STYLE'S template (slide content bound in, unfilled {scripture_*}
// placeholders blanked) INSTEAD of the slide's own blocks, and the style's
// background colour sits UNDER the slide's own (a slide background still
// wins unless it is unset). A template-less style leaves the slide as-is
// and the tile's own colour fallback handles the rest.
QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    if (!slide)
        return {};

    const pl::OutputStyleSpec style = pl::PresentationEngine::Instance().ActiveOutputStyle();
    QString background = QString::fromStdString(slide->background);
    QVariantList blocks;

    // Per-family rule, mirrored from SceneBuilder: the style's template only
    // restyles its own contentType (a scripture-keyed style must not
    // steamroll The Table's tab-template layout) — otherwise the slide's
    // own blocks carry the look.
    const std::string slideType = slideContentType(slide);
    const bool styleTemplateApplies = !style.templateBlocks.empty()
        && (style.contentType.empty() || slideType.empty() || slideType == style.contentType);

    if (styleTemplateApplies) {
        const QString styleBg = QString::fromStdString(style.backgroundColor);
        if (!styleBg.isEmpty() && styleBg != QLatin1String("transparent")
            && (background.isEmpty() || background == QLatin1String("transparent")
                || !style.clearBackgroundOnText))
            background = styleBg;
        for (const pl::ContentBlock &b : style.templateBlocks) {
            pl::ContentBlock bound = b;
            if (b.kind == "text") {
                if (!b.bind.empty()) {
                    std::string value = pl::SlideResolver::BoundValue(*slide, b.bind);
                    if (value.empty()) {
                        if (b.bind == "text")
                            value = slideContentText(slide);
                        else if (b.bind == "ref")
                            value = slide->title;
                    }
                    bound.text = std::move(value);
                } else if (b.text.find('{') != std::string::npos) {
                    bound.text.clear();
                }
            }
            blocks.append(ShowConverter::blockToVariant(bound));
        }
    } else {
        for (const pl::ContentBlock &b : slide->blocks)
            blocks.append(ShowConverter::blockToVariant(b));
    }

    return QVariantMap{
        { QStringLiteral("valid"), true },
        { QStringLiteral("title"), QString::fromStdString(slide->title) },
        { QStringLiteral("blocks"), blocks },
        { QStringLiteral("background"), background },
    };
}

} // namespace

LiveOutputService *LiveOutputService::s_instance = nullptr;

LiveOutputService::LiveOutputService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
}

LiveOutputService &LiveOutputService::instance()
{
    static LiveOutputService inst;
    return inst;
}

LiveOutputService *LiveOutputService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    // CppOwnership: QML must not GC the singleton the engine-side loop
    // signals into (same contract as TelemetryService).
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

// ---------------------------------------------------------------------------
// Go live / stop
// ---------------------------------------------------------------------------
void LiveOutputService::goLive()
{
    auto r = bps::live::LiveOutputController::Instance().StartFromOpenShow();
    if (!r.ok()) {
        // Surface the honest reason (no show open / no slides) as a property,
        // not a silent no-op — the Show screen toasts on it.
        qWarning("LiveOutputService: goLive failed: %s", r.error().message.c_str());
        emit onAirChanged();
        return;
    }

    if (!live_) {
        live_ = true;
        emit liveChanged();
        refreshOnAirSlide();
        emit onAirChanged();
        if (!poll_) {
            poll_ = new QTimer(this);
            poll_->setInterval(100);   // 10Hz — slide titles AND a live-looking preview
            connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
        }
        poll_->start();
        pollTick();
    }
}

// Any-content go-live: convert the QML slide maps to engine Slides and hand
// them to the controller. A failed start surfaces through the same qWarning +
// onAirChanged path as goLive (the UI toasts off the failure reason).
void LiveOutputService::goLiveWithSlides(const QString &name, const QVariantList &slides)
{
    bps::presentation::Presentation content;
    content.name = name.toStdString();
    for (const QVariant &v : slides)
        content.slides.push_back(ShowConverter::slideFromVariant(v.toMap()));
    if (content.slides.empty()) {
        qWarning("LiveOutputService: goLiveWithSlides: no slides in '%s'", name.toUtf8().constData());
        emit onAirChanged();
        return;
    }
    auto r = bps::live::LiveOutputController::Instance().StartFromSlides(name.toStdString(),
                                                                         content.slides);
    if (!r.ok()) {
        qWarning("LiveOutputService: goLiveWithSlides failed: %s", r.error().message.c_str());
        emit onAirChanged();
        return;
    }
    if (!live_) {
        live_ = true;
        emit liveChanged();
    }
    refreshOnAirSlide();
    emit onAirChanged();
    if (!poll_) {
        poll_ = new QTimer(this);
        poll_->setInterval(100);
        connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
    }
    if (!poll_->isActive()) {
        poll_->start();
        pollTick();
    }
}

// ---------------------------------------------------------------------------
// Taken video input — the output preview's camera/screen layer
// ---------------------------------------------------------------------------
// The monitor tiles already composite raster layers (the style's PNG/JPG
// background, the distributed frame) as plain QML Image items over the
// image providers; a taken INPUT rides the same path — its frames come
// from the PAL's videopreview taps (EngineBridge, owner "output"), the
// SAME tap class the Settings dialogs' previews use, so the device
// lifecycle (idempotent starts, owner-counted release) is already proven.
void LiveOutputService::takeInput(const QString &label, const QString &kind, const QString &mode)
{
    if (kind != QLatin1String("camera") && kind != QLatin1String("screen")) {
        // Honest refusal: media/NDI have no local tap yet (media plays
        // through the player graph, NDI through the network receiver —
        // neither is wired to a compositor layer today).
        qWarning("LiveOutputService: input kind '%s' has no output-preview tap yet", kind.toUtf8().constData());
        emit inputChanged();
        return;
    }
    if (label.isEmpty())
        return;

    // Re-taking while taken: release the previous hold first (one layer).
    if (inputLabel_ == label && inputKind_ == kind) {
        // Same source re-picked: just keep the tap alive (mode changes are
        // handled by the caller's new tap below).
    } else if (!inputLabel_.isEmpty()) {
        if (inputKind_ == QLatin1String("screen"))
            EngineBridge::instance().stopScreenPreview(inputLabel_, QStringLiteral("output"));
        else
            EngineBridge::instance().stopVideoPreview(inputLabel_, QStringLiteral("output"));
    }

    inputLabel_ = label;
    inputKind_ = kind;
    bool started = false;
    if (kind == QLatin1String("screen")) {
        // startScreenPreview reports whether the label resolved to a live
        // window/monitor and the tap started — a refused take (window closed,
        // title drifted) unwinds the take here instead of parking a
        // warm-up placeholder that can never fill.
        started = EngineBridge::instance().startScreenPreview(label, QStringLiteral("output"));
    } else {
        EngineBridge::instance().startVideoPreview(label, mode, QStringLiteral("output"));
        started = true;   // camera refusals surface via the provider's empty frames
    }
    if (!started) {
        inputLabel_.clear();
        inputKind_.clear();
        emit inputChanged();
        return;
    }

    if (!inputPump_) {
        inputPump_ = new QTimer(this);
        inputPump_->setInterval(66);   // ~15 fps, matching the tap's production rate
        connect(inputPump_, &QTimer::timeout, this, [this] {
            // FRAME DETECTION — SERVICE-SIDE, off the provider's decode-once
            // cache (the same cache requestImage drains): the previous QML
            // confirmInputFrame() round-trip created a write-in-binding
            // feedback edge (tile's onStatusChanged → inputRev bump → the
            // SAME tile's inputSource URL re-evaluates) and QML flagged it
            // as a binding loop. inputLive now flips purely from the tap's
            // real production; QML only reads.
            if (!inputLive_ && !inputLabel_.isEmpty()) {
                const QImage frame = EngineBridge::instance().previewFrameFor(inputLabel_);
                if (!frame.isNull() && frame.width() > 1) {
                    inputLive_ = true;
                    qInfo("LiveOutputService: first frame decoded for '%s' (%dx%d)",
                          qUtf8Printable(inputLabel_), frame.width(), frame.height());
                }
            }
            // Unconditional rev bump while taken: the tile's Image must
            // re-fetch every tick from the FIRST one (the provider answers
            // warm-up requests with a 1×1 transparent, which keeps the
            // placeholder up without ever flipping inputLive).
            inputRev_++;
            emit inputChanged();
        });
    }
    inputPump_->start();
    // inputLive_ flips true when the provider's first frame decodes — the
    // service can't see the provider's cache, so the QML side confirms via
    // confirmInputFrame() (warm-up honest: the pill/placeholder shows until
    // then).
    emit inputChanged();
}

// KEPT FOR COMPATIBILITY, DELIBERATELY EMPTY: frame detection moved into
// the service's pump (reading the provider's decode cache). The previous
// QML round-trip — tile's Image onStatusChanged → confirmInputFrame() →
// inputRev bump → the SAME tile's inputSource URL — was a write-in-binding
// feedback edge and QML flagged it as a binding loop.
void LiveOutputService::confirmInputFrame(const QString &label)
{
    Q_UNUSED(label)
}

void LiveOutputService::clearInput()
{
    if (inputLabel_.isEmpty())
        return;
    if (inputKind_ == QLatin1String("screen"))
        EngineBridge::instance().stopScreenPreview(inputLabel_, QStringLiteral("output"));
    else
        EngineBridge::instance().stopVideoPreview(inputLabel_, QStringLiteral("output"));
    inputLabel_.clear();
    inputKind_.clear();
    inputLive_ = false;
    if (inputPump_)
        inputPump_->stop();
    emit inputChanged();
}

// ---------------------------------------------------------------------------
// Env-gated boot self-test: VGR_OUTPUT_INPUT_TEST=1 drives the WHOLE
// taken-input chain automatically — enumerate → take the first real window
// → wait through the tap's warm-up → read the provider's decode cache → log
// a PASS/FAIL verdict. Exists so the input-preview pipeline can be verified
// from a launch log alone (no clicks, no dialog, no QML binding involved —
// the layer the binding loop poisoned). Inert without the env var.
void LiveOutputService::runEnvSelfTest()
{
    if (!qEnvironmentVariableIsSet("VGR_OUTPUT_INPUT_TEST"))
        return;
    QTimer::singleShot(2500, &instance(), [] {
        QString label;
        const QVariantList screen = EngineBridge::instance().screenDevices();
        for (const QVariant &v : screen) {
            const QVariantMap d = v.toMap();
            // First real window (not our own app's window — the engine's
            // enumeration excludes self by pid, the roster label here is
            // just belt-and-braces).
            if (d.value("id").toString().startsWith(QStringLiteral("win:"))) {
                label = d.value("label").toString();
                if (label != QCoreApplication::applicationName())
                    break;
            }
        }
        if (label.isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: no window source to take");
            return;
        }
        qInfo("LiveOutputService[selftest] taking window '%s'", qUtf8Printable(label));
        instance().takeInput(label, QStringLiteral("screen"), QString());
        if (instance().inputLabel().isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: take was refused");
            return;
        }
        QTimer::singleShot(4000, &instance(), [label] {
            // The service's OWN live flag — flipped by the decode-cache poll,
            // i.e. proof the tap produced real pixels end to end.
            if (instance().inputLive())
                qInfo("LiveOutputService[selftest] PASS: frames decoded for '%s'",
                      qUtf8Printable(label));
            else
                qWarning("LiveOutputService[selftest] FAIL: no frames decoded for '%s'",
                         qUtf8Printable(label));
            instance().clearInput();   // phase boundary — release before the next probe
            // RE-ACTIVATION PROBE — the new gesture model: activation is a
            // DOUBLE-click (one deliberate toggle in QML), so the service
            // must survive a same-source re-activation without losing the
            // tap or the frames (a stray rapid re-activation can also come
            // from a double-toggled card). Take again, take the SAME source
            // once more immediately, and expect the take — and the frames —
            // to still be alive 2.5 s later.
            QTimer::singleShot(400, &instance(), [label] {
                instance().takeInput(label, QStringLiteral("screen"), QString());
                instance().takeInput(label, QStringLiteral("screen"), QString());   // immediate re-activation
                const bool held = instance().inputLabel() == label;
                QTimer::singleShot(2500, &instance(), [label, held] {
                    if (held && instance().inputLive())
                        qInfo("LiveOutputService[selftest] REACTIVATE PASS: same-source re-activation kept the tap, frames flowing");
                    else
                        qWarning("LiveOutputService[selftest] REACTIVATE FAIL: held=%d live=%d",
                                 held ? 1 : 0, instance().inputLive() ? 1 : 0);
                    instance().clearInput();   // the self-test leaves NO side effects
                });
            });
        });
    });
}

void LiveOutputService::stop()
{
    bps::live::LiveOutputController::Instance().StopLive();
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    // Off air: the slide preview goes with it (also on a failed start).
    onAirSlide_ = QVariantMap{};
    emit onAirChanged();
    if (poll_)
        poll_->stop();
}

// ---------------------------------------------------------------------------
// Navigation while live
// ---------------------------------------------------------------------------
bool LiveOutputService::next()
{
    if (!live_)
        return false;
    auto r = pl::PresentationEngine::Instance().Next();
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

bool LiveOutputService::previous()
{
    if (!live_)
        return false;
    auto r = pl::PresentationEngine::Instance().Previous();
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

// The preview toolbar's ‹ › (FreeShow's OutputHelper.advanceOutputs): a step
// inside the on-air set is plain navigation; at either END of the set (a
// single-verse pick makes BOTH ends) the step becomes a PASSAGE step — the
// tab whose content is on air re-picks the neighbouring passage and replays
// it, exactly like the preview pane's own ‹ › pills.
void LiveOutputService::stepPassage(int direction)
{
    if (!live_)
        return;
    const bool moved = direction < 0 ? previous() : next();
    if (!moved)
        emit passageStepRequested(direction);
}

bool LiveOutputService::jumpTo(int index)
{
    if (!live_ || index < 0)
        return false;
    auto r = pl::PresentationEngine::Instance().JumpTo(size_t(index));
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The on-air slide as design blocks (the QML preview feed)
// ---------------------------------------------------------------------------
void LiveOutputService::refreshOnAirSlide()
{
    onAirSlide_ = live_ ? slideToVariantMap(pl::PresentationEngine::Instance().CurrentSlide())
                        : QVariantMap{};
}

QVariantMap LiveOutputService::onAirSlideAt(int index) const
{
    if (!live_ || index < 0)
        return {};
    auto &pres = pl::PresentationEngine::Instance();
    const pl::Presentation *active = pres.Runtime().ActivePresentation();
    if (!active || active->slides.empty())
        return {};
    // The index is a VISIBLE index (CurrentIndex() skips hidden slides, and
    // that is what onAirIndex carries) — walk the slides skipping hidden ones
    // so a thumbnail strip's indices line up with the on-air ones.
    int visible = -1;
    for (const pl::Slide &s : active->slides) {
        if (s.hidden)
            continue;
        ++visible;
        if (visible == index)
            return slideToVariantMap(&s);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Poll (onAir titles + frame counter while live)
// ---------------------------------------------------------------------------
void LiveOutputService::pollTick()
{
    if (!live_)
        return;

    auto &ctrl = bps::live::LiveOutputController::Instance();
    const qulonglong sent = ctrl.FramesSent();
    if (sent != framesSent_) {
        framesSent_ = sent;
        // Bump the cache-buster too — without this frameRev stayed 0 forever,
        // hasFrame was never true, and every monitor tile showed the
        // checkerboard while LIVE (the emit alone changes nothing: QML
        // re-evaluates the URL and gets the identical string).
        ++frameRev_;
        emit frameRevChanged();
    }

    // The engine's loop advances slides itself; mirror the runtime's state.
    auto &pres = pl::PresentationEngine::Instance();
    const pl::Slide *slide = pres.CurrentSlide();
    const QString title = slide ? QString::fromStdString(slide->title) : QString();
    const int idx = int(pres.CurrentIndex());
    const int total = int(pres.Runtime().ActivePresentation()
                              ? pres.Runtime().ActivePresentation()->slides.size()
                              : 0);
    if (title != onAirTitle_ || idx != onAirIndex_ || total != onAirTotal_) {
        onAirTitle_ = title;
        onAirIndex_ = idx;
        onAirTotal_ = total;
        refreshOnAirSlide();
        emit onAirChanged();
    } else {
        // Same title/index/total, but the slide's BLOCKS can still have moved
        // (a document edit re-synced while live, a style rebuild) — the poll
        // re-reads them so the QML preview stays honest. Cheap when unchanged:
        // a property write with no matching QML binding change is free.
        const QVariantMap next = slideToVariantMap(slide);
        if (next != onAirSlide_) {
            onAirSlide_ = next;
            emit onAirChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// The preview frame feed
// ---------------------------------------------------------------------------
QImage LivePreviewProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    // `id` is the URL path: "" = the shared PREVIEW feed (every tile's
    // default), "__out_<hash>__" = ONE output's own gated buffer (the
    // per-output pass the engine renders for that output's style). Strip the
    // ?query, resolve the buffer, read its last frame.
    QString name = id;
    const int q = name.indexOf(QLatin1Char('?'));
    if (q >= 0)
        name.truncate(q);
    QImage out;

    const std::string bufferName = name.isEmpty()
        ? std::string("__live_preview__")
        : name.toStdString();
    if (auto o = pr::RenderEngine::Instance().GetOutput(bufferName); o.ok()) {
        if (auto *fb = dynamic_cast<pr::FrameBufferOutput *>(o.value().get())) {
            const pr::Frame frame = fb->LastFrame();
            if (!frame.empty()) {
                QImage ref(reinterpret_cast<const uchar *>(frame.pixels.data()),
                           frame.width, frame.height,
                           frame.width * 4, QImage::Format_RGBA8888);
                // The engine packs colors 0xAABBGGRR (Color::Pack — R in the
                // LOW byte). Format_RGBA8888 reads bytes R,G,B,A — exactly
                // that layout on little-endian. (ARGB32 was WRONG here: it
                // reads BGRA in memory and swapped red/blue.) Copy: the
                // frame's pixels die with the lock inside LastFrame().
                out = ref.copy();
            }
        }
    }

    if (out.isNull()) {
        // Not live (or no frame yet): 1x1 transparent — the QML side reads it
        // as "no picture" the same way MediaTile does (no warning spam).
        out = QImage(1, 1, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
    }

    if (requestedSize.width() > 0 && requestedSize.height() > 0)
        out = out.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size)
        *size = out.size();
    return out;
}
