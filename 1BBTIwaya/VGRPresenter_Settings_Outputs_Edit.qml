import QtQuick

Rectangle {
    id: vGRPresenter_Settings_Outputs_Edit

    height: 900
    width: 1440

    clip: true
    color: "#0d0e13"

    Rectangle {
        id: backdrop_scrim

        height: 900
        width: 1440

        color: "#ad0a0b10"
    }
    Image {
        id: modal

        x: 136
        y: 110

        clip: true
        source: Qt.resolvedUrl("assets/modal_9.png")
    }
    Rectangle {
        id: dlg_scrim

        height: 900
        width: 1440

        color: "#b80a0b10"
    }
    Image {
        id: style_dialog

        x: 366
        y: -20

        clip: true
        source: Qt.resolvedUrl("assets/style_dialog.png")
    }
}