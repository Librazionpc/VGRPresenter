import QtQuick

// One item added via the Edit screen's "+" Add Content menu. A real QtObject
// (not a plain JS object) specifically so that mutating .x/.y/.width/.height
// directly — exactly what applyCanvasMove/applyCanvasResize in EditScreen.qml
// already do for the fixed title/verse/ref/date/camera objects — fires a
// real property-changed signal. A Repeater delegate's one-way `x: modelData.x`
// binding needs that signal to notice the change; a plain object literal
// mutated in place would update silently with nothing to react to it.
QtObject {
    id: item

    property string key: ""
    property string kind: "text"
    property string text: ""
    property real x: 0
    property real y: 0
    property real width: 160
    property real height: 40
    property CanvasItemStyle style: CanvasItemStyle { kind: item.kind }
}
