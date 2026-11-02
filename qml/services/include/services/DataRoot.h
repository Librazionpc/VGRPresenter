#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace dataroot {
inline QString appDataRoot()
{
    const QString profile = qEnvironmentVariable("VGR_PROFILE_DIR");
    if (!profile.isEmpty() && !QStandardPaths::isTestModeEnabled())
        return QDir::cleanPath(profile) + QStringLiteral("/roaming");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
} // namespace dataroot