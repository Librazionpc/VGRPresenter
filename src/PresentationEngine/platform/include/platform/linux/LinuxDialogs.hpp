#pragma once

#include "../IDialogs.hpp"

namespace bps::platform {
class LinuxDialogs final : public IDialogs {
public:
    bool Supported() const;
    Result<std::optional<std::string>> OpenFileDialog(
        std::string_view title, const std::vector<std::string>& filters) override;
    Result<std::optional<std::string>> SaveFileDialog(
        std::string_view title, std::string_view defaultName) override;
    Result<std::optional<std::string>> SelectFolderDialog(
        std::string_view title) override;
    Result<void> MessageDialog(std::string_view title, std::string_view message) override;
    Result<bool> QuestionDialog(std::string_view title, std::string_view message) override;
    Result<std::optional<std::string>> ColorPicker(
        std::string_view title, std::string_view initialColor = "#ffffff") override;
    Result<std::optional<std::string>> FontPicker(
        std::string_view title, std::string_view initialFont = "") override;
};
} // namespace bps::platform
