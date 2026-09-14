import QtQuick

Rectangle {
    id: vGRPresenter_Settings_Search

    height: 900
    width: 1440

    clip: true
    color: "#0e0f14"

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
        source: Qt.resolvedUrl("assets/modal_5.png")
    }
}