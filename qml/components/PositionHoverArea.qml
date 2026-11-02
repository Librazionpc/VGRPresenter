import QtQuick
import VGRPresenterUI

Item {
    id: root

    // True while the pointer is over the area. Bind visuals to this exactly
    // like the old containsMouse.
    property bool hovered: false
    property bool checkAncestors: true
    // Cursor pushed to AppCursor while hovered. Set showCursor: false on a
    // hover-only surface that must not claim the cursor.
    property bool showCursor: true
    property int cursorShape: Qt.PointingHandCursor
    property int cursorPriority: AppCursor.priorityHover
    // Hover-only overlays should let the actual control below them receive clicks.
    property bool clickEnabled: true

    signal entered()
    signal exited()
    signal clicked(var mouse)
    signal doubleClicked(var mouse)

    function chainShown() {
        if (!root.visible || root.enabled === false)
            return false
        if (!checkAncestors)
            return true
        let o = root.parent
        while (o) {
            if (o.visible === false || o.enabled === false)
                return false
            o = o.parent
        }
        return true
    }

    function _recompute() {
        const inside = AppCursor.windowHovered && root.chainShown()
                       && AppCursor.hovered(root) && !AppCursor.blocked(root)
        if (inside !== root.hovered) {
            root.hovered = inside
            if (inside)
                root.entered()
            else
                root.exited()
        }
    }

    onVisibleChanged: _recompute()
    onEnabledChanged: _recompute()

    Timer {
        interval: 250
        repeat: true
        running: root.hovered
        onTriggered: root._recompute()
    }

    onHoveredChanged: _syncCursor()
    onShowCursorChanged: _syncCursor()
    onCursorShapeChanged: _syncCursor()
    function _syncCursor() {
        if (root.hovered && root.showCursor)
            AppCursor.push(root.cursorShape, root, root.cursorPriority)
        else
            AppCursor.pop(root)
    }

    Connections {
        target: AppCursor
        function onPointerMoved() { root._recompute() }
        // A popup opening/closing over this area must settle it immediately —
        // the pointer may be perfectly still over the menu.
        function onBlockersChanged() { root._recompute() }
    }

    Component.onCompleted: _recompute()
    Component.onDestruction: AppCursor.pop(root)

    MouseArea {
        anchors.fill: parent
        enabled: root.clickEnabled
        hoverEnabled: false
        onClicked: (mouse) => root.clicked(mouse)
        onDoubleClicked: (mouse) => root.doubleClicked(mouse)
    }
}