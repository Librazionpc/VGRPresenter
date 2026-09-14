import QtQuick

// A menu dropdown panel: optional header (title + subtitle), then a list of
// items built from a plain data model. Each entry in `model` is one of:
//   { divider: true }
//   { label, trailing, danger }   // trailing = shortcut text, a submenu
//                                    chevron ("›"), or any right-aligned hint
//
// `danger` items get red text always (a destructive action, e.g. "Emergency
// Stop") — every other item is neutral text that only tints on hover, so no
// row looks permanently "selected" the way the raw export baked in.
//
// Every color/spacing/font value below is a literal, not a Theme.* reference:
// this file is instantiated from AppMenuBar.qml, itself nested two documents
// deep (Main -> VGRPresenterMainScreen -> AppMenuBar -> DropdownPanel). At
// that depth Qt 6.11.1's AOT compiler cannot resolve the Theme singleton at
// all — every property bound to it here logged "Unable to assign [undefined]"
// and rendered wrong permanently (confirmed, not just a transient warning).
// Each literal below is commented with the Theme token it must stay in sync
// with if that token's value ever changes.
Rectangle {
    id: root

    property var model: []
    property string headerTitle: ""
    property string headerSubtitle: ""

    signal itemActivated(string label)

    // Matches Theme.space4 — see x/y note below.
    readonly property int insetPad: 16

    width: 240
    height: content.height + 16
    radius: 10 // Theme.radiusLg
    color: "#16171e" // Theme.rowBg
    border.color: "#232530" // Theme.border
    border.width: 1
    clip: true

    Column {
        id: content
        y: 8
        width: parent.width

        Column {
            visible: root.headerTitle !== ""
            width: parent.width
            spacing: 2

            Text {
                x: root.insetPad
                topPadding: 4 // Theme.space1
                text: root.headerTitle
                color: "#6C5CE7" // Theme.accent
                font.family: "Inter" // Theme.fontFamily
                font.pixelSize: 13 // Theme.textMd
                font.weight: Font.Bold
            }
            Text {
                x: root.insetPad
                bottomPadding: 8 // Theme.space2
                text: root.headerSubtitle
                color: "#5c6475" // Theme.textMuted
                font.family: "Inter" // Theme.fontFamily
                font.pixelSize: 9 // Theme.textXs
            }
            Rectangle { width: parent.width; height: 1; color: "#232530" /* Theme.border */ }
        }

        Repeater {
            model: root.model
            delegate: Loader {
                required property var modelData
                width: content.width
                sourceComponent: modelData.divider === true ? dividerC : itemC

                Component {
                    id: dividerC
                    Item {
                        width: content.width
                        height: 12 // Theme.space3
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width - 32 // Theme.space4 * 2
                            height: 1
                            color: "#232530" // Theme.border
                        }
                    }
                }

                Component {
                    id: itemC
                    Rectangle {
                        width: content.width
                        height: 34
                        color: itemArea.containsMouse
                               ? (modelData.danger ? "#24ff4d3d" /* Theme.danger @ 14% */
                                                    : "#232530" /* Theme.border */)
                               : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            x: root.insetPad
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: modelData.danger ? "#ff6b61" /* Theme.dangerLight */ : "#e2e8f0" /* Theme.textPrimary */
                            font.family: "Inter" // Theme.fontFamily
                            font.pixelSize: 13 // Theme.textMd
                            font.weight: Font.Medium
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 16 // Theme.space4
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.trailing || ""
                            color: "#5c6475" // Theme.textMuted
                            font.family: "Inter" // Theme.fontFamily
                            font.pixelSize: 9 // Theme.textXs
                        }

                        MouseArea {
                            id: itemArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.itemActivated(modelData.label)
                        }
                    }
                }
            }
        }
    }
}
