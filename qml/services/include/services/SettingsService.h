#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "modules/settings/AppSettings.hpp"

class QQmlEngine;
class QJSEngine;

// The UI's window onto the ENGINE's settings (bps::settings::AppSettings - Settings > General and Smart Config), its hardware
// report, and its backups / crash-recovery store. The engine owns everything that matters: what can be set, the defaults, the
// allowed values, the checks, the file, and what a setting means for the rest of the engine. This class hands that to QML and
// does the few things only the app can do (start with Windows, tell the user something was refused).
//
// QML binds to `values` (a map: key -> current value) - it changes whenever any setting does, so
// `SettingsService.values["startup.autosave"]` is a live binding - and to `definitions` for the labels, descriptions and lists
// of choices, so a screen carries no default or option list of its own.
class SettingsService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    // key -> { key, group, label, description, kind ("bool"|"int"|"choice"|"text"), min, max, choices: [{ value, label, description, color, colorLight }] }
    Q_PROPERTY(QVariantMap definitions READ definitions CONSTANT)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentLight READ accentLight NOTIFY changed)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

    // Smart Config: what this machine has, and what the chosen profile allows of it.
    Q_PROPERTY(QVariantList hardwareRows READ hardwareRows NOTIFY hardwareChanged)   // [{ key, label, value, ok }]
    Q_PROPERTY(QString recommendedProfile READ recommendedProfile NOTIFY hardwareChanged)
    Q_PROPERTY(QString hardwareHeadline READ hardwareHeadline NOTIFY hardwareChanged)
    Q_PROPERTY(QString hardwareDetail READ hardwareDetail NOTIFY hardwareChanged)
    Q_PROPERTY(QVariantMap caps READ caps NOTIFY changed)              // { gpu, cpu } in force (percent)
    Q_PROPERTY(QVariantMap allocation READ allocation NOTIFY changed)  // { rendering, encoding, output } for the profile (percent)

public:
    static SettingsService &instance();
    static SettingsService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    bool ready() const { return settings_ != nullptr; }
    QVariantMap values() const { return values_; }
    QVariantMap definitions() const;
    QColor accent() const { return accent_; }
    QColor accentLight() const { return accentLight_; }
    QString appVersion() const;
    QVariantList hardwareRows() const { return hardwareRows_; }
    QString recommendedProfile() const { return recommendedProfile_; }
    QString hardwareHeadline() const { return hardwareHeadline_; }
    QString hardwareDetail() const { return hardwareDetail_; }
    QVariantMap caps() const;
    QVariantMap allocation() const;

    // Sets a setting. The engine checks it; a refused value is reported to the user and false is returned (nothing changes).
    Q_INVOKABLE bool setValue(const QString &key, const QVariant &value);
    Q_INVOKABLE QVariant value(const QString &key) const;
    // The label of the choice a Choice setting is on now ("Dark", "Every 30 minutes"...).
    Q_INVOKABLE QString choiceLabel(const QString &key) const;
    // Every setting back to its default; returns how many changed.
    Q_INVOKABLE int resetAll();

    // Looks at the machine again (after a device was plugged in, or when the Smart Config page opens).
    Q_INVOKABLE void refreshHardware();
    // Puts the engine's recommended profile in force.
    Q_INVOKABLE void applyRecommendedProfile();

    // Whether a file is there (the last show may have been moved or deleted since the app was closed).
    Q_INVOKABLE bool fileExists(const QString &path) const;

    // ---- Backups & recovery ----
    // Copies a show file into the backups folder (a dated copy) and drops the oldest beyond "Keep last". Returns the copy's path,
    // or "" when nothing was copied (the show is unchanged since its newest backup, or it could not be read).
    Q_INVOKABLE QString backupShow(const QString &showFile);
    Q_INVOKABLE QString backupDirectory() const;
    Q_INVOKABLE QVariantList backupsOf(const QString &showName) const;   // [{ path, stamp, sizeBytes }] newest first
    // Where the app writes the recovery copy of the open show; then noteRecovery() says which show it is.
    Q_INVOKABLE QString recoveryPath() const;
    Q_INVOKABLE bool noteRecovery(const QString &showName, const QString &originalPath);
    // What a previous run left behind: { path, showName, originalPath }, or {} when it ended cleanly.
    Q_INVOKABLE QVariantMap pendingRecovery() const;
    Q_INVOKABLE void clearRecovery();
    // Puts what a previous run left behind into a NEW file at `target` (never over an existing one) and clears it. True when done.
    Q_INVOKABLE bool restoreRecovery(const QString &target);

signals:
    void changed();
    void hardwareChanged();
    // One setting changed (already saved and applied to the engine).
    void valueChanged(const QString &key);

private:
    explicit SettingsService(QObject *parent = nullptr);
    void load();
    void refreshValues();
    void applyLaunchAtLogin();
    void report(const QString &message, const QString &level = QStringLiteral("error")) const;

    std::unique_ptr<bps::settings::AppSettings> settings_;
    QVariantMap values_;
    QColor accent_;
    QColor accentLight_;
    QVariantList hardwareRows_;
    QString recommendedProfile_;
    QString hardwareHeadline_;
    QString hardwareDetail_;
    std::string dataDir_;
};
