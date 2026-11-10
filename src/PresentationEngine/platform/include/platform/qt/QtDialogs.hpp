#pragma once

#include "platform/IDialogs.hpp"

#include <functional>

namespace bps::platform {

// File pickers and message boxes through Qt's platform theme — the same native dialogs Qt Quick's own
// FileDialog uses (the XDG desktop portal / GTK / KDE dialog on Linux, NSOpenPanel on macOS), reached
// WITHOUT Qt Widgets or any helper program.
//
// WHY. On Linux every picker used to shell out to `zenity` (or `kdialog`): a machine without one had no
// way to choose a file, and the app imports everything — media, shows, Bibles, styles — through those
// pickers. The theme's dialog helper needs nothing installed.
//
// `fallback` (optional) answers when the theme has no native dialog for the request or declines to show
// it; Linux passes its zenity implementation there so no machine gets LESS than it had.
//
// Calls from any thread: the dialog is shown on the GUI thread (the caller waits).
class QtDialogs final : public IDialogs {
public:
    explicit QtDialogs(IDialogs* fallback = nullptr, std::function<bool()> fallbackSupported = {});

    // True when the theme has native dialogs, or the fallback can show them.
    bool Supported() const;

    Result<std::optional<std::string>> OpenFileDialog(std::string_view title,
                                                      const std::vector<std::string>& filters) override;
    Result<std::optional<std::string>> SaveFileDialog(std::string_view title,
                                                      std::string_view defaultName) override;
    Result<std::optional<std::string>> SelectFolderDialog(std::string_view title) override;
    Result<void> MessageDialog(std::string_view title, std::string_view message) override;
    Result<bool> QuestionDialog(std::string_view title, std::string_view message) override;

private:
    IDialogs* fallback_;
    std::function<bool()> fallbackSupported_;
};

} // namespace bps::platform
