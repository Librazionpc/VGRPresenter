import QtQuick

QtObject {
    id: item

    property string key: ""
    property string kind: "text"
    property string text: ""
    // Templates only: which slide field this block shows ("title" | "text" | "line1" | "line2" | "ref" | "notes"),
    // "" = static content. It is the engine block's own `bind`; the canvas keeps it so an edit never drops it.
    property string bind: ""
    property real x: 0
    property real y: 0
    property real width: 160
    property real height: 40
    property CanvasItemStyle style: CanvasItemStyle { kind: item.kind }
    property var meta: ({})
}