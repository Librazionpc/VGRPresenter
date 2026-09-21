#include "services/DesignLibraryService.h"

#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/ShowConverter.h"

#include "modules/library/DesignCatalogs.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QJSEngine>
#include <QQmlEngine>

namespace bl = bps::library;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

} // namespace

// ---------------------------------------------------------------------------
// DesignLibraryService - the shared engine window (the subclasses are the QML singletons)
// ---------------------------------------------------------------------------

DesignLibraryService::DesignLibraryService(QObject *parent)
    : QObject(parent)
{
}

DesignLibraryService::~DesignLibraryService() = default;

void DesignLibraryService::open(bool forTemplates, QString noun, QString toastChannel)
{
    noun_ = std::move(noun);
    toastChannel_ = std::move(toastChannel);

    // The library's file path needs the engine's platform layer. Boot may already have run
    // (tests, reload) or may still be pending (the singletons are created when QML first
    // touches them, which can be before boot) — either way the library is built the moment
    // the platform layer exists. Without the retry a service created pre-boot stayed empty
    // for the whole session, the same gap ShowService had.
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this,
            [this, forTemplates] {
        if (EngineBridge::instance().booted())
            loadLibrary(forTemplates);
    });
    if (EngineBridge::instance().booted())
        loadLibrary(forTemplates);
}

void DesignLibraryService::loadLibrary(bool forTemplates)
{
    if (library_ || !EngineBridge::instance().booted())
        return;
    auto &platform = bps::platform::PlatformAccessor::Get();
    const std::string file = platform.Filesystem().Join(platform.Paths().UserDataDir(),
                                                        forTemplates ? "templates.json" : "overlays.json");
    library_ = forTemplates ? bl::MakeTemplateLibrary(file) : bl::MakeOverlayLibrary(file);
    if (auto loaded = library_->Load(); !loaded.ok())
        report(tr("The %1 library could not be read (%2). What ships is shown.").arg(noun_, qstr(loaded.error().message)));
    emit changed();   // everything bound to the empty pre-boot state re-reads
}

void DesignLibraryService::report(const QString &message, const QString &level) const
{
    const QString title = noun_.isEmpty() ? noun_ : noun_.at(0).toUpper() + noun_.mid(1);
    EventBus::instance().notify(message, level, title, toastChannel_);
}

DesignLibraryService &DesignLibraryService::overlays() { return OverlayLibraryService::instance(); }

DesignLibraryService &DesignLibraryService::templates() { return TemplateLibraryService::instance(); }

QString DesignLibraryService::noun() const { return noun_; }

QStringList DesignLibraryService::extraBlockKinds() const
{
    QStringList out;
    if (library_)
        for (const std::string &kind : library_->Config().extraBlockKinds)
            out.append(qstr(kind));
    return out;
}

QVariantList DesignLibraryService::categories() const
{
    QVariantList out;
    if (!library_)
        return out;
    const bl::DesignCounts counts = library_->Counts();
    for (const bl::DesignCategory &c : library_->Categories()) {
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

int DesignLibraryService::totalCount() const { return library_ ? int(library_->Counts().all) : 0; }

int DesignLibraryService::unlabeledCount() const { return library_ ? int(library_->Counts().unlabeled) : 0; }

QString DesignLibraryService::unlabeledFilter() const { return qstr(std::string(bl::kUnlabeled)); }

QStringList DesignLibraryService::categoryIcons() const
{
    // Icons the sidebar can draw (IconGlyph).
    return { QStringLiteral("star"), QStringLiteral("folder"), QStringLiteral("info"), QStringLiteral("cash"),
             QStringLiteral("clock"), QStringLiteral("layers"), QStringLiteral("music"), QStringLiteral("camera") };
}

QVariantList DesignLibraryService::designs(const QString &filter, const QString &query) const
{
    QVariantList out;
    if (!library_)
        return out;
    for (const bl::Design &d : library_->Designs(filter.toStdString(), query.toStdString()))
        out.append(toMap(d));
    return out;
}

QVariantMap DesignLibraryService::design(const QString &id) const
{
    if (!library_)
        return {};
    const auto found = library_->Get(id.toStdString());
    return found.ok() ? toMap(found.value()) : QVariantMap{};
}

QVariantMap DesignLibraryService::toMap(const bl::Design &d)
{
    QVariantList blocks;
    for (const bps::presentation::ContentBlock &b : d.blocks)
        blocks.append(ShowConverter::blockToVariant(b));
    return {
        { QStringLiteral("id"), qstr(d.id) },
        { QStringLiteral("name"), qstr(d.name) },
        { QStringLiteral("color"), qstr(d.color) },
        { QStringLiteral("category"), qstr(d.category) },
        { QStringLiteral("contentType"), qstr(d.contentType) },
        { QStringLiteral("isDefault"), d.isDefault },
        { QStringLiteral("locked"), d.locked },
        { QStringLiteral("placeUnderSlide"), d.placeUnderSlide },
        { QStringLiteral("displayDuration"), d.displayDuration },
        { QStringLiteral("background"), qstr(d.background) },
        { QStringLiteral("blocks"), blocks },
    };
}

// ---------------------------------------------------------------------------
// Editing - the engine decides; a refusal becomes a toast
// ---------------------------------------------------------------------------

template <typename Edit>
bool DesignLibraryService::apply(Edit &&edit)
{
    if (!library_)
        return false;
    auto done = edit(*library_);
    if (!done.ok()) {
        report(qstr(done.error().message));
        return false;
    }
    emit changed();
    return true;
}

QString DesignLibraryService::createDesign(const QString &name, const QString &categoryId)
{
    if (!library_)
        return {};
    auto made = library_->Create(name.toStdString(), categoryId.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message));
        return {};
    }
    emit changed();
    return qstr(made.value().id);
}

