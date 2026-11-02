#include "services/CallTrace.h"
#include "services/ShowService.h"

#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "services/EventBus.h"
#include "services/ShowConverter.h"

#include "modules/import/ImportEngine.hpp"
#include "modules/import/SongText.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/ShowEditor.hpp"
#include "modules/presentation/ShowLibrary.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QDir>
#include <QPointer>
#include <QThreadPool>
#include <QJSEngine>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <string_view>

namespace bp = bps::presentation;

namespace {
std::shared_ptr<bp::PresentationDocument> showDocument()
{
    return bp::PresentationEngine::Instance().Document();
}

// QML hands over either a plain path or a file:// URL (what a file dialog
// gives). The engine's file layer wants a plain path.
QString cleanPath(const QString &path)
{
    QString p = path.trimmed();
    if (p.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        p = QUrl(p).toLocalFile();
    return p;
}

std::string vgrPath(const QString &path)
{
    QString p = cleanPath(path);
    if (!p.isEmpty() && !p.endsWith(QLatin1String(".vgr"), Qt::CaseInsensitive))
        p += QLatin1String(".vgr");
    return p.toStdString();
}

QVariantMap entryToVariant(const bp::ShowEntry &e)
{
    return {
        { QStringLiteral("path"), QString::fromStdString(e.path) },
        { QStringLiteral("id"), QString::fromStdString(e.id) },
        { QStringLiteral("name"), QString::fromStdString(e.name) },
        { QStringLiteral("category"), QString::fromStdString(e.category) },
        { QStringLiteral("modifiedMs"), qlonglong(e.modifiedMs) },
        { QStringLiteral("slideCount"), e.slideCount },
    };
}

QVariantList entriesToVariant(const std::vector<bp::ShowEntry> &entries)
{
    QVariantList out;
    for (const bp::ShowEntry &e : entries)
        out.append(entryToVariant(e));
    return out;
}

QString message(const bps::Error &e) { return QString::fromStdString(e.message); }
} // namespace

// ===========================================================================
// The queued document jobs (see the header)
// ===========================================================================

bool ShowService::JobContext::inLibrary(const QString &path) const
{
    if (libraryRoot.isEmpty())
        return false;
    return QDir::fromNativeSeparators(path).startsWith(libraryRoot, Qt::CaseInsensitive);
}

ShowService::JobContext ShowService::jobContext() const
{
    JobContext ctx;
    ctx.booted = EngineBridge::instance().booted();
    if (library_)
        ctx.libraryRoot = QDir::fromNativeSeparators(libraryPath_);
    return ctx;
}

int ShowService::runDocumentJobOffThread(const QString &kind, JobDurability durability,
                                         const std::function<JobResult()> &work)
{
    const int token = ++jobToken_;
    const bps::JobTicketPtr ticket = std::make_shared<bps::JobTicket>();
    jobs_.insert(token, LiveJob{ kind, durability, ticket });
    vgr::EngineWorker::instance().runOffThreadCancellable(ticket, [this, token, kind, ticket, work]() {
        // The engine's document call, on the worker. Its own lock and its own
        // revision check make this the supported way to use it (see the header).
        const JobResult r = work();
        // Everything the UI sees happens back on the interface thread: the toast,
        // the signals, the library sweep, and the log line.
        vgr::EngineWorker::onInterfaceThread(this, [this, token, kind, ticket, r]() {
            jobs_.remove(token);
            if (!ticket->Deliver()) {
                if (ticket->Started())
                    applyJobState(r);
                return;
            }
            finishDocumentJob(kind, r);
            emit documentJobFinished(token, kind, r.ok, r.error, r.payload);
        });
    });
    return token;
}

// ---- taking a job back (see the header) ------------------------------------
bool ShowService::cancelDocumentJob(int token)
{
    const auto it = jobs_.constFind(token);
    if (it == jobs_.constEnd())
        return false;   // it already ended (its verdict landed), or was never ours
    const QString kind = it->kind;
    const bps::JobTicketPtr ticket = it->ticket;
    if (it->durability == JobDurability::Durable)
        ticket->WithdrawVerdict();   // the file the operator asked for is still written
    else
        ticket->Abandon();           // a read nobody is waiting for: do not spend the disk
    if (!ticket->Started()) {
        jobs_.remove(token);
    }
    emit documentJobCancelled(token, kind);
    return true;
}

int ShowService::cancelDocumentJobs(const QStringList &kinds)
{
    // Collected first: cancelDocumentJob() returns after the entry is gone, and
    // the emits below can re-enter QML (a handler may cancel more of them).
    QList<int> tokens;
    for (auto it = jobs_.constBegin(); it != jobs_.constEnd(); ++it)
        if (kinds.isEmpty() || kinds.contains(it->kind))
            tokens.append(it.key());
    int cancelled = 0;
    for (const int token : tokens)
        if (cancelDocumentJob(token))
            ++cancelled;
    return cancelled;
}

void ShowService::applyJobState(const JobResult &r)
{
    if (!r.logLine.isEmpty())
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), r.logLine);
    if (!r.touchedPath.isEmpty())
        dropPeek(QDir::fromNativeSeparators(r.touchedPath));
    if (r.documentChanged)
        emit showChanged();
    if (r.openDocumentEdited)
        dropPeekForOpenPath();   // the working show changed under the centre page
    if (r.libraryChanged && library_)
        refreshLibrary();
}

bool ShowService::finishHere(const JobResult &r)
{
    if (!r.ok && !r.error.isEmpty())
        reportError(r.title.isEmpty() ? QStringLiteral("Show") : r.title, r.error);
    if (!r.savedToast.isEmpty())
        EventBus::instance().notify(QStringLiteral("Saved to %1").arg(r.savedToast),
                                    QStringLiteral("success"), QStringLiteral("Show saved"),
                                    QStringLiteral("show.saved"));
    applyJobState(r);
    return r.ok;
}

void ShowService::finishDocumentJob(const QString &kind, const JobResult &r)
{
    Q_UNUSED(kind)   // the signal carries it; the finishing is the same for every kind
    finishHere(r);
}

// ---- the published peek (peekShow) -----------------------------------------
QVariantMap ShowService::peekAnswer(const QString &path) const
{
    if (!EngineBridge::instance().booted())
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), QStringLiteral("The engine isn't running.") },
                 { QStringLiteral("pending"), false } };
    if (auto open = showDocument(); open && open->HasDocument()
        && QDir::fromNativeSeparators(QString::fromStdString(open->Path())).compare(path, Qt::CaseInsensitive) == 0)
        return { { QStringLiteral("ok"), true }, { QStringLiteral("error"), QString() },
                 { QStringLiteral("pending"), false }, { QStringLiteral("show"), ShowConverter::toVariant(open->Snapshot()) } };
    bp::PresentationDocument peek;
    auto opened = peek.Open(vgrPath(path));
    if (!opened.ok())
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), message(opened.error()) },
                 { QStringLiteral("pending"), false } };
    return { { QStringLiteral("ok"), true }, { QStringLiteral("error"), QString() },
             { QStringLiteral("pending"), false }, { QStringLiteral("show"), ShowConverter::toVariant(peek.Snapshot()) } };
}

