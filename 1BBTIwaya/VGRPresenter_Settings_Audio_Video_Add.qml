import QtQuick

Rectangle {
    id: vGRPresenter_Settings_Audio_Video_Add

    height: 900
    width: 1440

    clip: true
    color: "#0e0f14"

    Rectangle {
        id: backdrop_scrim

        height: 900
        width: 1440

        color: "#0a0b10"
    }
    Image {
        id: modal

        x: 136
        y: 110

        clip: true
        source: Qt.resolvedUrl("assets/modal_11.png")
    }
    Rectangle {
        id: addsrc_scrim

        height: 900
        width: 1440

        color: "#b80a0b10"
    }
    Image {
        id: addsrc_dialog

        x: 366
        y: 18

        clip: true
        source: Qt.resolvedUrl("assets/addsrc_dialog.png")
    }
}