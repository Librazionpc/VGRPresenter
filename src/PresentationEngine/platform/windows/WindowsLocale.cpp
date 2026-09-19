#include "platform/windows/WindowsLocale.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>

namespace bps::platform {

namespace {
std::string LocaleName() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {0};
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) == 0) return "en-US";
    return win::Utf8(buf);
}

std::string LocaleInfoString(LCID lcid, LCTYPE lctype) {
    wchar_t buf[256] = {0};
    if (GetLocaleInfoW(lcid, lctype, buf, 256) == 0) return {};
    return win::Utf8(buf);
}
} // namespace

std::string WindowsLocale::Language() const {
    std::string name = LocaleName();
    size_t dash = name.find('-');
    return dash == std::string::npos ? name : name.substr(0, dash);
}

std::string WindowsLocale::Country() const {
    std::string name = LocaleName();
    size_t dash = name.find('-');
    if (dash == std::string::npos) return {};
    std::string c = name.substr(dash + 1);
    // "en-US" -> "US"; "zh-Hans-CN" -> "CN" (last segment).
    size_t dash2 = c.find('-');
    return dash2 == std::string::npos ? c : c.substr(dash2 + 1);
}

LocaleInfo WindowsLocale::Current() const {
    LocaleInfo info;
    std::string name = LocaleName();
    info.language = Language();
    info.country = Country();

    LCID lcid = LocaleNameToLCID(win::Wide(name).c_str(), 0);
    if (lcid == 0) lcid = LOCALE_USER_DEFAULT;
    info.dateFormat = LocaleInfoString(lcid, LOCALE_SLONGDATE);
    info.timeFormat = LocaleInfoString(lcid, LOCALE_STIMEFORMAT);
    info.currency = LocaleInfoString(lcid, LOCALE_SINTLSYMBOL);
    std::string dec = LocaleInfoString(lcid, LOCALE_SDECIMAL);
    std::string thou = LocaleInfoString(lcid, LOCALE_STHOUSAND);
    std::string digits = LocaleInfoString(lcid, LOCALE_SGROUPING);
    info.numberFormat = "1" + thou + "234" + dec + "56";   // "1,234.56" shaped
    if (info.numberFormat.empty()) info.numberFormat = "1,234.56";
    if (info.dateFormat.empty()) info.dateFormat = "MM/dd/yyyy";
    if (info.timeFormat.empty()) info.timeFormat = "HH:mm:ss";

    // RTL: reading layout 1 = right-to-left.
    std::string layout = LocaleInfoString(lcid, LOCALE_IREADINGLAYOUT);
    info.rtl = (layout == "1");
    return info;
}

} // namespace bps::platform
