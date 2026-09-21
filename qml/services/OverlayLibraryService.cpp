#include "services/OverlayLibraryService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "modules/overlays/OverlayLibrary.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QColor>
#include <QJSEngine>
#include <QQmlEngine>
#include <QRegularExpression>

namespace bo = bps::overlays;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

void report(const QString &message, const QString &level = QStringLiteral("error"))
{
    EventBus::instance().notify(message, level, QObject::tr("Overlays"), QStringLiteral("overlays.library"));
}

// The engine keeps colours the way FreeShow's CSS does ("#0b57a2", "dodgerblue", "rgba(0, 0, 0, 0.5)"). QML
// reads hex and names but not rgb()/rgba(), so those become "#aarrggbb" here; an empty colour stays empty (none).
QString qmlColor(const std::string &css)
{
    const QString c = qstr(css).trimmed();
    if (!c.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive))
        return c;
    const int open = c.indexOf(QLatin1Char('('));
    const int close = c.lastIndexOf(QLatin1Char(')'));
    if (open < 0 || close < open)
        return {};
    const QStringList parts = c.mid(open + 1, close - open - 1)
                                  .split(QRegularExpression(QStringLiteral("[,/\\s]+")), Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return {};
    auto channel = [](const QString &p) { return qBound(0, int(p.toDouble() + 0.5), 255); };
    QColor color(channel(parts[0]), channel(parts[1]), channel(parts[2]));
    color.setAlphaF(qBound(0.0, parts.size() > 3 ? parts[3].toDouble() : 1.0, 1.0));
    return color.name(QColor::HexArgb);
}

QString kindName(bo::OverlayElement::Kind kind)
{
    switch (kind) {
    case bo::OverlayElement::Kind::Text: return QStringLiteral("text");
    case bo::OverlayElement::Kind::Clock: return QStringLiteral("clock");
    case bo::OverlayElement::Kind::Vignette: return QStringLiteral("vignette");
    case bo::OverlayElement::Kind::Corners: return QStringLiteral("corners");
    case bo::OverlayElement::Kind::Box: break;
    }
    return QStringLiteral("box");
}

QVariantMap toMap(const bo::OverlayElement &e)
{
    return {
        { QStringLiteral("kind"), kindName(e.kind) },
        { QStringLiteral("x"), e.x },
        { QStringLiteral("y"), e.y },
        { QStringLiteral("width"), e.width },
        { QStringLiteral("height"), e.height },
        { QStringLiteral("background"), qmlColor(e.background) },
        { QStringLiteral("radius"), e.radius },
        { QStringLiteral("borderWidth"), e.borderWidth },
        { QStringLiteral("borderColor"), qmlColor(e.borderColor) },
        { QStringLiteral("text"), qstr(e.text) },
        { QStringLiteral("textColor"), qmlColor(e.textColor) },
        { QStringLiteral("fontSize"), e.fontSize },
        { QStringLiteral("bold"), e.bold },
        { QStringLiteral("uppercase"), e.uppercase },
        { QStringLiteral("align"), qstr(e.align) },
        { QStringLiteral("analog"), e.analog },
        { QStringLiteral("inset"), e.inset },
    };
}

QVariantMap toMap(const bo::Overlay &o)
{
    QVariantList elements;
    for (const bo::OverlayElement &e : o.elements)
        elements.append(toMap(e));
    return {
        { QStringLiteral("id"), qstr(o.id) },
        { QStringLiteral("name"), qstr(o.name) },
        { QStringLiteral("color"), qmlColor(o.color) },
        { QStringLiteral("category"), qstr(o.category) },
        { QStringLiteral("isDefault"), o.isDefault },
        { QStringLiteral("locked"), o.locked },
        { QStringLiteral("displayDuration"), o.displayDuration },
        { QStringLiteral("elements"), elements },
    };
}

} // namespace

OverlayLibraryService::OverlayLibraryService(QObject *parent)
    : QObject(parent)
{
    // The engine's platform layer (paths, files) exists once it has booted; the Overlays tab is only ever
    // created after that.
    if (!EngineBridge::instance().booted())
        return;
    auto &paths = bps::platform::PlatformAccessor::Get().Paths();
    auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    library_ = std::make_unique<bo::OverlayLibrary>(fs.Join(paths.UserDataDir(), "overlays.json"));
    if (auto loaded = library_->Load(); !loaded.ok())
        report(tr("The overlay library could not be read (%1). The default overlays are shown.")
                   .arg(qstr(loaded.error().message)));
}

OverlayLibraryService::~OverlayLibraryService() = default;

OverlayLibraryService &OverlayLibraryService::instance()
{
    static OverlayLibraryService s;
    return s;
}

OverlayLibraryService *OverlayLibraryService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

// ---------------------------------------------------------------------------
// Reading the engine's library
// ---------------------------------------------------------------------------

