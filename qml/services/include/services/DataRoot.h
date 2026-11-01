#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

// WHERE THIS PROCESS KEEPS ITS "ROAMING" DATA: engine data (kernel.json, bibles), crash
// records, imported media.
//
// An installed app keeps it in the user's Roaming\VGR\VGRPresenter folder. A DEV build must
// not: it and the installed app would read and write one profile (templates, settings,
// styles), and every test run would rewrite the operator's real configuration. main.cpp
// points a dev build at <project>/dev-profile (VGR_PROFILE_DIR), seeded from
// <project>/installer/seed-data, and this is the one place the Qt side asks.
//
// (The engine's own platform layer reads the same variable for its "local" side:
// WindowsPaths::UserDataDir — settings.json, templates.json, the_table.json.)
namespace dataroot {

inline QString appDataRoot()
{
    const QString profile = qEnvironmentVariable("VGR_PROFILE_DIR");
    if (!profile.isEmpty() && !QStandardPaths::isTestModeEnabled())
        return QDir::cleanPath(profile) + QStringLiteral("/roaming");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

} // namespace dataroot
