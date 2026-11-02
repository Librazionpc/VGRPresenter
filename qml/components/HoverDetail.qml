import QtQuick
import VGRPresenterUI

// ============================================================================
Item {
    id: root

    // The wrapped thing. Any children: `HoverDetail { IconGlyph { ... } }`.
    default property alias content: contentItem.data

    property string title: ""
    property string detail: ""
    readonly property string tipText: root.title !== "" && root.detail !== ""
                                      ? root.title + " · " + root.detail
                                      : (root.title !== "" ? root.title : root.detail)
    // A surface that has a tip but must not offer it right now (hovering while
    // a dialog is simply not the moment) turns this off.
    property bool tipEnabled: true
    property bool alwaysDelay: false

    // The wrapped control usually has its own cursor claim; leave that alone.
    // A dot or a bare label that wants the explaining hand sets this.
    property int cursorShape: Qt.ArrowCursor
    // Take the click as well as the hover? Off by default: the content keeps
    // its own MouseArea (see the header).
    property bool takeClicks: false
    property int hoverPadding: 4

    implicitWidth: contentItem.implicitWidth
    implicitHeight: contentItem.implicitHeight

    Item {
        id: contentItem
        anchors.fill: parent
    }

    PositionHoverArea {
        id: tipArea
        anchors.fill: parent
        anchors.margins: -root.hoverPadding
        showCursor: root.cursorShape !== Qt.ArrowCursor
        cursorShape: root.cursorShape
        clickEnabled: root.takeClicks
        onEntered: if (root.tipEnabled && root.tipText !== "") TipService.show(root.tipText, root, root.alwaysDelay)
        onExited: TipService.hide(root)
    }

    onTitleChanged: refreshTip()
    onDetailChanged: refreshTip()
    onTipEnabledChanged: refreshTip()
    function refreshTip() {
        if (!tipArea.hovered)
            return
        if (root.tipEnabled && root.tipText !== "")
            TipService.show(root.tipText, root, root.alwaysDelay)
        else
            TipService.hide(root)
    }

    // A recycled delegate unloads mid-hover: a tip must not outlive its control.
    Component.onDestruction: TipService.forget(root)
}