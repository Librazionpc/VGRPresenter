import QtQuick

Rectangle {
    id: vGRPresenter_Settings_General_Scrolled

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

        x: 132
        y: 110

        clip: true
        source: Qt.resolvedUrl("assets/modal_4.png")
    }
}