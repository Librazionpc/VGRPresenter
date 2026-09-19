#pragma once

// Windows PAL backend for the locale subsystem (GetUserDefaultLocaleName +
// GetLocaleInfoEx).

#include "platform/ILocale.hpp"

namespace bps::platform {

class WindowsLocale final : public ILocale {
public:
    LocaleInfo Current() const override;
    std::string Language() const override;
    std::string Country() const override;
};

} // namespace bps::platform
