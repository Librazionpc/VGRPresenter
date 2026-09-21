#include "services/TextFormatService.h"

#include "modules/presentation/TextFormat.hpp"

#include <QJSEngine>
#include <QQmlEngine>
#include <QVariantMap>

TextFormatService &TextFormatService::instance()
{
    static TextFormatService s;
    return s;
}

TextFormatService *TextFormatService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

QString TextFormatService::applyList(const QString &text, const QString &style) const
{
    if (style.isEmpty() || style == QLatin1String("none") || text.isEmpty())
        return text;
    return QString::fromStdString(bps::presentation::ApplyList(text.toStdString(), style.toStdString()));
}

QVariantList TextFormatService::listStyles() const
{
    QVariantList out;
    for (const bps::presentation::ListStyle &s : bps::presentation::ListStyles())
        out.append(QVariantMap{
            { QStringLiteral("key"), QString::fromStdString(s.key) },
            { QStringLiteral("label"), QString::fromStdString(s.label) },
            { QStringLiteral("sample"), QString::fromStdString(s.sample) },
        });
    return out;
}
