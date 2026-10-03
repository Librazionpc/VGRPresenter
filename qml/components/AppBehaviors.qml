import QtQuick
import Qt.labs.platform as Platform
import VGRPresenterUI

// What the Settings > General switches DO to the running app. The engine keeps the settings and their meaning
// (bps::settings::AppSettings, through SettingsService); the things only the app itself can carry out live here:
//
//   Start minimized          the window starts in the task bar
//   Open last project /      the show (and screen) you left comes back at start-up
//   Restore last session
//   Autosave                 saves the open show by itself, every "Autosave every", once it has a file
//   Automatic backups        a dated copy of the saved show every "Back up" interval, keeping "Keep last"
//   Crash recovery           the unsaved show is written aside every few seconds while it is dirty; after an unexpected
//                            exit it is offered back as a NEW show (nothing is overwritten)
//   Close to tray            closing the window hides it to the system tray; the tray icon brings it back or quits
//
// (Notifications and Lock In Mode are applied by NotificationCenter; launch at login, the log level, the resource profile and
// budgets are applied by the engine / SettingsService.) A plain Item: it draws nothing.
Item {
    id: root

    required property var window        // the ApplicationWindow
    required property var editScreen    // its EditScreen (owns the show session)

    readonly property var values: SettingsService.values
    readonly property var session: root.editScreen.session

    // An explicit Quit (the tray menu): the window closes for real instead of hiding to the tray.
    property bool quitting: false
    // The app has finished starting; from here the screen and show you are on are remembered.
    property bool started: false
    // A recovery copy of the open show was written this run, so it is ours to clear once the show is saved.
    property bool recoveryWritten: false

    // ---- start-up ----------------------------------------------------------------------------------------------

    // WAIT FOR THE ENGINE'S STORE, not a fixed delay. start() reads the
    // preferences and the last session out of SettingsService, and those only
    // carry the user's choices once the store has been read (its load runs on
    // bootedChanged). SettingsService.whenReady is the one idiom
    // for a pre-boot read; the timer below then gives the scene a beat to settle
    // before the last show/screen comes back. (A bare fixed delay read the
    // engine's DEFAULTS on a slow boot - startMinimized false, lastShowPath "",
    // recovery path "" - so Start minimized, Restore last session, Open last
    // project and crash recovery were silently ignored.)
    Component.onCompleted: SettingsService.whenReady(function() { startTimer.start() })

    Timer {
        id: startTimer
        interval: 700       // the screens are up (the STORE is what we wait for)
        onTriggered: root.start()
    }

    function start() {
        if (root.started)
            return          // never run twice (a late store load must not re-open a show)
        const lastShow = String(root.values["session.lastShowPath"] ?? "")
        const lastView = String(root.values["session.lastView"] ?? "")

        if (root.values["preferences.startMinimized"] === true)
            root.window.showMinimized()

        // Something an unexpected exit left behind comes first.
        const waiting = SettingsService.pendingRecovery()
        if (waiting.path !== undefined) {
            if (root.values["backups.crashRecovery"] === true && root.recover(waiting)) {
                root.started = true
                return
            }
            SettingsService.clearRecovery()
        }

        const restoring = root.values["preferences.restoreLastSession"] === true
        const wantsShow = root.values["startup.openLastProject"] === true || (restoring && lastView === "edit")
        if (wantsShow && SettingsService.fileExists(lastShow)) {
            root.editScreen.openShowPath(lastShow)
            root.window.currentView = restoring && lastView !== "" ? lastView : "edit"
        } else if (restoring && (lastView === "show" || lastView === "edit" && ShowService.hasShow)) {
            root.window.currentView = lastView
        }
        root.started = true
    }

    // Brings an interrupted show back as a new file in the library, named after it, and opens it.
    function recover(waiting) {
        const target = ShowService.newLibraryShowPath("", qsTr("%1 (recovered)").arg(waiting.showName))
        if (target === "" || !SettingsService.restoreRecovery(target))
            return false
        root.editScreen.openShowPath(target)
        root.window.currentView = "edit"
        EventBus.notify(qsTr("The app closed unexpectedly. Your unsaved changes to \"%1\" were saved as \"%2\".")
                            .arg(waiting.showName).arg(qsTr("%1 (recovered)").arg(waiting.showName)),
                        "warning", qsTr("Recovered"), "settings.recovery.restored")
        return true
    }

    // ---- remembering where you were ----------------------------------------------------------------------------

    Connections {
        target: root.window
        function onCurrentViewChanged() {
            if (root.started && root.window.currentView !== String(root.values["session.lastView"]))
                SettingsService.setValue("session.lastView", root.window.currentView)
        }
    }
    Connections {
        target: ShowService
        function onShowChanged() {
            if (root.started && ShowService.showPath !== "" && ShowService.showPath !== String(root.values["session.lastShowPath"]))
                SettingsService.setValue("session.lastShowPath", ShowService.showPath)
            // Saved (or nothing to save): a recovery copy this run wrote is stale now.
            if (root.recoveryWritten && ShowService.hasShow && !ShowService.showDirty) {
                SettingsService.clearRecovery()
                root.recoveryWritten = false
            }
        }
    }

    // ---- autosave ------------------------------------------------------------------------------------------------

    Timer {
        interval: Math.max(10, Number(root.values["startup.autosaveSeconds"] ?? 60)) * 1000
        repeat: true
        running: root.values["startup.autosave"] === true && ShowService.hasShow && !root.session.designMode
        onTriggered: {
            // Only a show that already has a file (autosave never asks where to put one), and only when it changed.
            if (ShowService.showPath === "" || !ShowService.showDirty)
                return
            root.session.flushAll()
            ShowService.saveCurrentShow("", true)
        }
    }

    // ---- automatic backups ---------------------------------------------------------------------------------------

    // True while any of the "back up this too" categories is selected — the
    // timer must run for those even when no show is open.
    readonly property bool backupsAnythingElse: root.values["backups.includeSettings"] === true
                                                || root.values["backups.includeOverlays"] === true
                                                || root.values["backups.includeTemplates"] === true

    Timer {
        interval: Math.max(1, Number(root.values["backups.intervalMinutes"] ?? 30)) * 60 * 1000
        repeat: true
        running: root.values["backups.automatic"] === true
                 && ((ShowService.hasShow && ShowService.showPath !== "") || root.backupsAnythingElse)
        onTriggered: {
            // The saved file as it is on disk; the engine skips a copy identical
            // to the newest one, so an unchanged show adds nothing.
            if (ShowService.showPath !== "")
                SettingsService.backupShow(ShowService.showPath)
            // Then whichever categories the user selected (settings / overlays /
            // templates), each also skipped when unchanged.
            SettingsService.backupCategories()
        }
    }

    // ---- crash recovery ------------------------------------------------------------------------------------------

    Timer {
        interval: 15000
        repeat: true
        running: root.values["backups.crashRecovery"] === true && ShowService.hasShow && ShowService.showDirty && !root.session.designMode
        onTriggered: root.writeRecoveryCopy()
    }

    function writeRecoveryCopy() {
        root.session.flushAll()   // the engine holds what is on the canvas
        if (ShowService.saveShowCopy(SettingsService.recoveryPath())) {
            SettingsService.noteRecovery(ShowService.showName, ShowService.showPath)
            root.recoveryWritten = true
        }
    }

    // ---- closing ---------------------------------------------------------------------------------------------------

    // Should closing the window hide it to the tray instead of ending the app?
    function hidesToTray() {
        return !root.quitting && root.values["preferences.closeToTray"] === true && Platform.SystemTrayIcon.available
    }

    // The app is really going away: nothing is waiting to be recovered.
    function cleanExit() {
        SettingsService.clearRecovery()
    }

    function quit() {
        root.quitting = true
        root.window.show()
        root.window.raise()
        root.window.requestActivate()
        root.window.close()
    }

    // ---- the system tray -----------------------------------------------------------------------------------------

    Platform.SystemTrayIcon {
        visible: root.values["preferences.closeToTray"] === true && Platform.SystemTrayIcon.available
        icon.source: Qt.resolvedUrl("../assets/tray.png")
        tooltip: qsTr("VGRPresenter")
        onActivated: (reason) => {
            if (reason === Platform.SystemTrayIcon.Trigger || reason === Platform.SystemTrayIcon.DoubleClick) {
                root.window.show()
                root.window.raise()
                root.window.requestActivate()
            }
        }
        menu: Platform.Menu {
            Platform.MenuItem {
                text: qsTr("Show VGRPresenter")
                onTriggered: {
                    root.window.show()
                    root.window.raise()
                    root.window.requestActivate()
                }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Quit")
                onTriggered: root.quit()
            }
        }
    }
}
