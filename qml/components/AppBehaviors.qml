import QtQuick
import Qt.labs.platform as Platform
import VGRPresenterUI

// What the Settings > General switches DO to the running app. The engine keeps the settings and their meaning
// (bps::settings::AppSettings, through SettingsService); the things only the app itself can carry out live here:
//
//   Start minimized          the window starts in the task bar
//   Open last project /      OPT-IN resume: the show (and screen) — or the template/overlay
//   Restore last session     edit — you left comes back at start-up. Both are OFF by default
//                            (see AppSettings.cpp): a launch starts on the home (Show) screen
//                            with nothing opened, whatever the last session was doing.
//   Autosave                 saves the open show by itself, every "Autosave every", once it has a file
//   Automatic backups        a dated copy of the saved show every "Back up" interval, keeping "Keep last"
//   Crash recovery           the unsaved show is written aside every few seconds while it is dirty; after an unexpected
//                            exit the home screen says so and offers it back as a NEW show (nothing is overwritten)
//                            with one click on the notice — the app does NOT open the editor over whatever you were
//                            doing when it starts
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
    // The copy an unexpected exit left behind, OFFERED but not opened: null once
    // it has been taken up (or when there is nothing to offer). Held so the
    // notice's own click can do the work later — see recover().
    property var pendingRecoveryOffer: null

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
    // The recovery notice's one click, registered once: the card draws a button
    // from the notice's own action id and this is what that id runs (the
    // notification layer stays data-driven — see NotificationCenter.registerAction).
    Component.onCompleted: {
        NotificationCenter.registerAction("recovery.open", function() { root.openOfferedRecovery() })
        SettingsService.whenReady(function() { startTimer.start() })
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

        // Something an unexpected exit left behind comes first — as an OFFER, not
        // as a takeover. Recovering used to open the copy on the Edit screen
        // before the app had even settled, which hijacked the launch: you asked
        // for the home screen and got somebody else's session back on the canvas,
        // with the only thing telling you why being a toast you had already
        // missed. The offer says what is waiting (on whatever screen you are on
        // — the home screen, normally) and carries the single click that opens
        // it; nothing is opened until you take it, and the copy stays put if you
        // do not.
        const waiting = SettingsService.pendingRecovery()
        if (waiting.path !== undefined) {
            if (root.values["backups.crashRecovery"] === true)
                root.offerRecovery(waiting)
            else
                SettingsService.clearRecovery()
        }

        const restoring = root.values["preferences.restoreLastSession"] === true
        const wantsShow = root.values["startup.openLastProject"] === true || (restoring && lastView === "edit")
        // WHERE you were, not just which tab: a template/overlay edit was an open
        // DESIGN, and the branch below reopened the last SHOW instead — so
        // quitting from a template edit came back as an unrelated show on the
        // canvas in edit mode ("it will automatically open the first project in
        // edit mode"). The design is tried first; a design that is gone (deleted
        // meanwhile) falls through to the show/screen restore as before.
        const design = restoring && lastView === "edit" ? root.designToRestore() : null
        if (design !== null && root.editScreen.openDesign(design.kind, design.id)) {
            root.window.currentView = "edit"
        } else if (wantsShow && SettingsService.fileExists(lastShow)) {
            root.editScreen.openShowPath(lastShow)
            root.window.currentView = restoring && lastView !== "" ? lastView : "edit"
        } else if (restoring && (lastView === "show" || lastView === "edit" && ShowService.hasShow)) {
            root.window.currentView = lastView
        }
        // ...and the PAGE inside Settings, when that is where the session ended:
        // it is remembered while the dialog is open and cleared the moment it
        // closes (Main.qml's rememberSettingsPage), so an empty value means
        // "Settings was shut" and nothing opens. With it set, the resume lands
        // exactly where the user was — the Show or Edit screen behind it AND the
        // page in front (Settings · Audio & Video, say) — instead of the home
        // screen, and without a trip back through the nav rail to find it.
        if (restoring) {
            const section = String(root.values["session.lastSettingsSection"] ?? "")
            if (section !== "") {
                root.window.openSettings(section)
                EngineBridge.log("info", "Navigation", "Resumed Settings \u00b7 " + section)
            }
        }
        root.started = true
    }

    // { kind, id } of the design the last session was editing, or null when that
    // session ended on a show ("" is the recorded "no design"). openDesign is
    // the one call that resolves the id against its library (and refuses one that
    // is no longer there), so nothing is checked twice here.
    function designToRestore() {
        const saved = String(root.values["session.lastDesign"] ?? "")
        const cut = saved.indexOf(":")
        if (cut <= 0 || cut === saved.length - 1)
            return null
        return { kind: saved.slice(0, cut), id: saved.slice(cut + 1) }
    }

    // The offer itself: one notice, on the screen the app started on, whose name
    // in bold is the show it is about (see NotificationToast.crux) and whose one
    // button opens it. Raised instead of opening the editor, so a launch that has
    // work waiting is still the launch the user asked for.
    function offerRecovery(waiting) {
        root.pendingRecoveryOffer = waiting
        const target = qsTr("%1 (recovered)").arg(waiting.showName)
        // Level "error" on purpose: this is a fault report about work that is
        // sitting unsaved, and it is the only notice in the app whose job is to
        // tell the user something real is recoverable. NotificationCenter lets
        // errors through even with notifications switched off or Lock In Mode on
        // (every other notice is dropped then), which is the one case where
        // dropping this offer would lose the user's work silently.
        EventBus.notify(qsTr("The app closed unexpectedly. Your unsaved changes to “%1” were written aside as “%2” — open them?")
                            .arg(waiting.showName).arg(target),
                        "error", qsTr("Crash recovery"), "settings.recovery.available",
                        [waiting.showName, target],
                        { id: "recovery.open", label: qsTr("Open recovered show") })
        // Same reason the boot and missing-device notices log: the offer is a
        // toast, the toast is gone in fifteen seconds, and "what did it say was
        // waiting?" has to stay answerable — including in a build nobody was
        // watching.
        EngineBridge.log("error", "Crash recovery",
                         qsTr("Offered the unsaved changes to \"%1\" as \"%2\" (not opened — one click on the notice opens it)")
                             .arg(waiting.showName).arg(target))
    }

    // The offer, taken up: brings the interrupted show back as a new file in the
    // library, named after it, and opens it — the same work start() used to do on
    // its own, now happening because someone clicked. Answers false only when the
    // copy cannot be opened, and says so rather than failing silently, because by
    // then the user has asked for it.
    function openOfferedRecovery() {
        const waiting = root.pendingRecoveryOffer
        if (waiting === null)
            return false
        const target = ShowService.newLibraryShowPath("", qsTr("%1 (recovered)").arg(waiting.showName))
        if (target === "" || !SettingsService.restoreRecovery(target)) {
            EventBus.notify(qsTr("The recovered copy of “%1” could not be opened any more.")
                                .arg(waiting.showName),
                            "error", qsTr("Crash recovery"), "settings.recovery.failed")
            return false
        }
        root.pendingRecoveryOffer = null
        root.editScreen.openShowPath(target)
        root.window.currentView = "edit"
        EngineBridge.log("info", "Crash recovery",
                         qsTr("Opened the recovered copy as \"%1\"")
                             .arg(qsTr("%1 (recovered)").arg(waiting.showName)))
        EventBus.notify(qsTr("“%1” was opened as “%2” — the recovered copy.")
                            .arg(waiting.showName).arg(qsTr("%1 (recovered)").arg(waiting.showName)),
                        "success", qsTr("Recovered"), "settings.recovery.restored")
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
    // The design being edited is part of "where you were" (see start()): the open
    // template/overlay rides the settings store the same way the screen and the
    // show do, so the edit that was interrupted comes back AS that design rather
    // than as whatever show was open under it. Closing it records "no design", so
    // a session that ends on a show can never come back into a stale template.
    // Both signals matter: designMode flips on open/close, while opening ANOTHER
    // design on top of the first leaves it true and only moves designId.
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

    // The app is really going away: nothing is waiting to be recovered — UNLESS an
    // offer this run was never answered. That copy is still the only record of
    // work the user has not opened yet, and a clean exit is not their answer to
    // it, so it stays for the next launch's offer (the engine clears it as soon
    // as the recovered show is saved, by the normal rule).
    function cleanExit() {
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
