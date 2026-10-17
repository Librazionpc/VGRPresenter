import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// "Add Bus" — the Edit screen's + ring asks which bus this slide should carry.
// Same scrim + centered card + `open`/`applied`/`cancelled` convention as
// CameraSourceModal.qml and the other pickers, with one difference that matters:
// its rows are the REAL buses (BusListModel — Settings · Audio & Video's
// Bus & Routing column), not a static list, so a bus created there is offered
// here without a restart and a removed one disappears.
//
// What the pick carries is the bus's IDENTITY (its stable engine number), not
// its row: a bus row can be reordered or renamed between opening this picker
// and the insert landing, and the item must still point at the same bus
// afterwards (the same reasoning BusListModel's route cache uses).
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // ---- Esc = cancel, Enter = Add (qml/settings/Hotkeys.qml) -------------
    property int hotkeyScope: 0
    function accept() {
        if (root.selectedBus !== null)
            root.applied(root.selectedBus)
    }
    onOpenChanged: {
        if (root.open)
            root.selectedId = 0
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Bus"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    // The bus roster as plain rows, re-read whenever the model changes — the
    // same revision trick MediaLibraryPane uses, because BusListModel's
    // accessors are Q_INVOKABLE (QML does not track them as dependencies).
    property int rev: 0
    Connections {
        target: BusListModel
        function onDataChanged() { root.rev++ }
        function onRowsInserted() { root.rev++ }
        function onRowsRemoved() { root.rev++ }
        function onModelReset() { root.rev++ }
    }
    readonly property var buses: {
        void root.rev
        const out = []
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) {
            const b = BusListModel.getBus(i)
            out.push({ row: i, id: b.id, name: b.name, type: b.type, level: b.level })
        }
        return out
    }

    // Selected by the bus's STABLE id (see the header): 0 = nothing picked.
    property int selectedId: 0
    readonly property var selectedBus: {
        for (let i = 0; i < root.buses.length; ++i)
            if (root.buses[i].id === root.selectedId)
                return root.buses[i]
        return null
    }

    // Fired by "Add", carrying the bus row { row, id, name, type, level }.
    signal applied(var bus)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    // The dialog belongs to the WINDOW, not to the pane that declares it (see
    // CameraSourceModal's note): a modal declared inside a settings page must
    // dim the whole app, and the card applies the app scale itself because
    // reparenting to the overlay leaves it with no scaled ancestor.
    parent: Overlay.overlay

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        anchors.centerIn: parent
        width: 520
        height: content.height + 40
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 16

            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Add Bus")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        radius: 7
                        color: closeArea.containsMouse ? Theme.hoverBg : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: Theme.textSecondary
                            font.pixelSize: 14
                        }

                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Pick the bus this item carries. The list is Settings · Audio & Video's buses.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }

            // ---- the buses -------------------------------------------------
            Rectangle {
                width: parent.width
                height: Math.min(300, Math.max(96, busList.contentHeight + 8))
                radius: 10
                color: Theme.inset
                border.color: Theme.border
                border.width: 1
                clip: true

                ListView {
                    id: busList
                    anchors.fill: parent
                    anchors.margins: 4
                    model: root.buses
                    spacing: 2
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: AppScrollBar { }

                    delegate: Rectangle {
                        id: busRow
                        required property var modelData
                        width: busList.width
                        height: 48
                        radius: 8
                        readonly property bool picked: root.selectedId === modelData.id
                        color: picked ? Qt.alpha(Theme.accent, 0.18)
                            : rowArea.containsMouse ? Theme.hoverBg : "transparent"
                        border.width: picked ? 1 : 0
                        border.color: Qt.alpha(Theme.accent, 0.45)

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                text: busRow.modelData.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: qsTr("%1 bus · level %2")
                                    .arg(busRow.modelData.type)
                                    .arg(Math.round(busRow.modelData.level))
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedId = busRow.modelData.id
                            onDoubleClicked: {
                                root.selectedId = busRow.modelData.id
                                root.accept()
                            }
                        }
                    }
                }

                // An install with no buses yet says so instead of showing an
                // empty box — the row that fixes it is one screen away.
                Text {
                    anchors.centerIn: parent
                    visible: root.buses.length === 0
                    width: parent.width - 40
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("No buses yet — add one in Settings · Audio & Video.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }

            // ---- footer ----------------------------------------------------
            Item {
                width: parent.width
                height: 36

                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 88
                    height: 36
                    radius: 10
                    enabled: root.selectedBus !== null
                    opacity: root.selectedBus !== null ? 1 : 0.4
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add")
                        color: "#ffffff"
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.accept()
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 88 + 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 88
                    height: 36
                    radius: 10
                    color: cancelArea.containsMouse ? Theme.hoverBg : "transparent"
                    border.color: Theme.border
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelled()
                    }
                }
            }
        }
    }
}