QVariantList OverlayLibraryService::categories() const
{
    QVariantList out;
    if (!library_)
        return out;
    const bo::OverlayCounts counts = library_->Counts();
    for (const bo::OverlayCategory &c : library_->Categories()) {
        const auto found = counts.byCategory.find(c.id);
        out.append(QVariantMap{
            { QStringLiteral("id"), qstr(c.id) },
            { QStringLiteral("name"), qstr(c.name) },
            { QStringLiteral("icon"), qstr(c.icon) },
            { QStringLiteral("isDefault"), c.isDefault },
            { QStringLiteral("count"), qulonglong(found == counts.byCategory.end() ? 0 : found->second) },
        });
    }
    return out;
}

int OverlayLibraryService::totalCount() const { return library_ ? int(library_->Counts().all) : 0; }

int OverlayLibraryService::unlabeledCount() const { return library_ ? int(library_->Counts().unlabeled) : 0; }

QString OverlayLibraryService::unlabeledFilter() const { return qstr(std::string(bo::kUnlabeled)); }

QStringList OverlayLibraryService::categoryIcons() const
{
    // Icons the sidebar can draw (IconGlyph).
    return { QStringLiteral("star"), QStringLiteral("folder"), QStringLiteral("info"), QStringLiteral("cash"),
             QStringLiteral("clock"), QStringLiteral("layers"), QStringLiteral("music"), QStringLiteral("camera") };
}

QVariantList OverlayLibraryService::overlays(const QString &filter, const QString &query) const
{
    QVariantList out;
    if (!library_)
        return out;
    for (const bo::Overlay &o : library_->Overlays(filter.toStdString(), query.toStdString()))
        out.append(toMap(o));
    return out;
}

QVariantMap OverlayLibraryService::overlay(const QString &id) const
{
    if (!library_)
        return {};
    const auto found = library_->Get(id.toStdString());
    return found.ok() ? toMap(found.value()) : QVariantMap{};
}

// ---------------------------------------------------------------------------
// Editing - the engine decides; a refusal becomes a toast
// ---------------------------------------------------------------------------

namespace {

// Runs an engine edit that returns Result<void>: tells the UI it changed, or says why it did not.
template <typename Edit>
bool apply(OverlayLibraryService *service, bo::OverlayLibrary *library, Edit &&edit)
{
    if (!library)
        return false;
    auto done = edit(*library);
    if (!done.ok()) {
        report(qstr(done.error().message));
        return false;
    }
    emit service->changed();
    return true;
}

} // namespace

QString OverlayLibraryService::createOverlay(const QString &name, const QString &categoryId)
{
    if (!library_)
        return {};
    auto made = library_->CreateOverlay(name.toStdString(), categoryId.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message));
        return {};
    }
    emit changed();
    return qstr(made.value().id);
}

QString OverlayLibraryService::duplicateOverlay(const QString &id)
{
    if (!library_)
        return {};
    auto made = library_->Duplicate(id.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message));
        return {};
    }
    emit changed();
    return qstr(made.value().id);
}

bool OverlayLibraryService::renameOverlay(const QString &id, const QString &name)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.Rename(id.toStdString(), name.toStdString()); });
}

bool OverlayLibraryService::setOverlayColor(const QString &id, const QString &color)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.SetColor(id.toStdString(), color.toStdString()); });
}

bool OverlayLibraryService::setOverlayCategory(const QString &id, const QString &categoryId)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.SetCategory(id.toStdString(), categoryId.toStdString()); });
}

bool OverlayLibraryService::setOverlayLocked(const QString &id, bool locked)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.SetLocked(id.toStdString(), locked); });
}

bool OverlayLibraryService::deleteOverlay(const QString &id)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.Delete(id.toStdString()); });
}

QString OverlayLibraryService::createCategory(const QString &name, const QString &icon)
{
    if (!library_)
        return {};
    auto made = library_->CreateCategory(name.toStdString(), icon.isEmpty() ? std::string("folder") : icon.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message), made.error().code == bps::Err::AlreadyExists ? QStringLiteral("info")
                                                                                          : QStringLiteral("error"));
        return {};
    }
    emit changed();
    return qstr(made.value().id);
}

bool OverlayLibraryService::renameCategory(const QString &id, const QString &name)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.RenameCategory(id.toStdString(), name.toStdString()); });
}

bool OverlayLibraryService::deleteCategory(const QString &id)
{
    return apply(this, library_.get(), [&](bo::OverlayLibrary &l) { return l.DeleteCategory(id.toStdString()); });
}

int OverlayLibraryService::restoreDefaults()
{
    if (!library_)
        return 0;
    auto restored = library_->RestoreDefaults();
    if (!restored.ok()) {
        report(qstr(restored.error().message));
        return 0;
    }
    if (restored.value() > 0)
        emit changed();
    return int(restored.value());
}
