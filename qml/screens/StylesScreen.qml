import QtQuick
import VGRPresenterUI
import "../components"

// Settings · Styles — the presentation "themes" an output renders with,
// promoted from a card inside Outputs to its own section. Roster lives in
// the StyleListModel C++ singleton (starts empty — nothing hardcoded, same
// rule as slides). Rows are inline-editable; each shows its live
// "applied by N outputs" count from the shared OutputListModel.
Item {
    id: root

    // Bumped on any change to either model so the usage counts recompute.
    // Plain Q_INVOKABLE reads in bindings aren't tracked, so the revision
    // counter is the honest dependency.
    property int modelsRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    function styleUsage(styleIdx) {
        let count = 0
        const rows = OutputListModel.rowCount()
        for (let i = 0; i < rows; ++i) {
            if (OutputListModel.getOutput(i).styleIndex === styleIdx)
                count++
        }
        return count
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space5
            width: flick.width - Theme.space6
            spacing: 16

            // ---- Page header ----
            Column {
                spacing: 2

                Text {
                    text: qsTr("Styles")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Presentation themes an output renders with — assign one per screen in its Edit dialog.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Styles card ----
            Rectangle {
                width: parent.width
                height: stylesCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: stylesCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    // A plain Item, not a Row — Row/Column/Grid forbid their
                    // own children from using anchors.left/right/fill/
                    // centerIn (the positioner sets each child's geometry
                    // itself), and both children here need anchors to sit
                    // on opposite edges of the row.
                    Item {
                        width: parent.width
                        height: Math.max(styleHeaderText.implicitHeight, addStyleButton.height)

                        Text {
                            id: styleHeaderText
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("All styles")
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }

                        AppButton {
                            id: addStyleButton
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add style")
                            variant: "ghost"
                            onClicked: StyleListModel.addStyle()
                        }
                    }

                    // Empty state — the roster starts empty by design.
                    Text {
                        visible: StyleListModel.rowCount() === 0
                        width: parent.width
                        text: qsTr("No styles yet. Add one, then assign it to a screen via its right-click Edit dialog.")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        topPadding: 4
                    }

                    Repeater {
                        model: StyleListModel

                        delegate: Rectangle {
                            id: styleRow
                            required property int index
                            required property string name
                            required property string res

                            width: stylesCol.width
                            height: Math.max(40, rowCol.implicitHeight + 16)
                            radius: Theme.radiusMd
                            color: Theme.inset

                            Column {
                                id: rowCol
                                anchors.verticalCenter: parent.verticalCenter
                                x: 12
                                spacing: 2

                                // Inline-editable name — looks like a label,
                                // commits on Enter / focus loss.
                                TextInput {
                                    width: Math.max(implicitWidth + 2, 120)
                                    text: styleRow.name
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.Medium
                                    selectByMouse: true
                                    onEditingFinished: StyleListModel.renameStyle(styleRow.index, text)
                                }

                                Row {
                                    spacing: 8

                                    TextInput {
                                        width: Math.max(implicitWidth + 2, 80)
                                        text: styleRow.res
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        selectByMouse: true
                                        onEditingFinished: StyleListModel.setResolution(styleRow.index, text)
                                    }

                                    Text {
                                        // modelsRev referenced inside the
                                        // binding keeps this count honest.
                                        text: {
                                            root.modelsRev
                                            const u = root.styleUsage(styleRow.index)
                                            return "·  applied by " + u + (u === 1 ? " output" : " outputs")
                                        }
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Shared app scrollbar at the fixed right edge (sibling of the
    // Flickable — see GeneralScreen's note).
    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }
}
