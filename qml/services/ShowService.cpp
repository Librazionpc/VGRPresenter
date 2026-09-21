#include "services/ShowService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/ShowConverter.h"

#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/ShowEditor.hpp"
#include "modules/presentation/ShowLibrary.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QDir>
#include <QJSEngine>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

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

ShowService::ShowService(QObject *parent)
    : QObject(parent)
{
    // Library root is chosen up front; the folder itself is only created once the
    // engine is up and the library is first used. VGR_LIBRARY_DIR overrides it
    // (tests, portable installs).
    const QString dirOverride = qEnvironmentVariable("VGR_LIBRARY_DIR");
    libraryPath_ = !dirOverride.isEmpty()
        ? dirOverride
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + QStringLiteral("/VGR Presenter/Shows");

    // The library can only be read once the engine's platform layer exists
    // (see library()) — so the first refresh fires on engine boot. The Show
    // screen's categories/shows bind to these properties and would otherwise
    // stay empty until the user's first save.
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

bool ShowService::saveShowFile(const QVariantMap &show, const QString &path)
{
    auto doc = showDocument();
    if (!doc) {
        reportError(QStringLiteral("Save show"),
                    QStringLiteral("The engine isn't running, so the show can't be saved."));
        return false;
    }
    // Keep the document's identity and creation time across saves: only the
    // content comes from the UI.
    bp::Presentation incoming = ShowConverter::fromVariant(show);
    const bp::Presentation current = doc->Snapshot();
    if (incoming.id.empty()) incoming.id = current.id;
    if (incoming.name.empty()) incoming.name = current.name;
    incoming.createdAt = current.createdAt;
    doc->Replace(std::move(incoming));

    auto saved = doc->Save(vgrPath(path));
    emit showChanged();
    if (!saved.ok()) {
        reportError(QStringLiteral("Couldn't save the show"), message(saved.error()));
        return false;
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("Saved '%1' to %2").arg(QString::fromStdString(doc->Snapshot().name), QString::fromStdString(doc->Path())));
    EventBus::instance().notify(QStringLiteral("Saved to %1").arg(QString::fromStdString(doc->Path())),
                                QStringLiteral("success"), QStringLiteral("Show saved"),
                                QStringLiteral("show.saved"));
    // Only rescan when the file landed inside the library — a scan reads every
    // show in it, which is wasted work for a save somewhere else.
    const QString saved_ = QDir::fromNativeSeparators(QString::fromStdString(doc->Path()));
    if (library_ && saved_.startsWith(QDir::fromNativeSeparators(libraryPath_), Qt::CaseInsensitive))
        refreshLibrary();
    return true;
}

bool ShowService::saveCurrentShow(const QString &path, bool quiet)
{
    auto doc = showDocument();
    if (!doc || !doc->HasDocument()) {
        reportError(QStringLiteral("Save show"), QStringLiteral("There is no show to save."));
        return false;
    }
    auto saved = doc->Save(path.isEmpty() ? std::string() : vgrPath(path));
    emit showChanged();
    if (!saved.ok()) {
        reportError(QStringLiteral("Couldn't save the show"), message(saved.error()));
        return false;
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("%1 '%2' to %3").arg(quiet ? QStringLiteral("Auto-saved") : QStringLiteral("Saved"), QString::fromStdString(doc->Snapshot().name), QString::fromStdString(doc->Path())));
    if (!quiet)
        EventBus::instance().notify(QStringLiteral("Saved to %1").arg(QString::fromStdString(doc->Path())),
                                    QStringLiteral("success"), QStringLiteral("Show saved"),
                                    QStringLiteral("show.saved"));
    const QString saved_ = QDir::fromNativeSeparators(QString::fromStdString(doc->Path()));
    if (library_ && saved_.startsWith(QDir::fromNativeSeparators(libraryPath_), Qt::CaseInsensitive))
        refreshLibrary();
    return true;
}

bool ShowService::saveShowCopy(const QString &path)
{
    auto doc = showDocument();
    if (!doc || !doc->HasDocument() || path.isEmpty())
        return false;
    return doc->SaveCopy(vgrPath(path)).ok();
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
        "Open show", { "VGR shows (*.vgr)", "All files (*.*)" });
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
    // The show that is open in the editor is read as it is there (unsaved edits included), not from the older copy on disk.
    if (auto open = showDocument(); open && open->HasDocument() && !path.isEmpty()
        && QDir::fromNativeSeparators(QString::fromStdString(open->Path())).compare(QDir::fromNativeSeparators(path), Qt::CaseInsensitive) == 0)
        return { { QStringLiteral("ok"), true }, { QStringLiteral("error"), QString() }, { QStringLiteral("show"), ShowConverter::toVariant(open->Snapshot()) } };
    bp::PresentationDocument peek;
    auto opened = peek.Open(vgrPath(path));
    if (!opened.ok())
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), message(opened.error()) } };
    return { { QStringLiteral("ok"), true }, { QStringLiteral("error"), QString() }, { QStringLiteral("show"), ShowConverter::toVariant(peek.Snapshot()) } };
}

