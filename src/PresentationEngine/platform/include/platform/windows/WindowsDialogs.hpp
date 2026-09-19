#pragma once

// Windows PAL backend for native dialogs: Common Item Dialog (IFileOpenDialog /
// IFileSaveDialog) for file/folder pickers, MessageBoxW for message boxes.
// COM is initialized per call on the calling thread (COINIT_APARTMENTTHREADED)
// and uninitialized on return.

#include "../IDialogs.hpp"

namespace bps::platform {

class WindowsDialogs final : public IDialogs {
public:
    Result<std::optional<std::string>> OpenFileDialog(
        std::string_view title, const std::vector<std::string>& filters) override;
    Result<std::optional<std::string>> SaveFileDialog(
        std::string_view title, std::string_view defaultName) override;
    Result<std::optional<std::string>> SelectFolderDialog(
        std::string_view title) override;
    Result<void> MessageDialog(std::string_view title, std::string_view message) override;
    Result<bool> QuestionDialog(std::string_view title, std::string_view message) override;
};

} // namespace bps::platform
