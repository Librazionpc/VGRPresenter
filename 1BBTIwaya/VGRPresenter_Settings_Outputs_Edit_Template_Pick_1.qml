import QtQuick

Rectangle {
    id: vGRPresenter_Settings_Outputs_Edit_Template_Pick

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
        source: Qt.resolvedUrl("assets/modal_22.png")
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
        source: Qt.resolvedUrl("assets/style_dialog_4.png")
    }
    Rectangle {
        id: pick_scrim

        height: 900
        width: 1440

        color: "#a80a0b10"
    }
    Image {
        id: picker_dialog

        x: 316
        y: 150

        clip: true
        source: Qt.resolvedUrl("assets/picker_dialog_1.png")
    }
}