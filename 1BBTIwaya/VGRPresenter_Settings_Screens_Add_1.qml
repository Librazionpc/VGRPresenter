import QtQuick

Rectangle {
    id: vGRPresenter_Settings_Screens_Add

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
        source: Qt.resolvedUrl("assets/modal_27.png")
    }
    Rectangle {
        id: add_scrim

        x: 180
        y: 130

        height: 640
        width: 1080

        color: "#ad0a0b10"
        opacity: 0.65
    }
    Image {
        id: add_dialog

        x: 366
        y: 145

        clip: true
        source: Qt.resolvedUrl("assets/add_dialog_1.png")
    }
}