QString DesignLibraryService::duplicateDesign(const QString &id)
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

bool DesignLibraryService::renameDesign(const QString &id, const QString &name)
{
    return apply([&](bl::DesignLibrary &l) { return l.Rename(id.toStdString(), name.toStdString()); });
}

bool DesignLibraryService::setDesignColor(const QString &id, const QString &color)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetColor(id.toStdString(), color.toStdString()); });
}

bool DesignLibraryService::setDesignCategory(const QString &id, const QString &categoryId)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetCategory(id.toStdString(), categoryId.toStdString()); });
}

bool DesignLibraryService::setDesignContentType(const QString &id, const QString &contentType)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetContentType(id.toStdString(), contentType.toStdString()); });
}

bool DesignLibraryService::setDesignLocked(const QString &id, bool locked)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetLocked(id.toStdString(), locked); });
}

bool DesignLibraryService::setDesignPlaceUnderSlide(const QString &id, bool under)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetPlaceUnderSlide(id.toStdString(), under); });
}

bool DesignLibraryService::setDesignDuration(const QString &id, double seconds)
{
    return apply([&](bl::DesignLibrary &l) { return l.SetDisplayDuration(id.toStdString(), seconds); });
}

bool DesignLibraryService::deleteDesign(const QString &id)
{
    return apply([&](bl::DesignLibrary &l) { return l.Delete(id.toStdString()); });
}

QString DesignLibraryService::createCategory(const QString &name, const QString &icon)
{
    if (!library_)
        return {};
    auto made = library_->CreateCategory(name.toStdString(), icon.isEmpty() ? std::string("folder") : icon.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message),
               made.error().code == bps::Err::AlreadyExists ? QStringLiteral("info") : QStringLiteral("error"));
        return {};
    }
    emit changed();
    return qstr(made.value().id);
}

bool DesignLibraryService::renameCategory(const QString &id, const QString &name)
{
    return apply([&](bl::DesignLibrary &l) { return l.RenameCategory(id.toStdString(), name.toStdString()); });
}

bool DesignLibraryService::deleteCategory(const QString &id)
{
    return apply([&](bl::DesignLibrary &l) { return l.DeleteCategory(id.toStdString()); });
}

int DesignLibraryService::restoreDefaults()
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

// ---------------------------------------------------------------------------
// Content - what the Edit screen's canvas flushes through
// ---------------------------------------------------------------------------

bool DesignLibraryService::setDesignBlocks(const QString &id, const QString &background, const QVariantList &blocks)
{
    if (!library_)
        return false;
    std::vector<bps::presentation::ContentBlock> converted;
    for (const QVariant &bv : blocks)
        converted.push_back(ShowConverter::blockFromVariant(bv.toMap()));
    return apply([&](bl::DesignLibrary &l) {
        return l.SetContent(id.toStdString(), background.toStdString(), std::move(converted));
    });
}

QString DesignLibraryService::addBlock(const QString &id, const QVariantMap &block, const QString &above)
{
    if (!library_)
        return {};
    auto made = library_->AddBlock(id.toStdString(),
                                   ShowConverter::blockFromVariant(block), above.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message));
        return {};
    }
    emit changed();
    return qstr(made.value());
}

QString DesignLibraryService::duplicateBlock(const QString &id, const QString &blockKey)
{
    if (!library_)
        return {};
    auto made = library_->DuplicateBlock(id.toStdString(), blockKey.toStdString());
    if (!made.ok()) {
        report(qstr(made.error().message));
        return {};
    }
    emit changed();
    return qstr(made.value());
}

bool DesignLibraryService::removeBlock(const QString &id, const QString &blockKey)
{
    return apply([&](bl::DesignLibrary &l) { return l.RemoveBlock(id.toStdString(), blockKey.toStdString()); });
}

QVariantMap DesignLibraryService::block(const QString &id, const QString &blockKey) const
{
    if (!library_)
        return {};
    const auto found = library_->Block(id.toStdString(), blockKey.toStdString());
    return found.ok() ? ShowConverter::blockToVariant(found.value()) : QVariantMap{};
}

// ---------------------------------------------------------------------------
// The QML singletons
// ---------------------------------------------------------------------------

OverlayLibraryService::OverlayLibraryService()
{
    open(false, tr("overlay"), QStringLiteral("overlays.library"));
}

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

TemplateLibraryService::TemplateLibraryService()
{
    open(true, tr("template"), QStringLiteral("templates.library"));
}

TemplateLibraryService &TemplateLibraryService::instance()
{
    static TemplateLibraryService s;
    return s;
}

TemplateLibraryService *TemplateLibraryService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}
