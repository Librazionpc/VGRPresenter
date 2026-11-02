#pragma once

#include "core/common/Common.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bps::platform {
class IDialogs {
public:
    virtual ~IDialogs() = default;

    // File-open dialog. `filters` are "Display Name (*.ext;*.ext)" style
    // entries; empty = all files. nullopt = user cancelled.
    virtual Result<std::optional<std::string>> OpenFileDialog(
        std::string_view title, const std::vector<std::string>& filters) = 0;

    // File-save dialog (with overwrite confirmation where the backend offers
    // it). nullopt = user cancelled.
    virtual Result<std::optional<std::string>> SaveFileDialog(
        std::string_view title, std::string_view defaultName) = 0;

    // Folder picker. nullopt = user cancelled.
    virtual Result<std::optional<std::string>> SelectFolderDialog(
        std::string_view title) = 0;

    // Modal info/warning box. Returns Err::Unsupported when the desktop
    // session has no dialog provider.
    virtual Result<void> MessageDialog(std::string_view title,
                                       std::string_view message) = 0;

    // Modal yes/no question. Returns true = Yes / OK, false = No / Cancel.
    virtual Result<bool> QuestionDialog(std::string_view title,
                                        std::string_view message) = 0;

    virtual Result<std::optional<std::string>> ColorPicker(
        std::string_view title, std::string_view initialColor = "#ffffff") {
        (void)title; (void)initialColor;
        return Error::Make(Err::Unsupported, "Dialogs", "color picker not available");
    }

    virtual Result<std::optional<std::string>> FontPicker(
        std::string_view title, std::string_view initialFont = "") {
        (void)title; (void)initialFont;
        return Error::Make(Err::Unsupported, "Dialogs", "font picker not available");
    }
};
} // namespace bps::platform