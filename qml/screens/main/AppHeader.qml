import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The app's TITLE BAR + tab strip, in two rows. The window is
// frameless, so the top row (32 px) is the title bar: the logo + File/Edit/View/Help
// menu (AppMenuBar, drawn over its left side), the connection status and the window's
// own minimize/maximize/close buttons. The second row, under the line, holds the
// Search button (far left), the Show/Edit/Stage tabs (centre) and GO LIVE at the far right
// (with Settings just inside it). Drag the empty strip to move the
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
// Colors read the Theme singleton (title-bar ground, borders, icon and text
// tones), so the header recolours with the rest of the app when Light is
// chosen. It carried literal hex here only while Theme was registered as an
// ordinary type and `Theme.*` resolved to undefined — a registration bug now
// fixed in CMakeLists.txt (QT_QML_SINGLETON_TYPE), not an AOT/nesting limit.
Rectangle {
    id: root

    // Title row + tab row. Main.qml's headerHeight must match (it pushes
    // the screens down by the difference from the original 48 px).
    readonly property int titleRowHeight: 32
    readonly property int tabRowHeight: 52
    height: titleRowHeight + tabRowHeight
    width: 1440

    border.color: Theme.border
    border.width: 1
    color: Theme.panelBg

    property string activeTab: "show"
    signal tabSelected(string tab)
    // The gear — the entry point to the Settings dialog (Main.qml owns the
    // dialog itself and shows it on this signal).
    signal settingsClicked()
    // The search button at the left end of the tab row.
    signal searchClicked()

    // Window drag / maximize by the strip itself. Declared FIRST — z-order is
    // declaration order, so every control below (GO LIVE, tabs, Search,
    // Settings, window buttons) stacks ABOVE this and takes its own clicks
    // before this sees them. Declared after the GO LIVE button once, the
    // full-header drag layer buried it and the button stopped responding.
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

    // GO LIVE holds the tab row's EXTREME right end, with Settings directly
    // beside it on its left (user call — the two used to be the other way
    // round). Reads the shared live service; no signal needed — it calls it
    // directly.
    readonly property bool live: LiveOutputService.live

    // The one on-air switch: red pill, GO LIVE / STOP (the transport arrows
    // and Clear stay on the monitor wall — this is the global switch).
    // Same icon-pill-above/caption-below layout as Search and Settings
    // (below) — was icon-left/text-right, the odd one out in the row.
    Item {
        id: goLiveButton
        // The end of the row is the live switch's now, so this is the item
        // PAIRED with parent.right and Settings chains off it. (Anchoring each
        // to the other's left is a binding loop — neither would have a fixed
        // edge to resolve from.)
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        width: Math.max(goLiveLabel.implicitWidth, pillGoLive.width) + 12
        height: 52

        function baseColor() {
            return root.live ? Theme.liveBgOnAir : Theme.liveBg
        }

        Rectangle {
            id: pillGoLive
            anchors.horizontalCenter: parent.horizontalCenter
            // Same vertical centring as pillSearch/pillGear below.
            y: 4
            width: 60
            height: 30
            radius: 8
            color: goLiveArea.containsMouse ? Qt.lighter(goLiveButton.baseColor(), 1.15)
                                            : goLiveButton.baseColor()
            Behavior on color { ColorAnimation { duration: 120 } }

            Rectangle {
                anchors.centerIn: parent
                width: 8; height: 8; radius: 4
                color: root.live ? Theme.dangerLight : "#ffffff"
                // On air: the dot pulses (a quiet "this is live" tell).
                SequentialAnimation on opacity {
                    running: root.live
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.25; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                }
            }

            PositionHoverArea {
                id: goLiveArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.live ? LiveOutputService.stop() : LiveOutputService.goLive()
            }
        }
        Text {
            id: goLiveLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 32
            text: root.live ? qsTr("STOP") : qsTr("GO LIVE")
            color: root.live ? Theme.dangerLight : Theme.textPrimary
            font.family: "Segoe UI"
            font.pixelSize: 13
            font.weight: Font.DemiBold
            font.letterSpacing: 0.5
            textFormat: Text.PlainText
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
            // Vertically centred as a unit with the caption below: pill 4..34,
            // label glyphs ~37.5..48.5 — even margins in the 52 px row.
            y: 4
            width: 60
            height: 30
            radius: 8
            color: searchHover.hovered ? Theme.hoverBg : "transparent"
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
                property color stroke: searchHover.hovered ? Theme.textPrimary : Theme.iconChrome
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
            y: 32
            text: qsTr("Search")
            color: searchHover.hovered ? Theme.textPrimary : Theme.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 15
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
        anchors.right: windowButtons.left
        anchors.rightMargin: 8
        y: 9
    }

    // Settings - second from the right now (GO LIVE took the end), still mirroring Search on
    // the left: the gear in a pill is the click target, the label a caption under it. Opens the
    // Settings dialog (Main.qml owns the dialog).
    Item {
        id: settingsButton
        anchors.right: goLiveButton.left
        anchors.rightMargin: 18
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        width: Math.max(settingsLabel.implicitWidth, pillGear.width) + 12
        height: 52

        Rectangle {
            id: pillGear
            anchors.horizontalCenter: parent.horizontalCenter
            // Same vertical centring as pillSearch above.
            y: 4
            width: 60
            height: 30
            radius: 8
            // Position truth - containsMouse latches forever in this build
            // (KNOWN_ISSUES.md).
            color: gearHover.hovered ? Theme.hoverBg : "transparent"
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
                    color: gearHover.hovered ? Theme.textPrimary : Theme.iconChrome
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
            y: 32
            text: qsTr("Settings")
            color: gearHover.hovered ? Theme.textPrimary : Theme.textSecondary
            font.family: "Segoe UI"
            font.pixelSize: 15
            font.bold: true
            textFormat: Text.PlainText
        }
    }
}
