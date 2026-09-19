#pragma once

// Linux PAL backend for the locale subsystem (DoD §15): derives identity and
// formatting conventions from the LANG/LC_* environment.

#include "../ILocale.hpp"

namespace bps::platform {

class LinuxLocale final : public ILocale {
public:
    LocaleInfo Current() const override;
    std::string Language() const override;
    std::string Country() const override;

private:
    static std::pair<std::string, std::string> LanguageCountry();
};

} // namespace bps::platform
