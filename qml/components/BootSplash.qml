import QtQuick
import VGRPresenterUI

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