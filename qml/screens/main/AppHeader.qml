import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

Rectangle {
    id: root
    objectName: "selfTestAppHeader"

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
    // Whether the Edit tab has anything to open (ViewTabs greys it when not).
    property bool editAvailable: true
    signal tabSelected(string tab)
    // The gear — the entry point to the Settings dialog (Main.qml owns the
    // dialog itself and shows it on this signal).
    signal settingsClicked()
    // The search button at the left end of the tab row.
    signal searchClicked()

    // Give the navigation toolbar its own pane shade, distinct from the
    // title/menu strip above it.
    Rectangle {
        x: 0
        y: root.titleRowHeight
        width: root.width
        height: root.tabRowHeight
        color: Theme.workspacePaneBg
    }

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

    readonly property bool live: LiveOutputService.live

    Item {
        id: goLiveButton
        objectName: "selfTestGoLive"
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        width: Math.max(goLiveLabel.implicitWidth, pillGoLive.width) + 12
        height: 52

        Rectangle {
            id: pillGoLive
            anchors.horizontalCenter: parent.horizontalCenter
            y: 4
            width: 60
            height: 30
            radius: 8
            color: root.live ? Theme.liveBgOnAir : Theme.liveBg
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            HoverGlow {
                anchors.fill: parent
                radius: parent.radius
                hovered: goLiveArea.hovered
                glowColor: Qt.alpha(Theme.controlInk, 0.16)
            }

            // A ring with a dot; on air the dot pulses (a quiet "this is live" tell).
            Rectangle {
                anchors.centerIn: parent
                width: 18; height: 18; radius: 9
                color: "transparent"
                border.width: 2
                border.color: root.live ? Theme.dangerLight : Theme.controlInk
                Rectangle {
                    anchors.centerIn: parent
                    width: 6; height: 6; radius: 3
                    color: root.live ? Theme.dangerLight : Theme.controlInk
                    SequentialAnimation on opacity {
                        running: root.live
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.25; duration: 700 }
                        NumberAnimation { to: 1.0; duration: 700 }
                    }
                }
            }

            PositionHoverArea {
                id: goLiveArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.live ? LiveOutputService.stop() : LiveOutputService.goLive()
            }
            HoverDetail {
                anchors.fill: parent
                title: root.live ? qsTr("Stop live output") : qsTr("Go live")
                detail: root.live
                        ? qsTr("Stop sending the current program to live outputs")
                        : qsTr("Send the staged program to live outputs")
            }
        }
        Text {
            id: goLiveLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 32
            text: root.live ? qsTr("STOP") : qsTr("GO LIVE")
            color: root.live ? Theme.dangerLight : Theme.controlInk
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
            textFormat: Text.PlainText
        }
    }

    ViewTabs {
        id: viewTabs
        // Second row, centred under the title bar (and vertically in the row).
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.titleRowHeight + (root.tabRowHeight - height) / 2
        activeTab: root.activeTab
        // Passed straight through from Main: whether Edit has a show to open
        // (see ViewTabs.editAvailable).
        editAvailable: root.editAvailable
        onTabSelected: (tab) => root.tabSelected(tab)
    }

    // Separate the title/menu row from the lower navigation toolbar.
    Rectangle {
        x: 0
        y: root.titleRowHeight - 1
        width: root.width
        height: 1
        color: Theme.border
    }

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
            color: "transparent"
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            HoverGlow {
                anchors.fill: parent
                radius: parent.radius
                hovered: searchHover.hovered
                glowColor: Theme.hoverBg
            }

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
                property color stroke: Theme.controlInk
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
            HoverDetail {
                anchors.fill: parent
                title: qsTr("Search")
                detail: qsTr("Find shows, media, scripture, and other content")
            }
        }
        Text {
            id: searchLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 32
            text: qsTr("Search")
            color: Theme.controlInk
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
            textFormat: Text.PlainText
        }
    }

    WindowControls {
        id: windowButtons
        objectName: "selfTestCaptionButtons"
        anchors.right: parent.right
        anchors.top: parent.top
    }

    readonly property real captionClusterWidth: windowButtons.width
    readonly property real captionClusterHeight: windowButtons.height

    HeaderStatus {
        id: status
        // The window buttons live in WindowControls now (real ones), not in this cluster.
        anchors.right: windowButtons.left
        anchors.rightMargin: 8
        y: 9
    }

    FeedStallPill {
        id: feedPill
        anchors.right: status.left
        anchors.rightMargin: 16
        y: status.y + 7 - height / 2
    }

    Item {
        id: settingsButton
        objectName: "selfTestSettingsTile"
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
            color: "transparent"
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            HoverGlow {
                anchors.fill: parent
                radius: parent.radius
                hovered: gearHover.hovered
                glowColor: Theme.hoverBg
            }

            Item {
                id: gearGlyph
                anchors.centerIn: parent
                width: 24
                height: 24

                IconGlyph {
                    readonly property real enlarge: 1.7
                    anchors.centerIn: parent
                    width: 9
                    height: 9.98
                    scale: enlarge
                    strokeWidth: 2 / enlarge
                    name: "settings"
                    color: Theme.controlInk
                }
            }
            PositionHoverArea {
                id: gearHover
                anchors.fill: parent
                onClicked: root.settingsClicked()
            }
            HoverDetail {
                anchors.fill: parent
                title: qsTr("Settings")
                detail: qsTr("Configure outputs, styles, audio, and app preferences")
            }
        }
        Text {
            id: settingsLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: 32
            text: qsTr("Settings")
            color: Theme.controlInk
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
            textFormat: Text.PlainText
        }
    }
}