void ShowService::peekOffThread(const QString &key) const
{
    if (peekInFlight_.contains(key) || !EngineBridge::instance().booted())
        return;
    peekInFlight_.insert(key);
    vgr::EngineWorker::instance().runOffThread([this, key]() {
        const QVariantMap answer = peekAnswer(key);
        vgr::EngineWorker::onInterfaceThread(const_cast<ShowService *>(this),
                                            [this, key, answer]() { publishPeek(key, answer); });
    });
}

void ShowService::publishPeek(const QString &key, const QVariantMap &answer) const
{
    peekInFlight_.remove(key);
    peekCache_.insert(key, answer);
    ++peekRevision_;
    emit const_cast<ShowService *>(this)->peekRevisionChanged();
}

void ShowService::dropPeek(const QString &key) const
{
    if (key.isEmpty())
        return;
    if (peekCache_.remove(key) > 0 || peekInFlight_.contains(key)) {
        ++peekRevision_;
        emit const_cast<ShowService *>(this)->peekRevisionChanged();
    }
}

void ShowService::dropPeekForOpenPath() const
{
    if (auto doc = showDocument(); doc && doc->HasDocument())
        dropPeek(QDir::fromNativeSeparators(QString::fromStdString(doc->Path())));
}

ShowService::ShowService(QObject *parent)
    : QObject(parent)
{
    const QString dirOverride = qEnvironmentVariable("VGR_LIBRARY_DIR");
    libraryPath_ = !dirOverride.isEmpty()
        ? dirOverride
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + QStringLiteral("/VGR Presenter/Shows");

    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged,
            this, &ShowService::refreshLibrary);
    if (EngineBridge::instance().booted())   // boot already ran (tests, reload)
        refreshLibrary();
}

ShowService::~ShowService() = default;

ShowService &ShowService::instance()
{
    static ShowService service;
    return service;
}

ShowService *ShowService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

void ShowService::reportError(const QString &title, const QString &msg) const
{
    EventBus::instance().notify(msg, QStringLiteral("error"), title, QStringLiteral("show.error"));
}

// ===========================================================================
// The working show
// ===========================================================================

bool ShowService::hasShow() const
{
    auto doc = showDocument();
    return doc && doc->HasDocument();
}

bool ShowService::showDirty() const
{
    auto doc = showDocument();
    return doc && doc->IsDirty();
}

QString ShowService::showPath() const
{
    auto doc = showDocument();
    return doc ? QString::fromStdString(doc->Path()) : QString();
}

QString ShowService::showName() const
{
    auto doc = showDocument();
    return doc && doc->HasDocument() ? QString::fromStdString(doc->Snapshot().name) : QString();
}

QVariantMap ShowService::currentShow() const
{
    auto doc = showDocument();
    return doc && doc->HasDocument() ? ShowConverter::toVariant(doc->Snapshot()) : QVariantMap{};
}

void ShowService::newShowDocument(const QString &name)

{
    if (auto doc = showDocument()) {
        doc->New(name.toStdString());
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("New show '%1'").arg(name));
        emit showChanged();
    }
}

int ShowService::saveShowFile(const QVariantMap &show, const QString &path)
{
    const bool booted = EngineBridge::instance().booted();
    const JobContext ctx = jobContext();
    return runDocumentJobOffThread(QStringLiteral("save"), JobDurability::Durable,
                                   [this, booted, ctx, show, path]() {
        const std::string target = vgrPath(path);
        JobResult r;
        r.title = QStringLiteral("Save show");
        auto doc = showDocument();
        if (!doc || !booted) {
            r.error = QStringLiteral("The engine isn't running, so the show can't be saved.");
            return r;
        }
        // Keep the document's identity and creation time across saves: only the
        // content comes from the UI.
        bp::Presentation incoming = ShowConverter::fromVariant(show);
        const bp::Presentation current = doc->Snapshot();
        if (incoming.id.empty()) incoming.id = current.id;
        if (incoming.name.empty()) incoming.name = current.name;
        incoming.createdAt = current.createdAt;
        doc->Replace(std::move(incoming));

        auto saved = doc->Save(target);
        r.documentChanged = true;   // announced either way, as it always was
        if (!saved.ok()) {
            r.title = QStringLiteral("Couldn't save the show");
            r.error = message(saved.error());
            return r;
        }
        const QString savedPath = QString::fromStdString(doc->Path());
        r.ok = true;
        r.logLine = QStringLiteral("Saved '%1' to %2")
                        .arg(QString::fromStdString(doc->Snapshot().name), savedPath);
        r.savedToast = savedPath;
        r.touchedPath = savedPath;
        // Only rescan when the file landed inside the library — a scan reads every
        // show in it, which is wasted work for a save somewhere else.
        r.libraryChanged = ctx.inLibrary(savedPath);
        r.payload = { { QStringLiteral("path"), savedPath } };
        return r;
    });
}

int ShowService::saveCurrentShow(const QString &path, bool quiet)
{
    const bool booted = EngineBridge::instance().booted();
    const JobContext ctx = jobContext();
    return runDocumentJobOffThread(QStringLiteral("save"), JobDurability::Durable,
                                   [this, booted, ctx, path, quiet]() {
        const std::string target = path.isEmpty() ? std::string() : vgrPath(path);
        JobResult r;
        r.title = QStringLiteral("Save show");
        auto doc = showDocument();
        if (!doc || !booted || !doc->HasDocument()) {
            r.error = QStringLiteral("There is no show to save.");
            return r;
        }
        auto saved = doc->Save(target);
        r.documentChanged = true;
        if (!saved.ok()) {
            r.title = QStringLiteral("Couldn't save the show");
            r.error = message(saved.error());
            return r;
        }
        const QString savedPath = QString::fromStdString(doc->Path());
        r.ok = true;
        r.logLine = QStringLiteral("%1 '%2' to %3")
                        .arg(quiet ? QStringLiteral("Auto-saved") : QStringLiteral("Saved"),
                             QString::fromStdString(doc->Snapshot().name), savedPath);
        if (!quiet)
            r.savedToast = savedPath;
        r.touchedPath = savedPath;
        r.libraryChanged = ctx.inLibrary(savedPath);
        r.payload = { { QStringLiteral("path"), savedPath },
                      { QStringLiteral("name"), QString::fromStdString(doc->Snapshot().name) } };
        return r;
    });
}

int ShowService::saveShowCopy(const QString &path)
{
    const bool booted = EngineBridge::instance().booted();
    return runDocumentJobOffThread(QStringLiteral("saveCopy"), JobDurability::Durable,
                                   [this, booted, path]() {
        const std::string target = vgrPath(path);
        const QString cleanTarget = cleanPath(path);
        JobResult r;
        auto doc = showDocument();
        if (!doc || !booted || !doc->HasDocument() || target.empty())
            return r;
        if (auto saved = doc->SaveCopy(target); !saved.ok()) {
            r.error = message(saved.error());
            r.title.clear();   // ...but nothing is toasted (see above)
            return r;
        }
        r.ok = true;
        r.payload = { { QStringLiteral("path"), cleanTarget } };
        return r;
    });
}

void ShowService::ensureShow(const QString &name)
{
    auto doc = showDocument();
    if (doc && !doc->HasDocument()) {
        doc->New(name.toStdString());
        emit showChanged();
    }
}