bool ShowService::editShow(const QString &path, const QString &title, const std::function<bps::Result<void>(bp::Presentation &)> &change)
{
    if (!EngineBridge::instance().booted() || path.isEmpty())
        return false;
    const QString wanted = QDir::fromNativeSeparators(path);

    // The show that is open in the editor takes it into the open document (unsaved, like any other edit).
    auto open = showDocument();
    if (open && open->HasDocument() && QDir::fromNativeSeparators(QString::fromStdString(open->Path())).compare(wanted, Qt::CaseInsensitive) == 0)
        return applyEdit(title, change);

    bp::PresentationDocument file;
    if (auto opened = file.Open(vgrPath(path)); !opened.ok()) {
        reportError(title, message(opened.error()));
        return false;
    }
    bp::Presentation show = file.Snapshot();
    if (auto changed = change(show); !changed.ok()) {
        reportError(title, message(changed.error()));
        return false;
    }
    file.Replace(std::move(show));
    if (auto saved = file.Save(vgrPath(path)); !saved.ok()) {
        reportError(title, message(saved.error()));
        return false;
    }
    if (library_ && wanted.startsWith(QDir::fromNativeSeparators(libraryPath_), Qt::CaseInsensitive))
        refreshLibrary();
    return true;
}

bool ShowService::setNextTimer(const QString &path, double seconds)
{
    const bool done = editShow(path, QStringLiteral("Couldn't set the next timer"), [&](bp::Presentation &s) { return bp::ShowEditor::SetNextTimer(s, seconds); });
    if (done)
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("Next timer %1 s on %2").arg(seconds).arg(path));
    return done;
}

QVariantMap ShowService::editSlides(const QString &path, const QString &op, const QStringList &slideIds, const QVariantMap &args)
{
    using Editor = bp::ShowEditor;
    std::vector<std::string> ids;
    for (const QString &id : slideIds) ids.push_back(id.toStdString());
    const QString title = QStringLiteral("Couldn't change the slides");
    QStringList made;
    size_t replaced = 0;
    bool ok = false;

    if (op == QLatin1String("hidden")) {
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::SetSlidesHidden(s, ids, args.value(QStringLiteral("hidden")).toBool()); });
    } else if (op == QLatin1String("group")) {
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::SetSlidesGroup(s, ids, args.value(QStringLiteral("group")).toString().toStdString()); });
    } else if (op == QLatin1String("title")) {
        ok = editShow(path, title, [&](bp::Presentation &s) -> bps::Result<void> {
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
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::FormatSlidesText(s, ids, format); });
    } else if (op == QLatin1String("replace")) {
        ok = editShow(path, title, [&](bp::Presentation &s) -> bps::Result<void> {
            auto r = Editor::ReplaceInSlides(s, ids, args.value(QStringLiteral("find")).toString().toStdString(), args.value(QStringLiteral("replace")).toString().toStdString(),
                                             args.value(QStringLiteral("caseSensitive")).toBool());
            if (!r.ok()) return r.error();
            replaced = r.value();
            return bps::Ok();
        });
    } else if (op == QLatin1String("split")) {
        ok = editShow(path, title, [&](bp::Presentation &s) -> bps::Result<void> {
            auto r = Editor::SplitSlidesInHalf(s, ids);
            if (!r.ok()) return r.error();
            for (const std::string &id : r.value()) made.append(QString::fromStdString(id));
            return bps::Ok();
        });
    } else if (op == QLatin1String("merge")) {
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::MergeSlides(s, ids); });
    } else if (op == QLatin1String("duplicate")) {
        ok = editShow(path, title, [&](bp::Presentation &s) -> bps::Result<void> {
            if (ids.empty()) return bps::Error::Make(bps::Err::InvalidArgument, "ShowService", "no slide was chosen");
            for (const std::string &id : ids) {
                auto r = Editor::DuplicateSlide(s, id);
                if (!r.ok()) return r.error();
                made.append(QString::fromStdString(r.value()));
            }
            return bps::Ok();
        });
    } else if (op == QLatin1String("remove")) {
        ok = editShow(path, title, [&](bp::Presentation &s) -> bps::Result<void> {
            if (ids.empty()) return bps::Error::Make(bps::Err::InvalidArgument, "ShowService", "no slide was chosen");
            for (const std::string &id : ids)
                if (auto r = Editor::RemoveSlide(s, id); !r.ok()) return r;
            return bps::Ok();
        });
    } else if (op == QLatin1String("transition")) {
        static const QStringList kinds = { QStringLiteral("fade"), QStringLiteral("slide"), QStringLiteral("push"), QStringLiteral("zoom"), QStringLiteral("wipe"), QStringLiteral("crossfade"), QStringLiteral("custom") };
        const int kind = std::max(0, static_cast<int>(kinds.indexOf(args.value(QStringLiteral("kind")).toString())));
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::SetSlidesTransition(s, ids, static_cast<bp::TransitionKind>(kind), args.value(QStringLiteral("ms"), 500).toDouble()); });
    } else if (op == QLatin1String("outputs")) {
        std::vector<std::string> outputs;
        for (const QVariant &o : args.value(QStringLiteral("outputs")).toList()) outputs.push_back(o.toString().toStdString());
        ok = editShow(path, title, [&](bp::Presentation &s) { return Editor::SetSlidesOutputs(s, ids, outputs); });
    } else if (op == QLatin1String("timer")) {
        ok = editShow(path, QStringLiteral("Couldn't set the next timer"), [&](bp::Presentation &s) { return Editor::SetNextTimer(s, args.value(QStringLiteral("seconds")).toDouble(), ids); });
    } else {
        reportError(title, QStringLiteral("There is no slide operation '%1'.").arg(op));
    }

    if (ok)
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("Slide %1 on %2 slide(s) of %3").arg(op).arg(slideIds.size()).arg(path));
    return { { QStringLiteral("ok"), ok }, { QStringLiteral("made"), made }, { QStringLiteral("replaced"), static_cast<int>(replaced) } };
}

