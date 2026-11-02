import QtQuick
import VGRPresenterUI

// The on-air overlay layer: one DesignPreview per active overlay. `underSlide` picks the
// placement group, which is also the draw order (under -> slide -> over), so a tile and a
// full output window cannot disagree about it.
Repeater {
    id: layer

    property bool underSlide: false
    property bool blacked: false

    model: blacked ? [] : LiveOutputService.activeOverlays
    delegate: DesignPreview {
        required property var modelData
        readonly property var design: OverlayLibraryService.design(modelData.id)
        anchors.fill: parent
        visible: (design.placeUnderSlide === true) === layer.underSlide
        showCheckerboard: false
        blocks: design.blocks ?? []
        background: design.background ?? "transparent"
    }
}