QString ShowService::pickShowToOpen()
{
    if (!EngineBridge::instance().booted()) return {};
    auto r = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        // .vgr only: the show loader refuses anything else, so offering
        // "All files" just moved the refusal from the picker to the import.
        "Open show", { "VGR shows (*.vgr)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}

QString ShowService::pickShowSavePath(const QString &defaultName)
{
    if (!EngineBridge::instance().booted()) return {};
    auto r = bps::platform::PlatformAccessor::Get().Dialogs().SaveFileDialog(
        "Save show", (defaultName.isEmpty() ? QStringLiteral("Untitled show") : defaultName).toStdString() + ".vgr");
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}

QVariantMap ShowService::peekShow(const QString &path) const
{
    const QString key = QDir::fromNativeSeparators(cleanPath(path));
    if (key.isEmpty())
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), QStringLiteral("No show was named.") },
                 { QStringLiteral("pending"), false } };
    const auto it = peekCache_.constFind(key);
    if (it != peekCache_.constEnd())
        return *it;
    peekOffThread(key);
    return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), QString() },
             { QStringLiteral("pending"), true } };
}

int ShowService::setNextTimer(const QString &path, double seconds)
{
    const JobContext ctx = jobContext();
    return runDocumentJobOffThread(QStringLiteral("timer"), JobDurability::Durable,
                                   [this, ctx, path, seconds]() {
        JobResult r = editShowOutcome(ctx, path, QStringLiteral("Couldn't set the next timer"),
                                      [&](bp::Presentation &s) { return bp::ShowEditor::SetNextTimer(s, seconds); });
        if (r.ok)
            r.logLine = QStringLiteral("Next timer %1 s on %2").arg(seconds).arg(path);
        return r;
    });
}

int ShowService::editSlides(const QString &path, const QString &op, const QStringList &slideIds, const QVariantMap &args)
{
    const JobContext ctx = jobContext();
    return runDocumentJobOffThread(QStringLiteral("slides"), JobDurability::Durable,
                                   [this, ctx, path, op, slideIds, args]() {
        return editSlidesOutcome(ctx, path, op, slideIds, args);
    });
}

ShowService::JobResult ShowService::editSlidesOutcome(const JobContext &ctx, const QString &path, const QString &op,
                                                     const QStringList &slideIds, const QVariantMap &args) const
{
    using Editor = bp::ShowEditor;
    std::vector<std::string> ids;
    for (const QString &id : slideIds) ids.push_back(id.toStdString());
    const QString title = QStringLiteral("Couldn't change the slides");
    QStringList made;
    size_t replaced = 0;
    bool ok = false;
    // What the ONE op this call runs found. Its toast, its signals and its log
    // line are the ones that land when the verdict reaches the interface thread.
    JobResult outcome;
    auto edit = [&](const QString &toastTitle,
                    const std::function<bps::Result<void>(bp::Presentation &)> &change) {
        outcome = editShowOutcome(ctx, path, toastTitle, change);
        return outcome.ok;
    };

    if (op == QLatin1String("hidden")) {
        ok = edit(title, [&](bp::Presentation &s) { return Editor::SetSlidesHidden(s, ids, args.value(QStringLiteral("hidden")).toBool()); });
    } else if (op == QLatin1String("group")) {
        ok = edit(title, [&](bp::Presentation &s) { return Editor::SetSlidesGroup(s, ids, args.value(QStringLiteral("group")).toString().toStdString()); });
    } else if (op == QLatin1String("title")) {
        ok = edit(title, [&](bp::Presentation &s) -> bps::Result<void> {
            if (ids.empty()) return bps::Error::Make(bps::Err::InvalidArgument, "ShowService", "no slide was chosen");
            for (const std::string &id : ids) {
                auto it = std::find_if(s.slides.begin(), s.slides.end(), [&](const bp::Slide &x) { return x.id == id; });
                if (it == s.slides.end()) return bps::Error::Make(bps::Err::NotFound, "ShowService", "no slide '" + id + "' in this show");
                it->title = args.value(QStringLiteral("title")).toString().toStdString();
            }
            return bps::Ok();
        });
    } else if (op == QLatin1String("format")) {
        const QString kind = args.value(QStringLiteral("kind")).toString();
        Editor::TextFormat format = Editor::TextFormat::Uppercase;
        if (kind == QLatin1String("lowercase")) format = Editor::TextFormat::Lowercase;
        else if (kind == QLatin1String("capitalize")) format = Editor::TextFormat::Capitalize;
        else if (kind == QLatin1String("trim")) format = Editor::TextFormat::Trim;
        ok = edit(title, [&](bp::Presentation &s) { return Editor::FormatSlidesText(s, ids, format); });
    } else if (op == QLatin1String("replace")) {
        ok = edit(title, [&](bp::Presentation &s) -> bps::Result<void> {
            auto r = Editor::ReplaceInSlides(s, ids, args.value(QStringLiteral("find")).toString().toStdString(), args.value(QStringLiteral("replace")).toString().toStdString(),
                                             args.value(QStringLiteral("caseSensitive")).toBool());
            if (!r.ok()) return r.error();
            replaced = r.value();
            return bps::Ok();
        });
    } else if (op == QLatin1String("split")) {
        ok = edit(title, [&](bp::Presentation &s) -> bps::Result<void> {
            auto r = Editor::SplitSlidesInHalf(s, ids);
            if (!r.ok()) return r.error();
            for (const std::string &id : r.value()) made.append(QString::fromStdString(id));
            return bps::Ok();
        });
    } else if (op == QLatin1String("merge")) {
        ok = edit(title, [&](bp::Presentation &s) { return Editor::MergeSlides(s, ids); });
    } else if (op == QLatin1String("duplicate")) {
        ok = edit(title, [&](bp::Presentation &s) -> bps::Result<void> {
            if (ids.empty()) return bps::Error::Make(bps::Err::InvalidArgument, "ShowService", "no slide was chosen");
            for (const std::string &id : ids) {
                auto r = Editor::DuplicateSlide(s, id);
                if (!r.ok()) return r.error();
                made.append(QString::fromStdString(r.value()));
            }
            return bps::Ok();
        });
    } else if (op == QLatin1String("remove")) {
        ok = edit(title, [&](bp::Presentation &s) -> bps::Result<void> {
            if (ids.empty()) return bps::Error::Make(bps::Err::InvalidArgument, "ShowService", "no slide was chosen");
            for (const std::string &id : ids)
                if (auto r = Editor::RemoveSlide(s, id); !r.ok()) return r;
            return bps::Ok();
        });
    } else if (op == QLatin1String("transition")) {
        static const QStringList kinds = { QStringLiteral("fade"), QStringLiteral("slide"), QStringLiteral("push"), QStringLiteral("zoom"), QStringLiteral("wipe"), QStringLiteral("crossfade"), QStringLiteral("custom") };
        const int kind = std::max(0, static_cast<int>(kinds.indexOf(args.value(QStringLiteral("kind")).toString())));
        ok = edit(title, [&](bp::Presentation &s) { return Editor::SetSlidesTransition(s, ids, static_cast<bp::TransitionKind>(kind), args.value(QStringLiteral("ms"), 500).toDouble()); });
    } else if (op == QLatin1String("outputs")) {
        std::vector<std::string> outputs;
        for (const QVariant &o : args.value(QStringLiteral("outputs")).toList()) outputs.push_back(o.toString().toStdString());
        ok = edit(title, [&](bp::Presentation &s) { return Editor::SetSlidesOutputs(s, ids, outputs); });
    } else if (op == QLatin1String("timer")) {
        ok = edit(QStringLiteral("Couldn't set the next timer"), [&](bp::Presentation &s) { return Editor::SetNextTimer(s, args.value(QStringLiteral("seconds")).toDouble(), ids); });
    } else {
        // An op that does not exist touches no document: the verdict is the whole
        // answer (the toast lands with it, see finishHere).
        outcome.title = title;
        outcome.error = QStringLiteral("There is no slide operation '%1'.").arg(op);
    }

    if (ok)
        outcome.logLine = QStringLiteral("Slide %1 on %2 slide(s) of %3").arg(op).arg(slideIds.size()).arg(path);
    outcome.payload = { { QStringLiteral("ok"), ok }, { QStringLiteral("made"), made },
                        { QStringLiteral("replaced"), static_cast<int>(replaced) } };
    return outcome;
}

