import QtQuick
import QtQuick.Shapes

// The app's TITLE BAR + tab strip, in two rows. The window is
// frameless, so the top row (32 px) is the title bar: the logo + File/Edit/View/Help
// menu (AppMenuBar, drawn over its left side), the connection status and the window's
// own minimize/maximize/close buttons. The second row, under the line, holds the
// Search button (far left), the Show/Edit/Stage tabs (centre) and Settings (far right). Drag the empty strip to move the
// window; double-click it to maximize/restore (both handled by the OS via
// startSystemMove, so edge-snapping works).
// Shared by every screen — instantiated once in Main.qml instead of once
// per screen, so there's exactly one ViewTabs/HeaderStatus in the tree
// rather than a separate copy behind each screen.
//
// The logo + File/Edit/View/Help menu (AppMenuBar) is deliberately NOT
// drawn here: it needs to sit above every screen (for its dropdowns and
// click-outside catcher to work regardless of which screen is showing), so
// Main.qml instantiates it separately as the topmost layer.
//
// The right-hand cluster is anchored to the window's right edge (window buttons, then
// status), so it stays put at any window width; the tabs stay centred.
//
// Literal colors, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml at this nesting depth.
Rectangle {
    id: root

    // Title row + tab row. Main.qml's headerHeight must match (it pushes
    // the screens down by the difference from the original 48 px).
    readonly property int titleRowHeight: 32
    readonly property int tabRowHeight: 52
    height: titleRowHeight + tabRowHeight
    width: 1440

    border.color: "#232530"
    border.width: 1
    color: "#12131a"

    property string activeTab: "show"
    signal tabSelected(string tab)
    // The gear — the entry point to the Settings dialog (Main.qml owns the
    // dialog itself and shows it on this signal).
    signal settingsClicked()
    // The search button at the left end of the tab row.
    signal searchClicked()

    // Window drag / maximize by the strip itself. Declared FIRST so every control above
    // it (menu labels, tabs, gear, window buttons) takes its own clicks before this sees
    // them.
    Item {
        anchors.fill: parent

        DragHandler {
            target: null
            onActiveChanged: {
                // Only while windowed: a maximized window is restored by double-click.
                if (active && root.Window.window && root.Window.window.visibility === Window.Windowed)
                    root.Window.window.startSystemMove()
            }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: windowButtons.toggleMaximize()
        }
    }

    ViewTabs {
        // Second row, centred under the title bar (and vertically in the row).
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        activeTab: root.activeTab
        onTabSelected: (tab) => root.tabSelected(tab)
    }

    // Search - the extreme LEFT of the tab row. Opens the search dialog. Like the tabs, the
    // button is the icon: the pill around the magnifier is the click target and the label
    // is a caption under it.
    Item {
        id: searchButton
        x: 12
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        width: Math.max(searchLabel.implicitWidth, pillSearch.width) + 12
        height: 52

        Rectangle {
            id: pillSearch
            anchors.horizontalCenter: parent.horizontalCenter
            y: 2
            width: 60
            height: 30
            radius: 8
            color: searchHover.hovered ? "#1c1d26" : "transparent"
            Behavior on color { ColorAnimation { duration: 100 } }

            // (IconGlyph's search icon is a fixed 10.5 px, so the large one is drawn here.)
            Shape {
                id: searchGlyph
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: -0.8   // the magnifier's ink sits ~1 px right/down of its box centre
                anchors.verticalCenterOffset: -0.8
                width: 24
                height: 24
                preferredRendererType: Shape.CurveRenderer
                scale: 0.83
                property color stroke: searchHover.hovered ? "#e2e8f0" : "#b4bccb"
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: searchGlyph.stroke
                    strokeWidth: 2 / 0.83
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    PathAngleArc { centerX: 10.5; centerY: 10.5; radiusX: 7.5; radiusY: 7.5; startAngle: 0; sweepAngle: 360 }
                }
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: searchGlyph.stroke
                    strokeWidth: 2 / 0.83
                    capStyle: ShapePath.RoundCap
                    startX: 16; startY: 16
                    PathLine { x: 21.5; y: 21.5 }
                }
            }
            PositionHoverArea {
                id: searchHover
                anchors.fill: parent
                onClicked: root.searchClicked()
            }
        }
        Text {
            id: searchLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 33
            text: qsTr("Search")
            color: searchHover.hovered ? "#e2e8f0" : "#8a94a6"
            font.family: "Inter"
            font.pixelSize: 13
            font.bold: true
            textFormat: Text.PlainText
        }
    }

    WindowControls {
        id: windowButtons
        anchors.right: parent.right
        anchors.top: parent.top
    }

    HeaderStatus {
        id: status
        // The window buttons live in WindowControls now (real ones), not in this cluster.
        showWindowControls: false
        anchors.right: windowButtons.left
        anchors.rightMargin: 8
        y: 9
    }

    // Settings - the extreme RIGHT of the tab row, mirroring Search on the left: the gear in
    // a pill is the click target, the label a caption under it. Opens the Settings dialog
    // (Main.qml owns the dialog).
    Item {
        id: settingsButton
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        width: Math.max(settingsLabel.implicitWidth, pillGear.width) + 12
        height: 52

        Rectangle {
            id: pillGear
            anchors.horizontalCenter: parent.horizontalCenter
            y: 2
            width: 60
            height: 30
            radius: 8
            // Position truth - containsMouse latches forever in this build
            // (KNOWN_ISSUES.md).
            color: gearHover.hovered ? "#1c1d26" : "transparent"
            Behavior on color { ColorAnimation { duration: 100 } }

            // A 24 px box holding the gear. IconGlyph is a plain Item that needs an explicit
            // size and is drawn at ~10 px, so it is enlarged with scale (its stroke is
            // thinned to match).
            Item {
                id: gearGlyph
                anchors.centerIn: parent
                width: 24
                height: 24

                // The glyph's box is exactly the gear's own size (9 x 9.98): the icon's path
                // sits at the top-left of its box, so any larger box would shift it off-centre
                // (and the enlargement would multiply that shift).
                IconGlyph {
                    readonly property real enlarge: 1.7
                    anchors.centerIn: parent
                    width: 9
                    height: 9.98
                    scale: enlarge
                    strokeWidth: 2 / enlarge
                    name: "settings"
                    color: gearHover.hovered ? "#e2e8f0" : "#b4bccb"
                }
            }
            PositionHoverArea {
                id: gearHover
                anchors.fill: parent
                onClicked: root.settingsClicked()
            }
        }
        Text {
            id: settingsLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 33
            text: qsTr("Settings")
            color: gearHover.hovered ? "#e2e8f0" : "#8a94a6"
            font.family: "Inter"
            font.pixelSize: 13
            font.bold: true
            textFormat: Text.PlainText
        }
    }
}
