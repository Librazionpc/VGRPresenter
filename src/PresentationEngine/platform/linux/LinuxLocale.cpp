#include "platform/linux/LinuxLocale.hpp"

#include <cstdlib>

namespace bps::platform {

std::pair<std::string, std::string> LinuxLocale::LanguageCountry() {
    // LANG="de_DE.UTF-8" -> ("de", "DE"); falls back to LC_ALL / LC_MESSAGES.
    const char* raw = std::getenv("LC_ALL");
    if (!raw || !*raw) raw = std::getenv("LANG");
    if (!raw || !*raw) raw = std::getenv("LC_MESSAGES");
    std::string tag = raw ? raw : "en_US";
    size_t dot = tag.find('.');
    if (dot != std::string::npos) tag = tag.substr(0, dot);
    std::string lang = tag.substr(0, 2);
    std::string country;
    if (tag.size() >= 5 && tag[2] == '_') country = tag.substr(3, 2);
    return {lang, country};
}

std::string LinuxLocale::Language() const {
    return LanguageCountry().first;
}

std::string LinuxLocale::Country() const {
    return LanguageCountry().second;
}

LocaleInfo LinuxLocale::Current() const {
    LocaleInfo info;
    auto [lang, country] = LanguageCountry();
    info.language = lang.empty() ? "en" : lang;
    info.country = country;

    // Small convention table for common locales; sensible defaults elsewhere.
    // These are display hints — the UI layer owns the real formatting.
    if (lang == "de") {
        info.dateFormat = "dd.MM.yyyy";
        info.numberFormat = "1.234,56";
        info.currency = "EUR";
    } else if (lang == "fr") {
        info.dateFormat = "dd/MM/yyyy";
        info.numberFormat = "1 234,56";
        info.currency = "EUR";
    } else if (lang == "ja") {
        info.dateFormat = "yyyy/MM/dd";
        info.numberFormat = "1,234.56";
        info.currency = "JPY";
    } else {
        info.dateFormat = "MM/dd/yyyy";
        info.numberFormat = "1,234.56";
        info.currency = "USD";
    }
    info.timeFormat = "HH:mm:ss";
    info.rtl = (lang == "ar" || lang == "he" || lang == "fa" || lang == "ur");
    return info;
}

} // namespace bps::platform