int ShowService::openShowFile(const QString &path)
{
    const bool booted = EngineBridge::instance().booted();
    return runDocumentJobOffThread(QStringLiteral("open"), JobDurability::Abandonable,
                                   [this, booted, path]() {
        const std::string target = vgrPath(path);
        JobResult r;
        r.title = QStringLiteral("Couldn't open the show");
        auto doc = showDocument();
        const auto failed = [&r](const QString &why) {
            r.payload = { { QStringLiteral("ok"), false }, { QStringLiteral("error"), why } };
            return r;
        };
        if (!doc || !booted)
            return failed(QStringLiteral("The engine isn't running."));
        auto opened = doc->Open(target);
        if (!opened.ok()) {
            r.error = message(opened.error());
            return failed(r.error);
        }
        r.ok = true;
        r.documentChanged = true;
        r.logLine = QStringLiteral("Opened '%1' from %2")
                        .arg(QString::fromStdString(doc->Snapshot().name), path);
        r.payload = { { QStringLiteral("ok"), true },
                      { QStringLiteral("error"), QString() },
                      { QStringLiteral("show"), ShowConverter::toVariant(doc->Snapshot()) } };
        return r;
    });
}

void ShowService::markShowClean()
{
    if (auto doc = showDocument()) {
        const bool wasDirty = doc->IsDirty();
        doc->MarkClean();
        if (wasDirty)
            emit showChanged();
    }
}

void ShowService::markShowDirty()
{
    if (auto doc = showDocument()) {
        const bool wasDirty = doc->IsDirty();
        doc->MarkDirty();
        if (!wasDirty)
            emit showChanged();
    }
}

// ===========================================================================
// Editing the show -- the engine (ShowEditor) does the work
// ===========================================================================// THE ONE BODY an edit has, answering instead of reporting: every entry point
// above reaches the engine through these two, so the queued path and the direct
// path can never drift apart (see the header).
ShowService::JobResult ShowService::applyEditOutcome(
    const QString &title,
    const std::function<bps::Result<void>(bp::Presentation &)> &fn) const
{
    JobResult r;
    r.title = title;
    auto doc = showDocument();
    if (!doc || !doc->HasDocument()) {
        r.error = QStringLiteral("No show is open.");
        return r;
    }
    if (auto res = doc->Edit(fn); !res.ok()) {
        r.error = message(res.error());
        return r;
    }
    r.ok = true;
    r.documentChanged = true;
    // The WORKING show changed: a centre page showing it is reading a stale
    // snapshot now (see dropPeekForOpenPath).
    r.openDocumentEdited = true;
    return r;
}

ShowService::JobResult ShowService::editShowOutcome(
    const JobContext &ctx, const QString &path, const QString &title,
    const std::function<bps::Result<void>(bp::Presentation &)> &change) const
{
    JobResult r;
    r.title = title;
    if (!ctx.booted || path.isEmpty())
        return r;   // silent, as the synchronous entry point was
    const QString wanted = QDir::fromNativeSeparators(path);

    // The show that is open in the editor takes it into the open document (unsaved, like any other edit).
    auto open = showDocument();
    if (open && open->HasDocument()
        && QDir::fromNativeSeparators(QString::fromStdString(open->Path())).compare(wanted, Qt::CaseInsensitive) == 0)
        return applyEditOutcome(title, change);

    bp::PresentationDocument file;
    if (auto opened = file.Open(vgrPath(path)); !opened.ok()) {
        r.error = message(opened.error());
        return r;
    }
    bp::Presentation show = file.Snapshot();
    if (auto changed = change(show); !changed.ok()) {
        r.error = message(changed.error());
        return r;
    }
    file.Replace(std::move(show));
    if (auto saved = file.Save(vgrPath(path)); !saved.ok()) {
        r.error = message(saved.error());
        return r;
    }
    r.ok = true;
    r.touchedPath = cleanPath(path);
    r.libraryChanged = ctx.inLibrary(wanted);
    return r;
}

bool ShowService::applyEdit(const QString &title,
                            const std::function<bps::Result<void>(bp::Presentation &)> &fn)

{
    return finishHere(applyEditOutcome(title, fn));
}

QString ShowService::applyEditForId(const QString &title,
                                    const std::function<bps::Result<std::string>(bp::Presentation &)> &fn)
{
    std::string id;
    const bool ok = applyEdit(title, [&](bp::Presentation &show) -> bps::Result<void> {
        auto r = fn(show);
        if (!r.ok()) return r.error();
        id = r.value();
        return bps::Ok();
    });
    return ok ? QString::fromStdString(id) : QString();
}

namespace {
std::optional<size_t> optIndex(int index)
{
    return index < 0 ? std::nullopt : std::optional<size_t>(size_t(index));
}
} // namespace

// ---- Categories ----
QString ShowService::addCategory(const QVariantMap &category)
{
    return applyEditForId(QStringLiteral("Couldn't add the category"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AddCategory(s, ShowConverter::categoryFromVariant(category));
    });
}

bool ShowService::updateCategory(const QString &id, const QVariantMap &patch)
{
    bp::CategoryPatch p;
    if (patch.contains(QStringLiteral("name")))
        p.name = patch.value(QStringLiteral("name")).toString().toStdString();
    if (patch.contains(QStringLiteral("contentType")))
        p.contentType = patch.value(QStringLiteral("contentType")).toString().toStdString();
    if (patch.contains(QStringLiteral("templateId")))
        p.templateId = patch.value(QStringLiteral("templateId")).toString().toStdString();
    if (patch.contains(QStringLiteral("outputs"))) {
        std::vector<std::string> outputs;
        for (const QVariant &o : patch.value(QStringLiteral("outputs")).toList())
            outputs.push_back(o.toString().toStdString());
        p.outputs = std::move(outputs);
    }
    if (patch.contains(QStringLiteral("meta")))
        p.metaJson = ShowConverter::categoryFromVariant({ { QStringLiteral("meta"), patch.value(QStringLiteral("meta")) } }).metaJson;
    return applyEdit(QStringLiteral("Couldn't update the category"), [&](bp::Presentation &s) {
        return bp::ShowEditor::UpdateCategory(s, id.toStdString(), p);
    });
}

bool ShowService::removeCategory(const QString &id)
{
    return applyEdit(QStringLiteral("Couldn't remove the category"), [&](bp::Presentation &s) {
        return bp::ShowEditor::RemoveCategory(s, id.toStdString());
    });
}

bool ShowService::moveCategory(const QString &id, int index)
{
    return applyEdit(QStringLiteral("Couldn't move the category"), [&](bp::Presentation &s) {
        return bp::ShowEditor::MoveCategory(s, id.toStdString(), size_t(std::max(index, 0)));
    });
}

