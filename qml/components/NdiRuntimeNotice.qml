import QtQuick
import VGRPresenterUI

// "NDI needs its runtime" notice — shown wherever an NDI source can be picked
// (the Add/Edit source dialogs) and in Settings · Plugins when the NDI runtime
// isn't usable on this machine. NDI is a separate, vendor-licensed install that
// VGR can't bundle, so instead of a dead-end "not enabled" row the user gets
// the reason plus a button straight to the download page, and a re-check
// button for after they've installed it (no app restart needed).
//
// Hidden when NDI is ready (or the engine hasn't booted yet, so a healthy
// machine never sees a flash of warning). `active` lets the consumer scope it
// (e.g. only while the NDI kind chip is selected).
Rectangle {
    id: root

    // Consumer gate — the notice only matters while NDI is what's being set up.
    property bool active: true

    readonly property string ndiState: EngineBridge.ndiState
    readonly property bool problem: root.ndiState === "notInstalled" || root.ndiState === "error"

    visible: root.active && root.problem
    height: root.visible ? col.height + 24 : 0
    radius: Theme.radiusMd
    color: "#1af5c26b"
    border.width: 1
    border.color: "#4df5c26b"

    Column {
        id: col
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 8

        Text {
            width: parent.width
            text: root.ndiState === "notInstalled" ? qsTr("NDI runtime not installed")
                                                : qsTr("NDI isn't working")
            color: Theme.warning
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
        }

        Text {
            width: parent.width
            text: root.ndiState === "notInstalled"
                  ? qsTr("VGR Presenter uses the free NDI runtime to find and receive video from other computers and apps on your network. Install it, then check again — no restart needed.")
                  : EngineBridge.ndiStatus
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }

        Flow {
            width: parent.width
            spacing: 8

            AppButton {
                visible: root.ndiState === "notInstalled"
                text: qsTr("Download NDI runtime")
                onClicked: EngineBridge.openNdiDownloadPage()
            }
            AppButton {
                text: root.ndiState === "notInstalled" ? qsTr("I've installed it — check again")
                                                    : qsTr("Check again")
                variant: "secondary"
                onClicked: EngineBridge.recheckNdi()
            }
        }
    }
}
