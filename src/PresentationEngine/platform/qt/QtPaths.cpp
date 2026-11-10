#include "platform/qt/QtServices.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace bps::platform {
namespace {
// The app's data folder name. It is the name already on people's disks.
constexpr const char* kAppFolder = "bps";

std::string Native(const QString& path) {
    QString p = QDir::toNativeSeparators(path);
    // No trailing separator (a drive root keeps its own).
    while (p.size() > 3 && (p.endsWith(QLatin1Char('\\')) || p.endsWith(QLatin1Char('/'))))
        p.chop(1);
    return p.toStdString();
}

std::string Join(const std::string& base, const char* leaf) {
    if (base.empty()) return {};
    return Native(QString::fromStdString(base) + QLatin1Char('/') + QString::fromLatin1(leaf));
}

// VGR_PROFILE_DIR points the whole profile somewhere else (the dev profile, a portable install).
std::string ProfileRoot() {
    const QString root = qEnvironmentVariable("VGR_PROFILE_DIR");
    return root.isEmpty() ? std::string() : Native(root);
}

// QStandardPaths' per-app locations end in "<organization>/<application>" when those are set.
// The app's own folder is "bps" whatever the application object is called, so that suffix is
// taken off again and what is left is the OS's root for that kind of data.
QString WithoutAppSuffix(QString path) {
    const auto strip = [&path](const QString& leaf) {
        if (leaf.isEmpty()) return;
        const QString tail = QLatin1Char('/') + leaf;
        if (path.endsWith(tail, Qt::CaseInsensitive)) path.chop(tail.size());
    };
    strip(QCoreApplication::applicationName());
    strip(QCoreApplication::organizationName());
    return path;
}

// Data that follows the user between machines (Windows "Roaming"; the config home elsewhere).
std::string RoamingRoot() {
#if defined(Q_OS_WIN)
    return Native(WithoutAppSuffix(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
#else
    return Native(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
#endif
}

// Data that stays on this machine.
std::string LocalRoot() {
    return Native(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}
} // namespace

std::string QtPaths::ExecutableDir() const {
    // Qt names the executable through the application object. A build tool or a unit test that
    // never creates one gets the working directory: it is the only instance-free answer that needs
    // no OS header (there is no portable exe-path API), and every caller that needs the real one
    // is an application and already has one.
    if (QCoreApplication::instance())
        return Native(QCoreApplication::applicationDirPath());
    return Native(QDir::currentPath());
}

std::string QtPaths::CurrentWorkingDir() const {
    return Native(QDir::currentPath());
}

std::string QtPaths::ConfigDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(root, "roaming");
    return Join(RoamingRoot(), kAppFolder);
}

std::string QtPaths::UserDataDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(root, "local");
    return Join(LocalRoot(), kAppFolder);
}

std::string QtPaths::AppDataDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(root, "roaming");
#if defined(Q_OS_WIN)
    return Join(RoamingRoot(), kAppFolder);
#else
    // No separate "roaming" concept off Windows: the data home.
    return UserDataDir();
#endif
}

std::string QtPaths::CacheDir() const {
#if defined(Q_OS_WIN)
    return Join(UserDataDir(), "cache");
#else
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(Join(root, "local"), "cache");
    return Join(Native(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)), kAppFolder);
#endif
}

std::string QtPaths::PluginDir() const {
#if defined(Q_OS_WIN)
    return Join(UserDataDir(), "plugins");
#else
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(Join(root, "local"), "plugins");
    return Native(QString::fromStdString(ExecutableDir()) + QStringLiteral("/../lib/bps/plugins"));
#endif
}

std::string QtPaths::AssetDir() const {
#if defined(Q_OS_WIN)
    return Join(UserDataDir(), "assets");
#else
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(Join(root, "local"), "assets");
    return Native(QString::fromStdString(ExecutableDir()) + QStringLiteral("/../share/bps/assets"));
#endif
}

std::string QtPaths::LogDir() const {
#if defined(Q_OS_WIN)
    return Join(UserDataDir(), "logs");
#else
    if (const std::string root = ProfileRoot(); !root.empty()) return Join(Join(root, "local"), "logs");
    const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    return Join(Join(Native(state), kAppFolder), "logs");
#endif
}

std::string QtPaths::TempDir() const {
    return Native(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
}

std::string QtPaths::DownloadsDir() const {
    return Native(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
}

std::string QtPaths::DocumentsDir() const {
    return Native(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

std::string QtPaths::DesktopDir() const {
    return Native(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
}

} // namespace bps::platform