bool ShowService::assignCategoryTemplate(const QString &categoryId, const QString &templateId)
{
    return applyEdit(QStringLiteral("Couldn't assign the template"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AssignTemplate(s, categoryId.toStdString(), templateId.toStdString());
    });
}

bool ShowService::setSlideCategory(const QString &slideId, const QString &categoryId)
{
    return applyEdit(QStringLiteral("Couldn't change the slide's category"), [&](bp::Presentation &s) {
        return bp::ShowEditor::SetSlideCategory(s, slideId.toStdString(), categoryId.toStdString());
    });
}

// ---- Templates ----
QString ShowService::addTemplate(const QVariantMap &tmpl)
{
    return applyEditForId(QStringLiteral("Couldn't add the template"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AddTemplate(s, ShowConverter::templateFromVariant(tmpl));
    });
}

bool ShowService::updateTemplate(const QString &id, const QVariantMap &tmpl)
{
    return applyEdit(QStringLiteral("Couldn't update the template"), [&](bp::Presentation &s) {
        return bp::ShowEditor::UpdateTemplate(s, id.toStdString(), ShowConverter::templateFromVariant(tmpl));
    });
}

bool ShowService::removeTemplate(const QString &id, bool force)
{
    return applyEdit(QStringLiteral("Couldn't remove the template"), [&](bp::Presentation &s) {
        return bp::ShowEditor::RemoveTemplate(s, id.toStdString(), force);
    });
}

QString ShowService::duplicateTemplate(const QString &id)
{
    return applyEditForId(QStringLiteral("Couldn't duplicate the template"), [&](bp::Presentation &s) {
        return bp::ShowEditor::DuplicateTemplate(s, id.toStdString());
    });
}

// ---- Overlays ----
QString ShowService::addOverlay(const QVariantMap &overlay)
{
    return applyEditForId(QStringLiteral("Couldn't add the overlay"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AddOverlay(s, ShowConverter::overlayFromVariant(overlay));
    });
}

bool ShowService::updateOverlay(const QString &id, const QVariantMap &overlay)
{
    return applyEdit(QStringLiteral("Couldn't update the overlay"), [&](bp::Presentation &s) {
        return bp::ShowEditor::UpdateOverlay(s, id.toStdString(), ShowConverter::overlayFromVariant(overlay));
    });
}

bool ShowService::removeOverlay(const QString &id)
{
    return applyEdit(QStringLiteral("Couldn't remove the overlay"), [&](bp::Presentation &s) {
        return bp::ShowEditor::RemoveOverlay(s, id.toStdString());
    });
}

bool ShowService::setOverlayEnabled(const QString &id, bool enabled)
{
    return applyEdit(QStringLiteral("Couldn't change the overlay"), [&](bp::Presentation &s) {
        return bp::ShowEditor::SetOverlayEnabled(s, id.toStdString(), enabled);
    });
}

// ---- Slides ----
QString ShowService::addSlide(const QVariantMap &slide, int index)
{
    return applyEditForId(QStringLiteral("Couldn't add the slide"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AddSlide(s, ShowConverter::slideFromVariant(slide), optIndex(index));
    });
}

bool ShowService::updateSlide(const QString &id, const QVariantMap &patch)
{
    return applyEdit(QStringLiteral("Couldn't update the slide"), [&](bp::Presentation &s) -> bps::Result<void> {
        const bp::Slide *current = bp::SlideResolver::FindSlide(s, id.toStdString());
        if (!current)
            return bps::Error::Make(bps::Err::NotFound, "ShowService", "no slide '" + id.toStdString() + "' in this show");
        // Start from what the engine already has and change only the keys present.
        bp::Slide merged = *current;
        const auto has = [&](const char *k) { return patch.contains(QLatin1String(k)); };
        const auto str = [&](const char *k) { return patch.value(QLatin1String(k)).toString().toStdString(); };
        if (has("title")) merged.title = str("title");
        if (has("tag")) {
            if (str("tag").empty()) { if (!merged.tags.empty()) merged.tags.erase(merged.tags.begin()); }
            else if (merged.tags.empty()) merged.tags.push_back(str("tag"));
            else merged.tags.front() = str("tag");
        }
        if (has("background")) merged.background = str("background");
        if (has("filters"))
            ShowConverter::applyFilters(merged, patch.value(QStringLiteral("filters")).toMap());
        if (has("categoryId")) merged.categoryId = str("categoryId");
        if (has("nextTimer")) merged.durationMs = std::clamp(patch.value(QStringLiteral("nextTimer")).toDouble(), 0.0, 3600.0) * 1000.0;
        if (has("blocks")) {
            merged.blocks.clear();
            for (const QVariant &b : patch.value(QStringLiteral("blocks")).toList())
                merged.blocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
        }
        if (has("tagColor") || has("line1") || has("line2") || has("ref")) {
            // Keep the meta keys we do not own; update the ones present. `text` mirrors
            // line1 + line2 so engine features that read text see the slide's content.
            QVariantMap meta = ShowConverter::metaOf(merged);
            for (const char *k : { "tagColor", "line1", "line2", "ref" })
                if (has(k)) meta.insert(QLatin1String(k), patch.value(QLatin1String(k)));
            merged.metaJson = ShowConverter::metaToJsonString(meta);
            const QString l1 = meta.value(QStringLiteral("line1")).toString();
            const QString l2 = meta.value(QStringLiteral("line2")).toString();
            merged.text = (l2.isEmpty() ? l1 : l1 + QLatin1Char('\n') + l2).toStdString();
        }
        return bp::ShowEditor::UpdateSlide(s, id.toStdString(), std::move(merged));
    });
}

QVariantMap ShowService::slideOf(const QString &id) const
{
    auto doc = showDocument();
    if (!doc || !doc->HasDocument()) return {};
    const bp::Presentation show = doc->Snapshot();
    const bp::Slide *slide = bp::SlideResolver::FindSlide(show, id.toStdString());
    if (!slide) return {};
    bp::Presentation one;
    one.slides.push_back(*slide);
    const QVariantList slides = ShowConverter::toVariant(one).value(QStringLiteral("slides")).toList();
    return slides.isEmpty() ? QVariantMap{} : slides.first().toMap();
}

QVariantMap ShowService::blockOf(const QString &slideId, const QString &blockId) const
{
    const QVariantMap slide = slideOf(slideId);
    for (const QVariant &b : slide.value(QStringLiteral("blocks")).toList())
        if (b.toMap().value(QStringLiteral("key")).toString() == blockId)
            return b.toMap();
    return {};
}

bool ShowService::removeSlide(const QString &id)
{
    return applyEdit(QStringLiteral("Couldn't remove the slide"), [&](bp::Presentation &s) {
        return bp::ShowEditor::RemoveSlide(s, id.toStdString());
    });
}

bool ShowService::moveSlide(const QString &id, int index)
{
    return applyEdit(QStringLiteral("Couldn't move the slide"), [&](bp::Presentation &s) {
        return bp::ShowEditor::MoveSlide(s, id.toStdString(), size_t(std::max(index, 0)));
    });
}

QString ShowService::duplicateSlide(const QString &id)
{
    return applyEditForId(QStringLiteral("Couldn't duplicate the slide"), [&](bp::Presentation &s) {
        return bp::ShowEditor::DuplicateSlide(s, id.toStdString());
    });
}

