// QtDialogs — native file pickers and message boxes from Qt's platform theme (see QtDialogs.hpp).
//
// The theme (QPlatformTheme) hands out a QPlatformFileDialogHelper / QPlatformMessageDialogHelper for the
// session's real dialog: on Linux the xdg-desktop-portal, GTK or KDE one, on macOS NSOpenPanel. This is
// exactly what Qt Quick Dialogs does under its QML FileDialog; using it from C++ means the engine's
// synchronous "ask the user for a file" call needs neither Qt Widgets nor an external program.
//
// A helper is shown with show() and awaited in a local event loop (the application keeps painting while
// the modal dialog is up). It is created and used on the GUI thread only.

#include "platform/qt/QtDialogs.hpp"

#include "platform/qt/QtGuiThread.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QWindow>
#include <private/qguiapplication_p.h>
#include <qpa/qplatformdialoghelper.h>
#include <qpa/qplatformtheme.h>

#include <memory>
#include <optional>

namespace bps::platform {
namespace {


QPlatformTheme* Theme() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return nullptr;
    return QGuiApplicationPrivate::platformTheme();
}

bool NativeFor(QPlatformTheme::DialogType type) {
    QPlatformTheme* theme = Theme();
    return theme && theme->usePlatformNativeDialog(type);
}

// The window a dialog should sit over: the focused one, else any top-level.
QWindow* ParentWindow() {
    if (QWindow* focus = QGuiApplication::focusWindow()) return focus;
    const auto windows = QGuiApplication::topLevelWindows();
    return windows.isEmpty() ? nullptr : windows.first();
}

// "Images (*.png;*.jpg)" (the PAL's spelling) -> "Images (*.png *.jpg)" (Qt's).
QStringList QtFilters(const std::vector<std::string>& filters) {
    QStringList out;
    for (const std::string& f : filters) {
        QString s = QString::fromStdString(f);
        const int open = s.indexOf(QLatin1Char('('));
        const int close = s.lastIndexOf(QLatin1Char(')'));
        if (open >= 0 && close > open) {
            QString patterns = s.mid(open + 1, close - open - 1);
            patterns.replace(QLatin1Char(';'), QLatin1Char(' ')).replace(QLatin1Char(','), QLatin1Char(' '));
            s = s.left(open) + QLatin1Char('(') + patterns.simplified() + QLatin1Char(')');
        }
        out << s;
    }
    if (out.isEmpty()) out << QStringLiteral("All files (*)");
    return out;
}

// Shows `helper` modally and waits. Returns false when it could not be shown at all (the caller falls
// back); `accepted` says whether the user confirmed.
bool RunHelper(QPlatformDialogHelper& helper, bool& accepted) {
    QEventLoop loop;
    accepted = false;
    QObject::connect(&helper, &QPlatformDialogHelper::accept, &loop, [&] { accepted = true; loop.quit(); });
    QObject::connect(&helper, &QPlatformDialogHelper::reject, &loop, [&] { loop.quit(); });
    if (!helper.show(Qt::Dialog, Qt::ApplicationModal, ParentWindow()))
        return false;
    loop.exec();
    helper.hide();
    return true;
}

enum class FileKind { Open, Save, Folder };

// nullopt (the outer one) = no native dialog could be shown; an empty inner optional = the user cancelled.
std::optional<std::optional<std::string>> NativeFileDialog(FileKind kind, std::string_view title,
                                                           const std::vector<std::string>& filters,
                                                           std::string_view defaultName) {
    if (!NativeFor(QPlatformTheme::FileDialog)) return std::nullopt;
    std::unique_ptr<QPlatformDialogHelper> helper(Theme()->createPlatformDialogHelper(QPlatformTheme::FileDialog));
    auto* fileHelper = qobject_cast<QPlatformFileDialogHelper*>(helper.get());
    if (!fileHelper) return std::nullopt;

    auto options = QFileDialogOptions::create();
    options->setWindowTitle(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
    switch (kind) {
    case FileKind::Open:
        options->setAcceptMode(QFileDialogOptions::AcceptOpen);
        options->setFileMode(QFileDialogOptions::ExistingFile);
        options->setNameFilters(QtFilters(filters));
        break;
    case FileKind::Save:
        options->setAcceptMode(QFileDialogOptions::AcceptSave);
        options->setFileMode(QFileDialogOptions::AnyFile);
        break;
    case FileKind::Folder:
        options->setAcceptMode(QFileDialogOptions::AcceptOpen);
        options->setFileMode(QFileDialogOptions::Directory);
        options->setOption(QFileDialogOptions::ShowDirsOnly, true);
        break;
    }
    fileHelper->setOptions(options);
    if (kind == FileKind::Save && !defaultName.empty())
        fileHelper->selectFile(QUrl::fromLocalFile(QString::fromUtf8(defaultName.data(), static_cast<qsizetype>(defaultName.size()))));

    bool accepted = false;
    if (!RunHelper(*fileHelper, accepted)) return std::nullopt;
    if (!accepted) return std::optional<std::string>{};
    const QList<QUrl> picked = fileHelper->selectedFiles();
    if (picked.isEmpty()) return std::optional<std::string>{};
    const QString path = picked.first().toLocalFile();
    if (path.isEmpty()) return std::optional<std::string>{};
    return std::optional<std::string>{path.toStdString()};
}

// 1 = Ok/Yes, 0 = No/closed, -1 = no native dialog.
int NativeMessage(std::string_view title, std::string_view message, bool question) {
    if (!NativeFor(QPlatformTheme::MessageDialog)) return -1;
    std::unique_ptr<QPlatformDialogHelper> helper(Theme()->createPlatformDialogHelper(QPlatformTheme::MessageDialog));
    auto* messageHelper = qobject_cast<QPlatformMessageDialogHelper*>(helper.get());
    if (!messageHelper) return -1;

    auto options = QMessageDialogOptions::create();
    options->setWindowTitle(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
    options->setText(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    options->setStandardIcon(question ? QMessageDialogOptions::Question : QMessageDialogOptions::Information);
    options->setStandardButtons(question
        ? QPlatformDialogHelper::StandardButtons(QPlatformDialogHelper::Yes | QPlatformDialogHelper::No)
        : QPlatformDialogHelper::StandardButtons(QPlatformDialogHelper::Ok));
    messageHelper->setOptions(options);

    QPlatformDialogHelper::StandardButton clicked = QPlatformDialogHelper::NoButton;
    QObject::connect(messageHelper, &QPlatformMessageDialogHelper::clicked, messageHelper,
                     [&](QPlatformDialogHelper::StandardButton button, QPlatformDialogHelper::ButtonRole) { clicked = button; });
    bool accepted = false;
    if (!RunHelper(*messageHelper, accepted)) return -1;
    return (clicked == QPlatformDialogHelper::Yes || clicked == QPlatformDialogHelper::Ok) ? 1 : 0;
}

Error NoDialogs() {
    return Error::Make(Err::Unsupported, "Dialogs",
                       "no native dialog is available in this desktop session");
}

} // namespace

QtDialogs::QtDialogs(IDialogs* fallback, std::function<bool()> fallbackSupported)
    : fallback_(fallback), fallbackSupported_(std::move(fallbackSupported)) {}

bool QtDialogs::Supported() const {
    if (NativeFor(QPlatformTheme::FileDialog)) return true;
    return fallbackSupported_ ? fallbackSupported_() : fallback_ != nullptr;
}

Result<std::optional<std::string>> QtDialogs::OpenFileDialog(std::string_view title,
                                                             const std::vector<std::string>& filters) {
    const std::string t(title);
    auto native = OnGuiThread([&]() -> Result<std::optional<std::optional<std::string>>> {
        return NativeFileDialog(FileKind::Open, t, filters, {});
    });
    if (native.ok() && native.value()) return *native.value();
    return fallback_ ? fallback_->OpenFileDialog(title, filters) : Result<std::optional<std::string>>(NoDialogs());
}

Result<std::optional<std::string>> QtDialogs::SaveFileDialog(std::string_view title, std::string_view defaultName) {
    const std::string t(title), name(defaultName);
    auto native = OnGuiThread([&]() -> Result<std::optional<std::optional<std::string>>> {
        return NativeFileDialog(FileKind::Save, t, {}, name);
    });
    if (native.ok() && native.value()) return *native.value();
    return fallback_ ? fallback_->SaveFileDialog(title, defaultName) : Result<std::optional<std::string>>(NoDialogs());
}

Result<std::optional<std::string>> QtDialogs::SelectFolderDialog(std::string_view title) {
    const std::string t(title);
    auto native = OnGuiThread([&]() -> Result<std::optional<std::optional<std::string>>> {
        return NativeFileDialog(FileKind::Folder, t, {}, {});
    });
    if (native.ok() && native.value()) return *native.value();
    return fallback_ ? fallback_->SelectFolderDialog(title) : Result<std::optional<std::string>>(NoDialogs());
}

Result<void> QtDialogs::MessageDialog(std::string_view title, std::string_view message) {
    const std::string t(title), m(message);
    auto shown = OnGuiThread([&]() -> Result<int> { return NativeMessage(t, m, false); });
    if (shown.ok() && shown.value() >= 0) return Ok();
    return fallback_ ? fallback_->MessageDialog(title, message) : Result<void>(NoDialogs());
}

Result<bool> QtDialogs::QuestionDialog(std::string_view title, std::string_view message) {
    const std::string t(title), m(message);
    auto shown = OnGuiThread([&]() -> Result<int> { return NativeMessage(t, m, true); });
    if (shown.ok() && shown.value() >= 0) return shown.value() == 1;
    return fallback_ ? fallback_->QuestionDialog(title, message) : Result<bool>(NoDialogs());
}

} // namespace bps::platform
