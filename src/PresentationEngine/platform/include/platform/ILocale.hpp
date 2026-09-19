#pragma once

// PAL locale subsystem (Phase 2 DoD §15): language/country identity and the
// formatting conventions the UI layer will need. Best-effort derivation from
// the OS locale on Linux; Windows/macOS backends follow.

#include <string>

namespace bps::platform {

struct LocaleInfo {
    std::string language;     // ISO 639-1, e.g. "en"
    std::string country;      // ISO 3166-1 alpha-2, e.g. "US"
    std::string dateFormat;   // e.g. "MM/dd/yyyy"
    std::string timeFormat;   // e.g. "HH:mm:ss"
    std::string numberFormat; // e.g. "1,234.56"
    std::string currency;     // ISO 4217, e.g. "USD"
    bool rtl = false;         // right-to-left script (ar/he/fa/...)
};

class ILocale {
public:
    virtual ~ILocale() = default;
    virtual LocaleInfo Current() const = 0;
    virtual std::string Language() const = 0;
    virtual std::string Country() const = 0;
};

} // namespace bps::platform