// ---- Content blocks ----
QString ShowService::addBlock(const QString &slideId, const QVariantMap &block, int index)
{
    return applyEditForId(QStringLiteral("Couldn't add the item"), [&](bp::Presentation &s) {
        return bp::ShowEditor::AddBlock(s, slideId.toStdString(), ShowConverter::blockFromVariant(block), optIndex(index));
    });
}

bool ShowService::updateBlock(const QString &slideId, const QString &blockId, const QVariantMap &block)
{
    return applyEdit(QStringLiteral("Couldn't update the item"), [&](bp::Presentation &s) {
        return bp::ShowEditor::UpdateBlock(s, slideId.toStdString(), blockId.toStdString(),
                                           ShowConverter::blockFromVariant(block));
    });
}

bool ShowService::removeBlock(const QString &slideId, const QString &blockId)
{
    return applyEdit(QStringLiteral("Couldn't delete the item"), [&](bp::Presentation &s) {
        return bp::ShowEditor::RemoveBlock(s, slideId.toStdString(), blockId.toStdString());
    });
}

bool ShowService::moveBlock(const QString &slideId, const QString &blockId, int index)
{
    return applyEdit(QStringLiteral("Couldn't reorder the item"), [&](bp::Presentation &s) {
        return bp::ShowEditor::MoveBlock(s, slideId.toStdString(), blockId.toStdString(), size_t(std::max(index, 0)));
    });
}

QString ShowService::duplicateBlock(const QString &slideId, const QString &blockId)
{
    return applyEditForId(QStringLiteral("Couldn't duplicate the item"), [&](bp::Presentation &s) {
        return bp::ShowEditor::DuplicateBlock(s, slideId.toStdString(), blockId.toStdString());
    });
}

// ===========================================================================
// Categories, templates, overlays at work
// ===========================================================================

QVariantMap ShowService::resolveSlide(const QVariantMap &show, const QString &slideId,
                                      const QString &outputId)
{
    const bp::Presentation presentation = ShowConverter::fromVariant(show);
    auto resolved = bp::SlideResolver::Resolve(presentation, slideId.toStdString(), outputId.toStdString());
    return resolved.ok() ? ShowConverter::resolvedToVariant(resolved.value()) : QVariantMap{};
}

QStringList ShowService::validateShow(const QVariantMap &show)
{
    QStringList out;
    for (const std::string &issue : bp::SlideResolver::DanglingReferences(ShowConverter::fromVariant(show)))
        out << QString::fromStdString(issue);
    return out;
}

int ShowService::saveTemplateFile(const QVariantMap &tmpl, const QString &path)
{
    return runDocumentJobOffThread(QStringLiteral("templateSave"), JobDurability::Durable,
                                   [tmpl, path]() {
        const std::string target = vgrPath(path);
        const QString cleanTarget = cleanPath(path);
        JobResult r;
        r.title = QStringLiteral("Couldn't save the template");
        auto saved = bp::TemplateFile::Save(ShowConverter::templateFromVariant(tmpl), target);
        if (!saved.ok()) {
            r.error = message(saved.error());
            return r;
        }
        r.ok = true;
        r.payload = { { QStringLiteral("path"), cleanTarget } };
        return r;
    });
}

int ShowService::loadTemplateFile(const QString &path)
{
    return runDocumentJobOffThread(QStringLiteral("templateLoad"), JobDurability::Abandonable,
                                   [path]() {
        const std::string target = vgrPath(path);
        JobResult r;
        r.title = QStringLiteral("Couldn't open the template");
        auto loaded = bp::TemplateFile::Load(target);
        if (!loaded.ok()) {
            r.error = message(loaded.error());
            r.payload = { { QStringLiteral("ok"), false }, { QStringLiteral("error"), r.error } };
            return r;
        }
        r.ok = true;
        r.payload = { { QStringLiteral("ok"), true },
                      { QStringLiteral("error"), QString() },
                      { QStringLiteral("template"), ShowConverter::templateToVariant(loaded.value()) } };
        return r;
    });
}

// ===========================================================================
// Library
// ===========================================================================

bp::ShowLibrary *ShowService::library()
{
    // The library reads files through the engine's platform layer, which exists
    // only once the engine has booted.
    if (!EngineBridge::instance().booted())
        return nullptr;
    if (!library_)
        library_ = std::make_shared<bp::ShowLibrary>(libraryPath_.toStdString());
    return library_.get();
}

void ShowService::publishLibrary()
{
    categories_.clear();
    shows_.clear();
    problems_.clear();
    if (library_) {
        for (const std::string &c : library_->Categories())
            categories_ << QString::fromStdString(c);
        shows_ = entriesToVariant(library_->Shows());
        for (const std::string &p : library_->Problems())
            problems_ << QString::fromStdString(p);
    }
    emit libraryChanged();
}

void ShowService::setLibraryPath(const QString &path)
{
    const QString p = cleanPath(path);
    if (p.isEmpty() || p == libraryPath_)
        return;
    libraryPath_ = p;
    library_.reset();   // recreated on next use against the new folder
    refreshLibrary();
}

void ShowService::refreshLibrary()
{
    // Breadcrumb: this now walks the library on a worker, but the entry point
    // still runs on the GUI thread — a stall here names itself in the log.
    VGR_CALL_TRACE("ShowService.refreshLibrary");
    if (!library())
        return;
    if (refreshBusy_) {
        refreshPending_ = true;   // one rescan follows the pass in flight
        return;
    }
    refreshBusy_ = true;
    std::shared_ptr<bp::ShowLibrary> keep = library_;   // alive past a path change
    QPointer<ShowService> guard(this);
    QThreadPool::globalInstance()->start([guard, keep]() {
        auto r = keep->Refresh();
        const QString failure = r.ok() ? QString() : QString::fromStdString(r.error().message);
        QMetaObject::invokeMethod(guard, [guard, failure]() {
            if (!guard)
                return;
            ShowService *self = guard.data();
            self->refreshBusy_ = false;
            if (!failure.isEmpty())
                self->reportError(QStringLiteral("Couldn't read the show library"), failure);
            self->publishLibrary();
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("Library"),
                                QStringLiteral("Show library scanned: %1 show(s) in %2")
                                    .arg(self->libraryShows().size()).arg(self->libraryPath_));
            if (self->refreshPending_) {
                self->refreshPending_ = false;
                self->refreshLibrary();   // a request that arrived mid-scan
            }
        }, Qt::QueuedConnection);
    });
}

QVariantList ShowService::libraryShowsIn(const QString &category) const
{
    return library_ ? entriesToVariant(library_->ShowsIn(category.toStdString())) : QVariantList{};
}

QVariantList ShowService::searchLibrary(const QString &text) const
{
    return library_ ? entriesToVariant(library_->Search(text.toStdString())) : QVariantList{};
}

bool ShowService::createLibraryCategory(const QString &name)
{
    auto *lib = library();
    if (!lib) return false;
    auto r = lib->CreateCategory(name.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't create the category"), message(r.error()));
        return false;
    }
    publishLibrary();
    return true;
}

bool ShowService::renameLibraryCategory(const QString &from, const QString &to)
{
    auto *lib = library();
    if (!lib) return false;
    auto r = lib->RenameCategory(from.toStdString(), to.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't rename the category"), message(r.error()));
        return false;
    }
    publishLibrary();
    return true;
}

