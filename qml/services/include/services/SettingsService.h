#pragma once

#include <QColor>
#include <QJSValue>
#include <QObject>
#include <QString>
#include <QTranslator>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <vector>

#include "modules/settings/AppSettings.hpp"

class QQmlEngine;
class QJSEngine;

namespace bps::presentation {
struct OutputStyleSpec;
}

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
    // The app's theme choice ("dark" | "light"), applied by the Theme singleton.
    Q_PROPERTY(QString theme READ theme NOTIFY changed)
    // Bumped every time the LOCALE is re-applied (appearance.language changes).
    // Locale-dependent formatting cannot re-evaluate by itself - QLocale has no
    // signal - so a binding that formats a date/number reads this to subscribe.
    Q_PROPERTY(int localeRevision READ localeRevision NOTIFY localeChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

    // Smart Config: what this machine has, and what the chosen profile allows of it.
    Q_PROPERTY(QVariantList hardwareRows READ hardwareRows NOTIFY hardwareChanged)   // [{ key, label, value, ok }]
    // The engine's config advice for this machine: [{ key, severity ("ok"|"info"|"warn"), text }]
    Q_PROPERTY(QVariantList hardwareAdvice READ hardwareAdvice NOTIFY hardwareChanged)
    Q_PROPERTY(QString hardwareAdviceSummary READ hardwareAdviceSummary NOTIFY hardwareChanged)
    Q_PROPERTY(QString recommendedProfile READ recommendedProfile NOTIFY hardwareChanged)
    Q_PROPERTY(QString hardwareHeadline READ hardwareHeadline NOTIFY hardwareChanged)
    Q_PROPERTY(QString hardwareDetail READ hardwareDetail NOTIFY hardwareChanged)
    Q_PROPERTY(QVariantMap caps READ caps NOTIFY changed)              // { gpu, cpu } in force (percent)
    Q_PROPERTY(QVariantMap allocation READ allocation NOTIFY changed)  // { rendering, encoding, output } for the profile (percent)

public:
    static SettingsService &instance();
    // The instance() reference is the QML singleton; the output models boot
    // before QML constructs it, so they use this null-safe pointer form.
    static SettingsService *instancePtr() { return s_instance; }
    static SettingsService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    bool ready() const { return settings_ != nullptr; }
    QVariantMap values() const { return values_; }
    QVariantMap definitions() const;
    QColor accent() const { return accent_; }
    QColor accentLight() const { return accentLight_; }
    QString theme() const;
    int localeRevision() const { return localeRevision_; }
    QString appVersion() const;
    QVariantList hardwareRows() const { return hardwareRows_; }
    QVariantList hardwareAdvice() const { return hardwareAdvice_; }
    QString hardwareAdviceSummary() const { return hardwareAdviceSummary_; }
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

    // Runs `callback` once the engine's settings store is open (immediately if
    // it already is). THE idiom for a read that must not see the engine's
    // defaults: at construction the store is not open yet - it loads only after
    // the engine boots - so `values` still holds defaults until then, and a
    // screen that acts on them at construction acts on the wrong thing. One
    // shot; the callback is never run twice. C++ services pass a lambda, QML a
    // function (SettingsService.whenReady(function() { ... })).
    //
    // It is NOT cancelled if the caller dies first, so register from something
    // that outlives boot - a root-level screen or a singleton service, as every
    // current caller does.
    void whenReady(std::function<void()> callback);
    Q_INVOKABLE void whenReady(const QJSValue &callback);

    // Looks at the machine again (after a device was plugged in, or when the Smart Config page opens).
    Q_INVOKABLE void refreshHardware();
    // Puts the engine's recommended profile in force.
    Q_INVOKABLE void applyRecommendedProfile();
    // The GPU / CPU caps a resource profile allows ({ gpu, cpu }, percent). The
    // Smart Config screen uses it to tell whether the recommended profile is
    // still IN FORCE: in Manual mode the caps in force are the sliders' own
    // numbers, so moving one off the profile's numbers means the recommendation
    // is no longer what the engine runs on ("Apply" becomes available again).
    Q_INVOKABLE QVariantMap capsFor(const QString &profile) const;

    // The scripture options (the Scripture tab's settings) in the form the engine's slide builder takes them.
    bps::presentation::ScriptureSettings scriptureSettings() const;
    // The Table's slide-builder options (the "table." keys).
    bps::presentation::ScriptureSettings theTableSettings() const;

    // ---- Output style (Settings · Styles applied to the on-air output) ----
    // Bridges OutputListModel's UI state into the engine's live render loop
    // (PresentationEngine::SetActiveOutputStyle). Safe to call before boot —
    // a not-yet-running engine simply has nothing to push to.
    void setActiveOutputStyle(const bps::presentation::OutputStyleSpec &spec);
    // The per-output pass set (one gated render per style-wearing output —
    // PresentationEngine::SetLiveOutputStyles). Safe before boot, like the
    // single-spec push above.
    void setLiveOutputStyles(const std::vector<bps::presentation::OutputStyleSpec> &specs,
                             const std::vector<std::string> &bufferNames);

    // Whether a file is there (the last show may have been moved or deleted since the app was closed).
    Q_INVOKABLE bool fileExists(const QString &path) const;

    // ---- Backups & recovery ----
    // Copies a show file into the backups folder (a dated copy) and drops the oldest beyond "Keep last". Returns the copy's path,
    // or "" when nothing was copied (the show is unchanged since its newest backup, or it could not be read).
    Q_INVOKABLE QString backupShow(const QString &showFile);
    Q_INVOKABLE QString backupDirectory() const;
    // Shows the backups folder in the OS file manager. Creates the folder first when the app has
    // never taken a backup (the engine only makes it on the first Create), so the user sees an empty
    // folder rather than an error. False when the engine is not up yet (no data directory) or the
    // OS refused to open it.
    Q_INVOKABLE bool openBackupDirectory() const;
    // Reveals ONE backup file in the OS file manager (a browser row's "Show in
    // folder") — selects it in Explorer / Finder. Only a path inside the backups
    // folder is accepted; on a desktop with no portable "select" (Linux) this
    // falls back to opening the enclosing folder. False when refused.
    Q_INVOKABLE bool showBackupInFolder(const QString &path) const;
    Q_INVOKABLE QVariantList backupsOf(const QString &showName) const;   // [{ path, stamp, sizeBytes }] newest first
    // EVERY backup, newest first, each carrying a "category": the all-shows
    // browser (the row above only ever sees the OPEN show):
    // [{ path, show, category, stamp, sizeBytes }]. "category" is "show" for a
    // show file, "settings"/"overlays"/"templates" for one of those copies, so
    // the browser knows which restore a row needs.
    Q_INVOKABLE QVariantList allBackups() const;
    // Copies each SELECTED category (the backups.include* settings) into the
    // backups folder and prunes it to "Keep last". Returns how many copies were
    // made (0 when a category is off or its file has never been written).
    Q_INVOKABLE int backupCategories();
    // Puts a settings / overlays / templates backup back over its live file and
    // reloads what reads it. False (with a report) for a show backup or a path
    // outside the backups folder. The current file is replaced — the caller
    // confirms first.
    Q_INVOKABLE bool restoreCategoryBackup(const QString &backupPath);
    // Removes one backup file (a browser row's Delete). False + a report when the
    // engine refuses — a path that is not one of this store's own backups.
    Q_INVOKABLE bool deleteBackup(const QString &path);
    // Copies one of those backups OUT to `target` as a new file (an existing file is never overwritten).
    // The BackupStore could only ever create and prune backups; this is the read side, so the UI can hand one
    // back to the user as a new show instead of the backups being write-only. True when the copy was made.
    Q_INVOKABLE bool restoreBackup(const QString &backupPath, const QString &target);
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
    // The locale was re-applied (see localeRevision).
    void localeChanged();
    // One setting changed (already saved and applied to the engine).
    void valueChanged(const QString &key);

private:
    explicit SettingsService(QObject *parent = nullptr);
    static SettingsService *s_instance;
    void load();
    void refreshValues();
    void applyLaunchAtLogin();
    // Puts appearance.language in force: the default QLocale (dates, times,
    // numbers follow it at once) and any installed translation for it.
    void applyLanguage();
    void report(const QString &message, const QString &level = QStringLiteral("error")) const;

    std::unique_ptr<bps::settings::AppSettings> settings_;
    // Callbacks handed to whenReady() before the store opened; drained once, at
    // the end of load().
    std::vector<std::function<void()>> pendingReady_;
    // The QML engine, kept only so a language change can re-translate the UI
    // (QQmlEngine::retranslate re-evaluates every qsTr binding).
    QQmlEngine *qmlEngine_ = nullptr;
    std::unique_ptr<QTranslator> translator_;
    int localeRevision_ = 0;
    QVariantMap values_;
    QColor accent_;
    QColor accentLight_;
    QVariantList hardwareRows_;
    QVariantList hardwareAdvice_;
    QString hardwareAdviceSummary_;
    QString recommendedProfile_;
    QString hardwareHeadline_;
    QString hardwareDetail_;
    std::string dataDir_;
};
