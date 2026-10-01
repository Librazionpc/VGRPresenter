import QtQuick
import VGRPresenterUI

// The boot splash — the first face of the app, matching the brand sheet:
// the hexagon-play logo tile, the VGR (white) / Presenter (grey) wordmark,
// the "Expressive · memorable · presenter-first" tagline pill and a thin
// booting bar.
//
// Why this exists: the engine used to boot synchronously BEFORE any QML
// loaded, so the screen stayed blank for the whole kernel boot. Boot is now
// DEFERRED (main.cpp fires it once this splash has painted), and this
// overlay is what the user sees instead of a dead desktop: the kernel boots
// behind it, and the app only opens when the engine has settled.
//
// Reveal contract: done means the boot RETURNED — success (EngineBridge.
// booted) or FAILURE (EngineBridge.bootError). A failure must still bring
// the splash down: the UI is designed to keep working against mock data,
// and the error surfaces as its own toast. Minimum on-screen time (1.2 s)
// stops a fast boot from strobing the splash for a single frame.
//
// Holding still is by design: boot() runs ON the GUI thread (EngineBridge.h
// forbids a worker-thread boot), so this splash freezes for the boot's
// duration — the logo, wordmark and tagline are static, so nothing looks
// broken mid-freeze.
//
// Declared LAST in Main.qml at the top z: nothing behind it (screens,
// modals, even toasts) paints or takes input during boot.
Rectangle {
    id: root

    // The backdrop AROUND the splash card (black — the card below carries the
    // brand surface). Still a full-window cover: nothing behind it paints or
    // takes input during boot.
    color: "#000000"

    // ---- Reveal state machine ------------------------------------------
    readonly property bool done: EngineBridge.booted
                                 || EngineBridge.bootError.length > 0
    property bool minTimeMet: false
    property bool revealing: false
    property bool hidden: false

    // FULLY OPAQUE FROM THE FIRST FRAME — deliberately no entrance fade.
    // The boot blocks the GUI thread ~200 ms after this loads (main.cpp),
    // and a 280 ms fade-in froze half-done at that point: the live UI showed
    // through a ~70%-opaque splash for the whole boot (the "splash doesn't
    // cover the app" report). Only the hand-over fades — by then the loop is
    // alive again, so that animation actually runs.
    opacity: 1
    visible: !hidden
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

    // Input sink: nothing behind the splash may react during boot.
    MouseArea {
        anchors.fill: parent
    }

    // The splash CARD — the user's size reference (678×378), centered on the
    // display. Card color stays the brand asset's own page background
    // (rgb 17,19,27): the logo tile's rounded corners blend into it, no halo.
    Rectangle {
        anchors.centerIn: parent
        width: 678
        height: 378
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

            // ---- Boot status + thin indeterminate bar. During the boot itself
            // these hold still (GUI-thread boot — see the header); the "Ready"
            // beat before the fade proves it was worth the wait.
            Column {
                width: 190
                anchors.horizontalCenter: parent.horizontalCenter
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
}