bool ShowService::removeLibraryCategory(const QString &name)
{
    auto *lib = library();
    if (!lib) return false;
    auto r = lib->RemoveCategory(name.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't remove the category"), message(r.error()));
        return false;
    }
    publishLibrary();
    return true;
}

bool ShowService::clearLibraryCategory(const QString &name)
{
    auto *lib = library();
    if (!lib) return false;
    auto r = lib->ClearCategory(name.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't clear the category"), message(r.error()));
        return false;
    }
    publishLibrary();
    return true;
}

bool ShowService::clearLibraryCategoryNoPublish(const QString &name)
{
    auto *lib = library();
    if (!lib) return false;
    auto r = lib->ClearCategory(name.toStdString());
    if (!r.ok()) {
        // EventBus marshals cross-thread calls onto the GUI thread itself.
        reportError(QStringLiteral("Couldn't clear the category"), message(r.error()));
        return false;
    }
    return true;
}

QString ShowService::moveShowToCategory(const QString &path, const QString &category)
{
    auto *lib = library();
    if (!lib) return {};
    auto r = lib->MoveShow(cleanPath(path).toStdString(), category.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't move the show"), message(r.error()));
        return {};
    }
    publishLibrary();
    return QString::fromStdString(r.value());
}

QString ShowService::renameShow(const QString &path, const QString &newName)
{
    auto *lib = library();
    if (!lib) return {};
    auto r = lib->RenameShow(cleanPath(path).toStdString(), newName.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't rename the show"), message(r.error()));
        return {};
    }
    publishLibrary();
    return QString::fromStdString(r.value());
}

QString ShowService::duplicateShow(const QString &path, const QString &newName)
{
    auto *lib = library();
    if (!lib) return {};
    auto r = lib->DuplicateShow(cleanPath(path).toStdString(), newName.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't duplicate the show"), message(r.error()));
        return {};
    }
    publishLibrary();
    return QString::fromStdString(r.value());
}

int ShowService::rewriteShowFromText(const QString &path, const QString &text)
{
    const bool booted = EngineBridge::instance().booted();
    return runDocumentJobOffThread(QStringLiteral("rewrite"), JobDurability::Durable,
                                   [booted, text, path]() {
        const std::string p = vgrPath(path);
        JobResult r;
        r.title = QStringLiteral("Couldn't open the show");
        if (!booted)
            return r;   // silent, as the synchronous entry point was

        bp::PresentationDocument doc;
        if (auto opened = doc.Open(p); !opened.ok()) {
            r.error = message(opened.error());
            return r;
        }
        bp::Presentation show = doc.Snapshot();

        bps::import::ImportedShow imported = bps::import::ParseSongText(text.toStdString());
        if (imported.sections.empty()) {
            r.title = QStringLiteral("Couldn't save the edit");
            r.error = QStringLiteral("There is no text.");
            return r;
        }

        bps::import::ShowBuildOptions options;
        if (!show.slides.empty()) {
            options.background = show.slides.front().background;
            options.templateBlocks = show.slides.front().blocks;
        }
        bp::Presentation rebuilt = bps::import::ShowFromImported(imported, options);

        const std::string categoryId = show.categories.empty() ? "" : show.categories.front().id;
        for (bp::Slide &slide : rebuilt.slides)
            slide.categoryId = categoryId;
        show.slides = std::move(rebuilt.slides);

        doc.Replace(std::move(show));
        if (auto saved = doc.Save(p); !saved.ok()) {
            r.title = QStringLiteral("Couldn't save the edit");
            r.error = message(saved.error());
            return r;
        }
        r.ok = true;
        r.libraryChanged = true;
        r.documentChanged = true;
        r.touchedPath = cleanPath(path);
        return r;
    });
}

int ShowService::updateShowTextItems(const QString &path, const QString &slideId,
                                     const QVariantMap &textByBlockId)
{
    const bool booted = EngineBridge::instance().booted();
    return runDocumentJobOffThread(QStringLiteral("quickEdit"), JobDurability::Durable,
                                   [booted, path, slideId, textByBlockId]() {
        JobResult r;
        r.title = QStringLiteral("Couldn't save the quick edit");
        if (!booted || path.isEmpty())
            return r;

        // Keep the centre's file edit isolated from the Edit screen's working
        // document, and patch only the requested text items in place.
        bp::PresentationDocument doc;
        if (auto opened = doc.Open(vgrPath(path)); !opened.ok()) {
            r.error = message(opened.error());
            return r;
        }
        auto changed = doc.Edit([&](bp::Presentation &show) -> bps::Result<void> {
            auto slide = std::find_if(show.slides.begin(), show.slides.end(),
                                      [&](const bp::Slide &candidate) {
                return candidate.id == slideId.toStdString();
            });
            if (slide == show.slides.end())
                return bps::Error::Make(bps::Err::NotFound, "ShowService",
                                        "the slide changed before the quick edit was saved");

            bool updated = false;
            for (auto it = textByBlockId.constBegin(); it != textByBlockId.constEnd(); ++it) {
                const std::string blockId = it.key().toStdString();
                const std::string text = it.value().toString().toStdString();
                if (blockId == "@slide_text") {
                    slide->text = text;
                    updated = true;
                    continue;
                }
                auto block = std::find_if(slide->blocks.begin(), slide->blocks.end(),
                                          [&](const bp::ContentBlock &candidate) {
                    return candidate.id == blockId && candidate.kind == "text";
                });
                if (block == slide->blocks.end())
                    return bps::Error::Make(bps::Err::NotFound, "ShowService",
                                            "a text item changed before the quick edit was saved");
                block->text = text;
                updated = true;
            }
            if (!updated)
                return bps::Error::Make(bps::Err::InvalidArgument, "ShowService",
                                        "there is no text to save");

            if (!slide->blocks.empty()) {
                slide->text.clear();
                for (const bp::ContentBlock &block : slide->blocks) {
                    if (block.kind != "text" || block.text.empty())
                        continue;
                    if (!slide->text.empty())
                        slide->text.push_back('\n');
                    slide->text += block.text;
                }
            }
            return bps::presentation::ShowEditor::UpdateSlide(show, slideId.toStdString(), *slide);
        });
        if (!changed.ok()) {
            r.error = message(changed.error());
            return r;
        }
        if (auto saved = doc.Save(vgrPath(path)); !saved.ok()) {
            r.error = message(saved.error());
            return r;
        }

        r.ok = true;
        r.documentChanged = true;
        r.libraryChanged = true;
        r.touchedPath = cleanPath(path);
        r.logLine = QStringLiteral("Quick-edited text items on slide %1 in %2")
                        .arg(slideId, path);
        return r;
    });
}

