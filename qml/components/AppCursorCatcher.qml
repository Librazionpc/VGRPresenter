import QtQuick
import VGRPresenterUI

Item {
    anchors.fill: parent

    HoverHandler {
        id: catcherHandler
        blocking: false
        onHoveredChanged: {
            AppCursor.windowHovered = hovered
            if (!hovered)
                AppCursor.clearPointerPos()
        }
        onPointChanged: AppCursor.setPointerPos(point.scenePosition)
    }

    function syncShape() {
        catcherHandler.cursorShape = AppCursor.active ? AppCursor.shape
                                                      : undefined
        if (AppCursor.active)
            CursorService.apply(AppCursor.shape)
        else
            CursorService.clear()
    }
    Component.onCompleted: syncShape()
    Connections {
        target: AppCursor
        function onActiveChanged() { syncShape() }
        function onShapeChanged() {
            if (AppCursor.active)
                syncShape()
        }
    }
}