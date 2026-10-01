import QtQuick
import VGRPresenterUI

// The boot splash — the first face of the app, matching the brand sheet:
// the hexagon-play logo tile, the VGR (white) / Presenter (grey) wordmark
// and a thin booting bar. NO full-window backdrop: the splash IS the card —
// a small frameless always-on-top window centered on the screen (the classic
// Photoshop-style splash), floating over the app while the engine boots.
// The user asked for exactly this: "remove the bg filler, leave the model
// alone" — the card (its logo, wordmark, status line and bar) is untouched;
// only the black Rectangle backdrop is gone.
//
// Why this exists: the engine used to boot synchronously BEFORE any QML
// loaded, so the screen stayed blank for the whole kernel boot. Boot is now
// DEFERRED (main.cpp fires it once this splash has painted), and this
// floating card is what the user sees instead of a dead desktop: the kernel
// boots behind it, and the app only opens when the engine has settled.
//
// Reveal contract: done means the boot RETURNED — success (EngineBridge.
// booted) or FAILURE (EngineBridge.bootError). A failure must still bring
// the splash down: the UI is designed to keep working against mock data,
// and the error surfaces as its own toast. Minimum on-screen time (1.2 s)
// stops a fast boot from strobing the splash for a single frame.
//
// Holding still is by design: boot() runs ON the GUI thread (EngineBridge.h
// forbids a worker-thread boot), so this splash freezes for the boot's
// duration — the logo, wordmark and status line are static, so nothing
// looks broken mid-freeze.
Window {
    id: root

    // The card's size IS the splash's size (678×378, the brand sheet's
    // reference) — the window hugs the card, no backdrop around it.
    width: 678
    height: 378
    flags: Qt.SplashScreen | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    // ---- Reveal state machine ------------------------------------------
    readonly property bool done: EngineBridge.booted
                                 || EngineBridge.bootError.length > 0
    property bool minTimeMet: false
    property bool revealing: false
    property bool hidden: false

    // FULLY OPAQUE FROM THE FIRST FRAME — deliberately no entrance fade.
    // The boot blocks the GUI thread ~200 ms after this loads (main.cpp),
    // and a fade-in would freeze half-done. Only the hand-over fades — by
    // then the loop is alive again, so that animation actually runs.
    opacity: 1
    visible: true
    Behavior on opacity {
        NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
    }

    onOpacityChanged: if (root.revealing && root.opacity === 0)
                          root.hidden = true   // cut after the fade, not during

    Timer {
        id: minTimeTimer
        interval: 1200
        running: true
        onTriggered: root.minTimeMet = true
    }

    // Both conditions can complete in either order — either flip may be the
    // one that unlocks the reveal.
    onDoneChanged: if (root.done && root.minTimeMet) root.beginReveal()
    onMinTimeMetChanged: if (root.done && root.minTimeMet) root.beginReveal()

    function beginReveal() {
        if (root.revealing)
            return
        root.revealing = true
        hideTimer.start()
    }

    // Hold the "Ready" beat so the hand-over feels deliberate, not a flash.
    Timer {
        id: hideTimer
        interval: EngineBridge.bootError.length > 0 ? 900 : 450
        onTriggered: root.opacity = 0
    }

    onHiddenChanged: if (root.hidden) root.close()

    // Centered on the SCREEN the app's main window is on (a QML Window with
    // no explicit x/y lands wherever the OS puts it — pin it to the primary
    // screen's centre so the splash reads as part of the app, not a stray
    // window on a second monitor). Recomputed on screen changes (a display
    // hotplug between launches).
    function recenter() {
        const scr = Qt.application.screens[0] ?? null
        if (!scr)
            return
        x = scr.virtualX + (scr.width - width) / 2
        y = scr.virtualY + (scr.height - height) / 2
    }
    Component.onCompleted: root.recenter()
    onWidthChanged: root.recenter()
    onHeightChanged: root.recenter()

    // The splash CARD — the whole splash now (678×378, no backdrop behind
    // it). Card color stays the brand asset's own page background
    // (rgb 17,19,27): the logo tile's rounded corners blend into it, no halo.
    Rectangle {
        id: card
        anchors.fill: parent
        radius: 24
        color: "#11131b"

        Column {
            anchors.centerIn: parent
            spacing: 22

            // ---- The lockup on ONE line: logo IN FRONT, then the wordmark
            // (VGR white/bold + Presenter grey/lighter). The REAL brand
            // artwork (qml/assets/logo-mark.png), not a redrawn approximation.
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16

                Image {
                    width: 64
                    height: 64
                    anchors.verticalCenter: parent.verticalCenter
                    source: "../assets/logo-mark.png"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1

                    Text {
                        text: "VGR"
                        color: "#f2f3f7"
                        font.family: Theme.fontFamily
                        font.pixelSize: 40
                        font.weight: Font.Bold
                    }
                    Text {
                        text: "Presenter"
                        color: "#8a8fa0"
                        font.family: Theme.fontFamily
                        font.pixelSize: 40
                        font.weight: Font.Normal
                    }
                }
            }
        }

        // ---- Boot status + thin indeterminate bar, pinned INSIDE the card
        // below the lockup. During the boot itself these hold still (GUI-
        // thread boot — see the header); the "Ready" beat before the fade
        // proves it was worth the wait.
        Column {
            width: 190
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 34
            spacing: 12

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: EngineBridge.booted
                          ? qsTr("Ready")
                        : EngineBridge.bootError.length > 0
                          ? qsTr("Engine failed to boot — opening anyway")
                        : qsTr("Booting the presentation engine…")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.letterSpacing: 0.4
            }

            Rectangle {
                width: parent.width
                height: 3
                radius: 1.5
                color: "#1d1e28"
                clip: true

                Rectangle {
                    id: barPill
                    width: 58
                    height: parent.height
                    radius: 1.5
                    color: Theme.accent
                    SequentialAnimation on x {
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: -barPill.width
                            to: 190
                            duration: 1100
                            easing.type: Easing.InOutQuad
                        }
                    }
                }
            }
        }
    }
}