QVariantMap ShowService::openShowFile(const QString &path)
{

    auto doc = showDocument();
    if (!doc)
        return { { QStringLiteral("ok"), false },
                 { QStringLiteral("error"), QStringLiteral("The engine isn't running.") } };
    auto opened = doc->Open(vgrPath(path));
    if (!opened.ok()) {
        const QString why = message(opened.error());
        reportError(QStringLiteral("Couldn't open the show"), why);
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), why } };
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Show"), QStringLiteral("Opened '%1' from %2").arg(QString::fromStdString(doc->Snapshot().name), path));
    emit showChanged();
    return { { QStringLiteral("ok"), true },
             { QStringLiteral("error"), QString() },
             { QStringLiteral("show"), ShowConverter::toVariant(doc->Snapshot()) } };
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
// ===========================================================================

bool ShowService::applyEdit(const QString &title,
                            const std::function<bps::Result<void>(bp::Presentation &)> &fn)
{
    auto doc = showDocument();
    if (!doc || !doc->HasDocument()) {
        reportError(title, QStringLiteral("No show is open."));
        return false;
    }
    if (auto r = doc->Edit(fn); !r.ok()) {
        reportError(title, message(r.error()));
        return false;
    }
    emit showChanged();
    return true;
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

bool ShowService::saveTemplateFile(const QVariantMap &tmpl, const QString &path)
{
    auto saved = bp::TemplateFile::Save(ShowConverter::templateFromVariant(tmpl), vgrPath(path));
    if (!saved.ok()) {
        reportError(QStringLiteral("Couldn't save the template"), message(saved.error()));
        return false;
    }
    return true;
}

QVariantMap ShowService::loadTemplateFile(const QString &path)
{
    auto loaded = bp::TemplateFile::Load(vgrPath(path));
    if (!loaded.ok()) {
        const QString why = message(loaded.error());
        reportError(QStringLiteral("Couldn't open the template"), why);
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), why } };
    }
    return { { QStringLiteral("ok"), true },
             { QStringLiteral("error"), QString() },
             { QStringLiteral("template"), ShowConverter::templateToVariant(loaded.value()) } };
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
        library_ = std::make_unique<bp::ShowLibrary>(libraryPath_.toStdString());
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
    if (auto *lib = library()) {
        if (auto r = lib->Refresh(); !r.ok())
            reportError(QStringLiteral("Couldn't read the show library"), message(r.error()));
        publishLibrary();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Library"), QStringLiteral("Show library scanned: %1 show(s) in %2").arg(libraryShows().size()).arg(libraryPath_));
    }
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
