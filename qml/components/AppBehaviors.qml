import QtQuick
import Qt.labs.platform as Platform
import VGRPresenterUI

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
    property int _recoveryToken: 0
    property var pendingRecoveryOffer: null

    // ---- start-up ----------------------------------------------------------------------------------------------

    Component.onCompleted: {
        NotificationCenter.registerAction("recovery.open", function() { root.openOfferedRecovery() })
        NotificationCenter.registerAction("buses.undoRemove", function() { BusListModel.restoreRemovedBus() })
        SettingsService.whenReady(function() { startTimer.start() })
    }

    // ---- the media file Undo's one click ---------------------------------------------------------
    Connections {
        target: EventBus
        function onEventPosted(topic, payload) {
            void topic
            const action = payload ? payload.action : null
            if (!action || typeof action !== "object" || !action.id || action.token === undefined)
                return
            const token = String(action.token)
            const id = String(action.id)
            NotificationCenter.registerAction(id, function() {
                MediaLibraryService.undoRemove(token)
                NotificationCenter.unregisterAction(id)
            })
        }
    }

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

        SettingsService.requestPendingRecovery()

        const restoring = root.values["preferences.restoreLastSession"] === true
        const wantsShow = root.values["startup.openLastProject"] === true || (restoring && lastView === "edit")
        const design = restoring && lastView === "edit" ? root.designToRestore() : null
        if (design !== null && root.editScreen.openDesign(design.kind, design.id)) {
            root.window.currentView = "edit"
        } else if (wantsShow && SettingsService.fileExists(lastShow)) {
            root.editScreen.openShowPath(lastShow)
            root.window.currentView = restoring && lastView !== "" ? lastView : "edit"
        } else if (restoring && (lastView === "show" || lastView === "edit" && ShowService.hasShow)) {
            root.window.currentView = lastView
        }
        if (restoring) {
            const section = String(root.values["session.lastSettingsSection"] ?? "")
            if (section !== "") {
                root.window.openSettings(section)
                EngineBridge.log("info", "Navigation", "Resumed Settings \u00b7 " + section)
            }
        }
        root.started = true
    }

    function designToRestore() {
        const saved = String(root.values["session.lastDesign"] ?? "")
        const cut = saved.indexOf(":")
        if (cut <= 0 || cut === saved.length - 1)
            return null
        return { kind: saved.slice(0, cut), id: saved.slice(cut + 1) }
    }

    function offerRecovery(waiting) {
        root.pendingRecoveryOffer = waiting
        const target = qsTr("%1 (recovered)").arg(waiting.showName)
        EventBus.notify(qsTr("The app closed unexpectedly. Your unsaved changes to “%1” were written aside as “%2” — open them?")
                            .arg(waiting.showName).arg(target),
                        "error", qsTr("Crash recovery"), "settings.recovery.available",
                        [waiting.showName, target],
                        { id: "recovery.open", label: qsTr("Open recovered show") })
        EngineBridge.log("error", "Crash recovery",
                         qsTr("Offered the unsaved changes to \"%1\" as \"%2\" (not opened — one click on the notice opens it)")
                             .arg(waiting.showName).arg(target))
    }

    property string recoveryTarget: ""
    function openOfferedRecovery() {
        const waiting = root.pendingRecoveryOffer
        if (waiting === null)
            return false
        const target = ShowService.newLibraryShowPath("", qsTr("%1 (recovered)").arg(waiting.showName))
        if (target === "") {
            EventBus.notify(qsTr("The recovered copy of “%1” could not be opened any more.")
                                .arg(waiting.showName),
                            "error", qsTr("Crash recovery"), "settings.recovery.failed")
            return false
        }
        root.recoveryTarget = target
        SettingsService.restoreRecovery(target)
        return true
    }

    // The engine's answers, off the interface thread.
    Connections {
        target: SettingsService
        // What a previous run left behind. `path` is undefined for a clean exit.
        function onPendingRecoveryReady(info) {
            if (info.path === undefined)
                return
            if (root.values["backups.crashRecovery"] === true)
                root.offerRecovery(info)
            else
                SettingsService.clearRecovery()
        }
        // A recovery copy landed (or was refused): open what was asked for.
        function onRestoreFinished(ok, what) {
            if (root.recoveryTarget === "" || what !== root.recoveryTarget)
                return
            const target = root.recoveryTarget
            root.recoveryTarget = ""
            const waiting = root.pendingRecoveryOffer
            const showName = waiting === null ? "" : String(waiting.showName)
            if (!ok) {
                EventBus.notify(qsTr("The recovered copy of “%1” could not be opened any more.").arg(showName),
                                "error", qsTr("Crash recovery"), "settings.recovery.failed")
                return
            }
            const label = qsTr("%1 (recovered)").arg(showName)
            root.pendingRecoveryOffer = null
            root.editScreen.openShowPath(target)
            root.window.currentView = "edit"
            EngineBridge.log("info", "Crash recovery",
                             qsTr("Opened the recovered copy as \"%1\"").arg(label))
            EventBus.notify(qsTr("“%1” was opened as “%2” — the recovered copy.")
                                .arg(showName).arg(label),
                            "success", qsTr("Recovered"), "settings.recovery.restored")
        }
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
        target: root.session
        function onDesignModeChanged() { root.rememberDesign() }
        function onDesignIdChanged() { root.rememberDesign() }
    }
    function rememberDesign() {
        if (!root.started)
            return
        const at = root.session.designMode
                   ? root.session.designKind + ":" + root.session.designId : ""
        if (at !== String(root.values["session.lastDesign"] ?? ""))
            SettingsService.setValue("session.lastDesign", at)
    }
    Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok) {
            if (kind !== "saveCopy" || token !== root._recoveryToken)
                return
            root._recoveryToken = 0
            if (!ok)
                return
            SettingsService.noteRecovery(ShowService.showName, ShowService.showPath)
            root.recoveryWritten = true
        }
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
        // The write is queued (ShowService's document jobs) and its verdict lands
        // in this file's Connections above: the copy is recorded there, not here.
        root._recoveryToken = ShowService.saveShowCopy(SettingsService.recoveryPath())
    }

    // ---- closing ---------------------------------------------------------------------------------------------------

    // Should closing the window hide it to the tray instead of ending the app?
    function hidesToTray() {
        return !root.quitting && root.values["preferences.closeToTray"] === true && Platform.SystemTrayIcon.available
    }

    function cleanExit() {
        ShowService.cancelDocumentJobs(["open", "templateLoad"])
        if (root.pendingRecoveryOffer !== null)
            return
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