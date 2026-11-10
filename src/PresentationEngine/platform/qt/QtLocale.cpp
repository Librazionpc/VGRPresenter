#include "platform/qt/QtServices.hpp"

#include <QLocale>
#include <QString>

namespace bps::platform {
namespace {
// "en-US", "zh-Hans-CN": the system locale as a BCP 47 tag.
QString Tag() {
    const QString tag = QLocale::system().bcp47Name();
    return tag.isEmpty() || tag == QLatin1String("C") ? QStringLiteral("en-US") : tag;
}
} // namespace

std::string QtLocale::Language() const {
    const QString tag = Tag();
    const int dash = tag.indexOf(QLatin1Char('-'));
    return (dash < 0 ? tag : tag.left(dash)).toStdString();
}

std::string QtLocale::Country() const {
    // The territory as its ISO 3166 code ("US"), whatever script sits between it and the language.
    const QLocale system = QLocale::system();
    if (system.territory() == QLocale::AnyTerritory)
        return {};
    return QLocale::territoryToCode(system.territory()).toStdString();
}

LocaleInfo QtLocale::Current() const {
    const QLocale system = QLocale::system();
    LocaleInfo info;
    info.language = Language();
    info.country = Country();
    info.dateFormat = system.dateFormat(QLocale::LongFormat).toStdString();
    info.timeFormat = system.timeFormat(QLocale::LongFormat).toStdString();
    info.currency = system.currencySymbol(QLocale::CurrencyIsoCode).toStdString();
    // "1,234.56" in this locale's own separators.
    info.numberFormat = (QStringLiteral("1") + system.groupSeparator() + QStringLiteral("234")
                         + system.decimalPoint() + QStringLiteral("56")).toStdString();
    if (info.dateFormat.empty()) info.dateFormat = "MM/dd/yyyy";
    if (info.timeFormat.empty()) info.timeFormat = "HH:mm:ss";
    info.rtl = system.textDirection() == Qt::RightToLeft;
    return info;
}

} // namespace bps::platform
