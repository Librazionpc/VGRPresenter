#include "services/FontService.h"

#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <QFontDatabase>
#include <QString>

namespace {

constexpr const char *kDefaultKey = "\x01default";

const bps::rendering::FontManager &fonts()
{
    return bps::rendering::RenderEngine::Instance().Fonts();
}

} // namespace

FontService &FontService::instance()
{
    static FontService service;
    return service;
}

FontService *FontService::create(QQmlEngine *, QJSEngine *)
{
    // QML owns the singleton's lifetime through this pointer; the object is the
    // process-wide one so C++ callers and every QML surface see one cache.
    return &instance();
}

FontService::FontService(QObject *parent)
    : QObject(parent)
{
}

QString FontService::defaultFamily() const
{
    const QString key = QString::fromLatin1(kDefaultKey);
    if (const auto it = cache_.constFind(key); it != cache_.constEnd())
        return it.value();
    // The engine's answer, not a name of ours: "builtin" only when the platform
    // cannot resolve a system font at all (off Windows, or a broken GDI+).
    const QString answer = QString::fromStdString(fonts().DefaultFamily());
    cache_.insert(key, answer);
    return answer;
}

QString FontService::monoFamily() const
{
    const QString key = QStringLiteral("\x01mono");
    if (const auto it = cache_.constFind(key); it != cache_.constEnd())
        return it.value();
    // Qt's own answer for "the platform's fixed-width font"; empty only if the
    // platform has none, in which case the caller keeps Qt's default.
    const QString answer =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    cache_.insert(key, answer);
    return answer;
}

QString FontService::resolve(const QString &requested) const
{
    const QString family = requested.trimmed();
    if (family.isEmpty())
        return defaultFamily();
    if (const auto it = cache_.constFind(family); it != cache_.constEnd())
        return it.value();
    // FontManager::SubstituteFamily: requested when installed, else the host's
    // UI face, else "" — the caller keeps the requested name in that last case
    // (there is no substitute, and Qt will draw *something*).
    const std::string resolved = fonts().SubstituteFamily(family.toStdString());
    const QString answer = resolved.empty() ? family : QString::fromStdString(resolved);
    cache_.insert(family, answer);
    return answer;
}
