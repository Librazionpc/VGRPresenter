import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

Item {
    id: root

    property string activeTab: "show" // "show" | "edit" | "stage"
    signal tabSelected(string tab)
    property bool editAvailable: true
    property bool stageAvailable: false

    height: 52
    width: 268

    component TabButton: Item {
        id: tabRoot

        property string tabKey: ""
        property string label: ""
        property string iconPath: ""
        property string glyph: ""
        property real iconShapeX: 1.75
        property real iconShapeY: 1.75
        property real iconShapeWidth: 10.50
        property real iconShapeHeight: 10.50

        readonly property bool isActive: root.activeTab === tabKey
        // A tab that has nothing to open: greyed, and it takes no hover or
        // click (see PositionHoverArea below).
        readonly property bool available: (tabRoot.tabKey !== "edit" || root.editAvailable)
                                            && (tabRoot.tabKey !== "stage" || root.stageAvailable)
        readonly property bool isHover: tabHover.hovered && tabRoot.available

        property real iconScale: 1.5
        // Filled glyph (play), the stroke width used with it, and an optional extra
        // stroke-only path (the pencil's underline).
        property bool filledIcon: false
        property real iconStroke: 2
        property string linePath: ""

        height: 52

        Rectangle {
            id: pill
            anchors.horizontalCenter: parent.horizontalCenter
            y: 4
            width: 60
            height: 30
            radius: 8
            border.width: 1
            border.color: tabRoot.isActive ? Theme.border : "transparent"
            color: tabRoot.isActive ? Theme.activeBg : "transparent"
            // Greyed when there is nothing to open, so "don't press this yet"
            // is visible before the pointer is on it.
            opacity: tabRoot.available ? 1 : 0.4
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            HoverGlow {
                anchors.fill: parent
                radius: parent.radius
                hovered: tabRoot.isHover && tabRoot.available && !tabRoot.isActive
                glowColor: Theme.hoverBg
            }

            // 24 px icon box, centred in the pill.
            Item {
                id: iconBox
                anchors.centerIn: parent
                width: 24
                height: 24

                IconGlyph {
                    visible: tabRoot.glyph !== ""
                    anchors.centerIn: parent
                    name: tabRoot.glyph
                    color: tabRoot.available && tabRoot.isActive
                           ? Theme.accent : Theme.controlInk
                    width: 18; height: 18
                    fit: true
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                }

                Shape {
                    visible: tabRoot.glyph === ""
                    width: tabRoot.iconShapeWidth
                    height: tabRoot.iconShapeHeight
                    transformOrigin: Item.TopLeft
                    scale: tabRoot.iconScale
                    x: (iconBox.width - width * scale) / 2
                    y: (iconBox.height - height * scale) / 2

                    ShapePath {
                        fillColor: tabRoot.filledIcon ? strokeColor : "#00000000"
                        strokeColor: tabRoot.isActive ? Theme.accent : Theme.controlInk
                        strokeWidth: tabRoot.iconStroke / tabRoot.iconScale
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        Behavior on strokeColor { ColorAnimation { duration: 100 } }

                        PathSvg { path: tabRoot.iconPath }
                    }
                    ShapePath {
                        fillColor: "#00000000"
                        strokeColor: tabRoot.isActive ? Theme.accent : Theme.controlInk
                        strokeWidth: tabRoot.linePath === "" ? 0 : 2 / tabRoot.iconScale
                        capStyle: ShapePath.RoundCap
                        Behavior on strokeColor { ColorAnimation { duration: 100 } }

                        PathSvg { path: tabRoot.linePath }
                    }
                }
            }

            // Only the icon pill takes hover and clicks.
            PositionHoverArea {
                id: tabHover
                anchors.fill: parent
                onClicked: {
                    if (!tabRoot.available)
                        return
                    root.tabSelected(tabRoot.tabKey)
                }
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 32
            color: !tabRoot.available ? Theme.textMuted : Theme.controlInk
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
            text: tabRoot.label
            textFormat: Text.PlainText
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
        }
    }

    TabButton {
        tabKey: "show"
        label: "Preview"
        width: 84
        glyph: "home"
    }
    // Edit: a pencil with an underline (24 px grid).
    TabButton {
        x: 92
        tabKey: "edit"
        label: "Edit"
        width: 84
        iconScale: 0.9
        filledIcon: true
        iconStroke: 0.5
        iconShapeWidth: 24; iconShapeHeight: 24
        iconPath: "M 4.55 14.16 L 4.55 17.35 L 7.74 17.35 L 17.14 7.95 L 13.95 4.76 Z M 19.6 5.48 C 19.93 5.15 19.93 4.61 19.6 4.28 L 17.61 2.29 C 17.28 1.96 16.74 1.96 16.41 2.29 L 14.85 3.85 L 18.04 7.04 Z"
        linePath: "M 4 21 L 20 21"
    }
    TabButton {
        x: 184
        tabKey: "stage"
        label: "Stage"
        width: 84
        iconShapeX: 1.75; iconShapeY: 1.75; iconShapeWidth: 10.50; iconShapeHeight: 10.50
        iconPath: "M 0.5833333333333334 0 L 3.5 0 C 3.822166085243225 0 4.083333333333334 0.2611672133207321 4.083333333333334 0.5833333333333334 L 4.083333333333334 4.666666666666667 C 4.083333333333334 4.988833030064901 3.822166085243225 5.25 3.5 5.25 L 0.5833333333333334 5.25 C 0.2611672133207321 5.25 0 4.988833030064901 0 4.666666666666667 L 0 0.5833333333333334 C 0 0.2611672133207321 0.2611672133207321 0 0.5833333333333334 0 Z M 7 0 L 9.916666666666668 0 C 10.238832751909893 0 10.5 0.2611672133207321 10.5 0.5833333333333334 L 10.5 2.3333333333333335 C 10.5 2.6554994185765586 10.238832751909893 2.916666666666667 9.916666666666668 2.916666666666667 L 7 2.916666666666667 C 6.677833879987399 2.916666666666667 6.416666666666667 2.6554994185765586 6.416666666666667 2.3333333333333335 L 6.416666666666667 0.5833333333333334 C 6.416666666666667 0.2611672133207321 6.677833879987399 0 7 0 Z M 7 5.25 L 9.916666666666668 5.25 C 10.238832751909893 5.25 10.5 5.511167213320733 10.5 5.833333333333334 L 10.5 9.916666666666668 C 10.5 10.238833030064901 10.238832751909893 10.5 9.916666666666668 10.5 L 7 10.5 C 6.677833879987399 10.5 6.416666666666667 10.238833030064901 6.416666666666667 9.916666666666668 L 6.416666666666667 5.833333333333334 C 6.416666666666667 5.511167213320733 6.677833879987399 5.25 7 5.25 Z M 0.5833333333333334 7.583333333333334 L 3.5 7.583333333333334 C 3.822166085243225 7.583333333333334 4.083333333333334 7.844500546654067 4.083333333333334 8.166666666666668 L 4.083333333333334 9.916666666666668 C 4.083333333333334 10.238832751909893 3.822166085243225 10.5 3.5 10.5 L 0.5833333333333334 10.5 C 0.2611672133207321 10.5 0 10.238832751909893 0 9.916666666666668 L 0 8.166666666666668 C 0 7.844500546654067 0.2611672133207321 7.583333333333334 0.5833333333333334 7.583333333333334 Z"
    }
}