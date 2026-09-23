import QtQuick
import VGRPresenterUI
import "."

// The Routing… modal — the input's channel × bus matrix (reference:
// the ProPresenter-style channel patcher: an Auto toggle, column labels
// for destinations, row labels for channels, and bright cells you click
// to patch). One modal serves Add and Edit: it never touches the models
// itself — the consumer loads state into it before `open()` and receives
// the whole patch back through `applied(auto, routes)`.
//
// routes is a JS object { channelIndex: [busIndex, ...] } — missing keys
// read as unrouted. Buses come from BusListModel rows; `busRev` must be
// bound to the consumer's modelsRev-style counter (Q_INVOKABLE rowCount()
// reads are untracked by QML's binding system — the same honesty rule as
// every revision bridge in the settings screens).
ModalCard {
    id: root

    property string inputName: ""
    property int channelCount: 1
    property bool autoRoute: false
    property var routes: ({})
    // Consumer-bound revision of BusListModel (modelsRev) — the buses
    // list re-reads through it.
    property int busRev: 0

    readonly property var busList: {
        root.busRev
        const arr = []
        for (let i = 0; i < BusListModel.rowCount(); i++) {
            const b = BusListModel.getBus(i)
            arr.push({ name: b.name !== undefined ? b.name : "" })
        }
        return arr
    }

    title: qsTr("%1 — Routing").arg(root.inputName)
    cardWidth: 460
    saveText: qsTr("Apply")
    cancelText: qsTr("Discard")

    // { channelIndex: [busIndex, ...] } — the consumer writes this back
    // wherever its state lives (model row or Add-dialog buffer).
    signal applied(bool autoRoute, var routes)

    onAccepted: root.applied(root.autoRoute, root.routes)

    function toggle(channel, bus) {
        if (root.autoRoute) return
        const next = {}
        for (const k in root.routes) next[k] = root.routes[k].slice()
        const list = (next[channel] || []).slice()
        const at = list.indexOf(bus)
        if (at >= 0) list.splice(at, 1)
        else list.push(bus)
        next[channel] = list
        root.routes = next
    }

    Column {
        width: parent.width
        spacing: Theme.space4

        // ---- Auto + caption row ------------------------------------
        Item {
            width: parent.width
            height: 34

            Rectangle {
                id: autoBtn
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 74; height: 30; radius: Theme.radiusMd
                color: root.autoRoute ? "#3574f0" : Theme.inset
                border.width: 1
                border.color: root.autoRoute ? "#3574f0" : Theme.border
                Behavior on color { ColorAnimation { duration: 110 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Auto")
                    color: root.autoRoute ? "#ffffff" : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.autoRoute = !root.autoRoute
                }
            }

            Text {
                anchors.left: autoBtn.right
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - autoBtn.width - 12
                text: root.autoRoute
                      ? qsTr("Every channel feeds every bus")
                      : qsTr("Click a cell to patch that channel to the bus")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                elide: Text.ElideRight
            }
        }

        // ---- The matrix ---------------------------------------------
        // Column labels (buses) over a cell grid; row labels (channels)
        // at the left. Auto lights every cell and ignores clicks.
        // Horizontal scroll: 46 px gutter + 44 px per bus outgrows the card at ~9
        // buses, and the bus list is unbounded.
        Flickable {
            id: matrixFlick
            width: parent.width
            height: matrixCol.height
            contentWidth: matrixCol.width
            contentHeight: matrixCol.height
            clip: true
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width

            // Vertical wheel scrolls sideways while the matrix overflows.
            WheelHandler {
                enabled: matrixFlick.contentWidth > matrixFlick.width
                onWheel: (event) => {
                    matrixFlick.contentX = Math.max(0, Math.min(
                        matrixFlick.contentWidth - matrixFlick.width,
                        matrixFlick.contentX - event.angleDelta.y))
                }
            }

        Column {
            id: matrixCol
            width: implicitWidth
            spacing: 6

            Row {
                spacing: 6

                Item { width: 46; height: 24 } // row-label gutter

                Row {
                    spacing: 4

                    Repeater {
                        model: root.busList.length

                        delegate: Item {
                            id: colHead
                            required property int index
                            width: 40
                            height: 24

                            Text {
                                anchors.centerIn: parent
                                width: parent.width
                                // The bus list can shrink while the modal is open —
                                // the Repeater may still hand us an index past its end.
                                text: (root.busList[colHead.index] || {}).name || ""
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            Column {
                spacing: 4

                Repeater {
                    model: root.channelCount

                    delegate: Row {
                        id: chRow
                        required property int index

                        spacing: 6

                        Text {
                            width: 46
                            height: 34
                            text: String(chRow.index + 1)
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        Row {
                            spacing: 4

                            Repeater {
                                model: root.busList.length

                                delegate: Rectangle {
                                    id: cell
                                    required property int index
                                    readonly property int busIndex: cell.index
                                    readonly property bool patched: root.autoRoute
                                        || ((root.routes[chRow.index] || []).indexOf(cell.busIndex) >= 0)
                                    width: 40
                                    height: 34
                                    radius: 2
                                    color: cell.patched ? "#4da3ff" : Theme.inset
                                    border.width: 1
                                    border.color: root.autoRoute ? "#3a3d48" : Theme.border
                                    opacity: root.autoRoute ? 0.65 : 1
                                    Behavior on color { ColorAnimation { duration: 100 } }

                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -2   // half the 4 px gap — adjacent hit areas touch, never overlap
                                        cursorShape: root.autoRoute ? Qt.ArrowCursor : Qt.PointingHandCursor
                                        enabled: !root.autoRoute
                                        onClicked: root.toggle(chRow.index, cell.busIndex)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        }

        Text {
            visible: root.busList.length === 0
            width: parent.width
            text: qsTr("No buses yet — add one in Buses & Routing, then patch channels to it here.")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
            wrapMode: Text.WordWrap
        }
    }
}