int ShowService::applyTextEditBatch(const QVariantList &slideEdits)
{
    const bool booted = EngineBridge::instance().booted();
    return runDocumentJobOffThread(QStringLiteral("textEditBatch"), JobDurability::Durable,
                                   [booted, slideEdits]() {
        JobResult r;
        r.title = QStringLiteral("Couldn't apply the text edits");
        if (!booted) {
            r.error = QStringLiteral("The engine isn't running.");
            r.logLine = QStringLiteral("Text edit batch rejected before parsing: engine is not running");
            return r;
        }
        const auto doc = showDocument();
        if (!doc || !doc->HasDocument()) {
            r.error = QStringLiteral("No show is open.");
            r.logLine = QStringLiteral("Text edit batch rejected before applying: no open show");
            return r;
        }

        QStringList changedSlideIds;
        int requestedNewItems = 0;
        int createdNewItems = 0;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Text edit"),
                            QStringLiteral("Worker received batch with %1 slide edit(s)")
                                .arg(slideEdits.size()));
        QString failedSlideId;
        QString failedOperation;
        auto changed = doc->Edit([&](bp::Presentation &show) -> bps::Result<void> {
            for (const QVariant &entry : slideEdits) {
                const QVariantMap edit = entry.toMap();
                const QString slideId = edit.value(QStringLiteral("slideId")).toString();
                failedSlideId = slideId;
                auto slide = std::find_if(show.slides.begin(), show.slides.end(),
                                          [&](const bp::Slide &candidate) {
                    return candidate.id == slideId.toStdString();
                });
                if (slide == show.slides.end()) {
                    failedOperation = QStringLiteral("slide lookup");
                    return bps::Error::Make(bps::Err::NotFound, "ShowService",
                                            "a slide changed before the text edit was applied");
                }

                bool slideChanged = false;
                const QVariantMap textByBlockId = edit.value(QStringLiteral("textByBlockId")).toMap();
                std::vector<std::pair<int, std::string>> newItems;
                EngineBridge::write(QStringLiteral("debug"), QStringLiteral("Text edit"),
                                    QStringLiteral("Worker parsing slide %1: %2 item change(s)")
                                        .arg(slideId).arg(textByBlockId.size()));
                for (auto it = textByBlockId.constBegin(); it != textByBlockId.constEnd(); ++it) {
                    const std::string key = it.key().toStdString();
                    const std::string text = it.value().toString().toStdString();
                    constexpr std::string_view kNewPrefix = "@new:";
                    if (key.compare(0, kNewPrefix.size(), kNewPrefix) == 0) {
                        ++requestedNewItems;
                        bool ok = false;
                        const int order = QString::fromStdString(key.substr(kNewPrefix.size())).toInt(&ok);
                        if (!ok || order < 0) {
                            failedOperation = QStringLiteral("new-item marker validation");
                            return bps::Error::Make(bps::Err::InvalidArgument, "ShowService",
                                                    "a new text item has an invalid position");
                        }
                        newItems.emplace_back(order, text);
                        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Text edit"),
                                            QStringLiteral("Queued new text item for slide %1 at text index %2 (%3 chars)")
                                                .arg(slideId).arg(order).arg(text.size()));
                        continue;
                    }
                    if (key == "@slide_text") {
                        slide->text = text;
                        slideChanged = true;
                        continue;
                    }
                    auto block = std::find_if(slide->blocks.begin(), slide->blocks.end(),
                                              [&](const bp::ContentBlock &candidate) {
                        return candidate.id == key && candidate.kind == "text";
                    });
                    if (block == slide->blocks.end()) {
                        failedOperation = QStringLiteral("existing text-item lookup");
                        return bps::Error::Make(bps::Err::NotFound, "ShowService",
                                                "a text item changed before the text edit was applied");
                    }
                    block->text = text;
                    slideChanged = true;
                }

                std::sort(newItems.begin(), newItems.end(), [](const auto &a, const auto &b) {
                    return a.first < b.first;
                });
                const QString fontFamily = edit.value(QStringLiteral("fontFamily")).toString();
                for (std::size_t i = 0; i < newItems.size(); ++i) {
                    const int count = static_cast<int>(slide->blocks.size());
                    QVariantMap block{
                        { QStringLiteral("kind"), QStringLiteral("text") },
                        { QStringLiteral("text"), QString::fromStdString(newItems[i].second) },
                        { QStringLiteral("x"), 40 + (count % 6) * 20 },
                        { QStringLiteral("y"), 40 + (count % 6) * 20 },
                        { QStringLiteral("width"), 220 },
                        { QStringLiteral("height"), 44 },
                        { QStringLiteral("meta"), QVariantMap{
                            { QStringLiteral("fontFamily"), fontFamily }
                        } }
                    };
                    auto added = bp::ShowEditor::AddBlock(show, slideId.toStdString(),
                                                          ShowConverter::blockFromVariant(block), 0);
                    if (!added.ok()) {
                        failedOperation = QStringLiteral("AddBlock");
                        return added.error();
                    }
                    ++createdNewItems;
                    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Text edit"),
                                        QStringLiteral("AddBlock created text item %1/%2 on slide %3 (%4 chars)")
                                            .arg(i + 1).arg(newItems.size()).arg(slideId)
                                            .arg(newItems[i].second.size()));
                    slideChanged = true;
                }

                if (!slideChanged)
                    continue;
                if (!slide->blocks.empty()) {
                    slide->text.clear();
                    for (const bp::ContentBlock &block : slide->blocks) {
                        if (block.kind != "text" || block.text.empty())
                            continue;
                        if (!slide->text.empty())
                            slide->text.push_back('\n');
                        slide->text += block.text;
                    }
                }
                changedSlideIds.append(slideId);
            }
            return bps::Ok();
        });
        if (!changed.ok()) {
            r.error = message(changed.error());
            r.logLine = QStringLiteral("Text edit batch failed at %1 for slide %2; new text items requested=%3, created=%4: %5")
                            .arg(failedOperation, failedSlideId)
                            .arg(requestedNewItems)
                            .arg(createdNewItems)
                            .arg(r.error);
            return r;
        }

        const bp::Presentation snapshot = doc->Snapshot();
        QVariantList updatedSlides;
        for (const QString &id : changedSlideIds) {
            const bp::Slide *slide = bp::SlideResolver::FindSlide(snapshot, id.toStdString());
            if (!slide)
                continue;
            bp::Presentation one;
            one.slides.push_back(*slide);
            const QVariantList slides = ShowConverter::toVariant(one)
                                            .value(QStringLiteral("slides")).toList();
            if (!slides.isEmpty())
                updatedSlides.append(slides.first());
        }
        r.ok = true;
        r.documentChanged = true;
        r.openDocumentEdited = true;
        r.payload.insert(QStringLiteral("slides"), updatedSlides);
        r.payload.insert(QStringLiteral("requestedNewItems"), requestedNewItems);
        r.payload.insert(QStringLiteral("createdNewItems"), createdNewItems);
        r.logLine = QStringLiteral("Text edit batch applied to %1 slide(s); new text items requested=%2, created=%3")
                        .arg(updatedSlides.size())
                        .arg(requestedNewItems)
                        .arg(createdNewItems);
        return r;
    });
}

QString ShowService::deleteShow(const QString &path)
{
    auto *lib = library();
    if (!lib) return {};
    auto r = lib->DeleteShow(cleanPath(path).toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't delete the show"), message(r.error()));
        return {};
    }
    publishLibrary();
    return QString::fromStdString(r.value());
}

bool ShowService::emptyDeletedShows()
{
    auto *lib = library();
    if (!lib) return false;
    if (auto r = lib->EmptyDeleted(); !r.ok()) {
        reportError(QStringLiteral("Couldn't empty the deleted shows"), message(r.error()));
        return false;
    }
    return true;
}

QString ShowService::newLibraryShowPath(
const QString &category, const QString &name)
{
    auto *lib = library();
    if (!lib) return {};
    auto r = lib->NewShowPath(category.toStdString(), name.toStdString());
    if (!r.ok()) {
        reportError(QStringLiteral("Couldn't prepare the show file"), message(r.error()));
        return {};
    }
    return QString::fromStdString(r.value());
}