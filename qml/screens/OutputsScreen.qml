import QtQuick
import VGRPresenterUI
import "../components"

// Settings · Outputs — CRUD hub for the outputs a show is sent to: the
// output roster (Add / Edit / Duplicate / Delete), each output's live state
// and on/off toggle. Styles live in their own section (StylesScreen.qml);
// both read the same C++ singletons (OutputListModel / StyleListModel) —
// the SAME roster the Edit screen's output monitor renders — so a rename,
// restyle, duplicate or delete here is reflected everywhere at once.
Item {
    id: root

    // Index currently open in the edit dialog; -1 = closed.
    property int editIndex: -1
    // Index pending deletion (confirm dialog); -1 = none.
    property int deleteIndex: -1
    // Index the right-click context menu was opened for; -1 = closed.
    property int contextMenuIndex: -1

    // Add-dialog form state (VGRPresenter · Settings · Outputs · Add).
    property bool addShown: false
    property string addName: ""
    property string addType: "HDMI"
    property string addRes: "1920 × 1080"
    property string addRefresh: "60 Hz"
    // Which existing screen the new one is placed against (-1 = none).
    // Physical display the new screen is placed on (QScreen::name,
    // "" = windowed), and whether its assignment starts locked.
    property string addScreenName: ""
    property bool addLocked: false
    // "smpte" | "gradient" | "checker" | "solidred"
    property string addPattern: "smpte"

    function submitAdd() {
        OutputListModel.addScreen(root.addName, root.addType, root.addRes, root.addRefresh, root.addPattern)
        // Placement/lock are post-insert setters — the row must exist first.
        if (root.addScreenName !== "" || root.addLocked) {
            const row = OutputListModel.rowCount() - 1
            OutputListModel.setScreenName(row, root.addScreenName)
            OutputListModel.setBoundsLocked(row, root.addLocked)
        }
        // Reset for next time, close.
        root.addName = ""
        root.addType = "HDMI"
        root.addScreenName = ""
        root.addLocked = false
        root.addPattern = "smpte"
        root.addShown = false
    }

    // Editable copies loaded when the dialog opens — the model is only
    // written on Save, matching the dialog's Cancel/Save contract.
    property string editName: ""
    property string editType: "HDMI"
    property string editRes: ""
    property string editRefresh: "60 Hz"
    property string editPattern: "smpte"
    property string editScreenName: ""
    property bool editLocked: false
    property int editStyleIndex: 0

    function openEdit(index) {
        const data = OutputListModel.getOutput(index)
        root.editIndex = index
        root.editName = data.name
        root.editType = data.kind
        root.editRes = data.res
        root.editRefresh = data.refresh !== "" ? data.refresh : "60 Hz"
        root.editPattern = data.testPattern !== "none" ? data.testPattern : "smpte"
        root.editScreenName = data.screenName
        root.editLocked = data.boundsLocked
        root.editStyleIndex = data.styleIndex
    }

    function saveEdit() {
        if (root.editIndex < 0)
            return
        OutputListModel.renameOutput(root.editIndex, root.editName)
        OutputListModel.setKind(root.editIndex, root.editType)
        OutputListModel.setResolution(root.editIndex, root.editRes)
        OutputListModel.setRefresh(root.editIndex, root.editRefresh)
        OutputListModel.setTestPattern(root.editIndex, root.editPattern)
        OutputListModel.setScreenName(root.editIndex, root.editScreenName)
        OutputListModel.setBoundsLocked(root.editIndex, root.editLocked)
        OutputListModel.setStyle(root.editIndex, root.editStyleIndex)
        root.editIndex = -1
    }

    // Bumped on any change to the outputs model so derived values recompute.
    // Plain Q_INVOKABLE reads in bindings aren't tracked, so the revision
    // counter is the honest way.
    property int modelsRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + Theme.space6 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space6
            width: flick.width - Theme.space6
            spacing: Theme.space6

            Column {
                spacing: Theme.space1
                Text {
                    text: "Outputs"
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }
                Text {
                    // Wraps clear of the pinned "+ Add Output" button.
                    width: layout.width - 200
                    text: "Settings · Outputs — manage the screens and streams the show is sent to, and the styles they render with."
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.WordWrap
                }
            }

            // ---- Screens list ----
            Repeater {
                model: OutputListModel

                delegate: Rectangle {
                    id: card
                    required property int index
                    required property string name
                    required property string badge
                    required property string kind
                    required property string res
                    required property bool active
                    required property bool isEnabled
                    required property string styleName

                    width: layout.width
                    height: cardCol.height + Theme.space5 * 2
                    radius: Theme.radiusLg
                    color: Theme.card
                    border.width: card.active ? 1 : 0
                    border.color: Theme.danger
                    // A disabled screen reads dimmed at a glance.
                    opacity: card.isEnabled ? 1 : 0.45
                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    Column {
                        id: cardCol
                        x: 20
                        y: 20
                        width: parent.width - Theme.space5 * 2
                        spacing: Theme.space3

                        Row {
                            spacing: Theme.space3

                            Pill {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.badge
                                tint: false
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textMd
                                font.weight: Font.Medium
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "·  " + card.kind
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }

                        Row {
                            spacing: Theme.space2

                            Text {
                                text: card.res
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                            Text {
                                text: "·  Style:"
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                            Pill {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.styleName
                                tint: false
                            }
                        }
                    }

                    // Status pill + on/off toggle, anchored top-right.
                    // Live = Theme.danger red (#ff4d3d) wash + light-red
                    // text; Inactive = flat neutral chip. Pill's `tint` flag
                    // picks which colors apply: tint=true uses
                    // baseColor/lightColor (washed bg + colored text),
                    // tint=false renders the flat neutral chip and IGNORES
                    // baseColor — hence the flip between the two states.
                    // Disabled screens are dimmed and can't go live (model
                    // guards it; the click is inert here too).
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space5
                        anchors.top: parent.top
                        anchors.topMargin: Theme.space5
                        spacing: Theme.space3

                        // Quick edit affordance — same dialog the right-click
                        // menu's Edit opens.
                        AppButton {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Edit"
                            variant: "ghost"
                            onClicked: root.openEdit(card.index)
                        }

                        Pill {
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.active ? "LIVE" : "Inactive"
                            baseColor: Theme.danger
                            lightColor: Theme.dangerLight
                            tint: card.active
                            tintAlpha: card.active ? 0.22 : 0.14

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: card.isEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: OutputListModel.setActive(card.index)
                            }
                        }

                        SettingsToggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: card.isEnabled
                            onToggled: OutputListModel.setEnabled(card.index, !card.isEnabled)
                        }
                    }

                    // Right-click menu — same DropdownPanel pattern as the
                    // slide rows in EditScreen (Edit / Duplicate / Delete).
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        cursorShape: Qt.ArrowCursor
                        onClicked: (mouse) => {
                            screenContextMenu.openAt(card, mouse.x, mouse.y, root)
                            root.contextMenuIndex = card.index
                        }
                    }
                }
            }

        }
    }

    // "+ Add Output" pinned top-right — always visible no matter how far
    // the roster scrolls (sits above the Flickable in paint order). A
    // custom Rectangle rather than plain AppButton: a drawn plus glyph
    // (matching this app's hand-drawn icon language — see ScreenForm's
    // Identify/Lock rows) plus a touch more padding/presence than
    // AppButton's default 34px height gives it, since this is the
    // section's one primary call-to-action, not a row-level action.
    Rectangle {
        id: addOutputBtn
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space6
        anchors.rightMargin: Theme.space6 + Theme.space2
        width: addOutputRow.width + Theme.space6 * 2
        height: 40
        radius: Theme.radiusMd
        color: addOutputArea.pressed ? Qt.darker(Theme.accent, 1.15)
             : addOutputArea.containsMouse ? Qt.lighter(Theme.accent, 1.1)
             : Theme.accent
        Behavior on color { ColorAnimation { duration: 120 } }

        Row {
            id: addOutputRow
            anchors.centerIn: parent
            spacing: 8

            Item {
                width: 11; height: 11
                anchors.verticalCenter: parent.verticalCenter
                Rectangle { anchors.centerIn: parent; width: 11; height: 1.6; radius: 0.8; color: "#ffffff" }
                Rectangle { anchors.centerIn: parent; width: 1.6; height: 11; radius: 0.8; color: "#ffffff" }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Add Output")
                color: "#ffffff"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }

        MouseArea {
            id: addOutputArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.addShown = true
        }
    }

    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }

    // ---- Card context menu (Edit / Duplicate / Delete) ----
    DropdownPanel {
        id: screenContextMenu
        visible: false
        model: [
            { label: "Edit" },
            { label: "Duplicate" },
            { divider: true },
            { label: "Delete", danger: true }
        ]
        onItemActivated: (label) => {
            switch (label) {
            case "Edit":
                root.openEdit(root.contextMenuIndex)
                break
            case "Duplicate":
                OutputListModel.duplicateOutput(root.contextMenuIndex)
                break
            case "Delete":
                root.deleteIndex = root.contextMenuIndex
                break
            }
            screenContextMenu.visible = false
        }
    }

    // ---- Delete confirm ----
    ConfirmDialog {
        id: deleteDialog
        shown: root.deleteIndex >= 0
        title: "Delete output?"
        message: {
            if (root.deleteIndex < 0)
                return ""
            const data = OutputListModel.getOutput(root.deleteIndex)
            return "\u201C" + data.name + "\u201D will be removed from the output roster. This affects every view that renders it."
        }
        confirmLabel: "Delete"
        onConfirmed: {
            OutputListModel.removeOutput(root.deleteIndex)
            root.deleteIndex = -1
        }
        onDismissed: root.deleteIndex = -1
    }

    // ---- Edit dialog ----
    ModalCard {
        id: editDialog
        shown: root.editIndex >= 0
        title: "Edit output"
        subtitle: "Rename, restyle, or reposition this output."
        cardWidth: 760
        saveText: "Save"
        onCancelled: root.editIndex = -1
        onAccepted: root.saveEdit()

        // Same form the Add dialog shows — one design, one implementation
        // (ScreenForm), seeded with this screen's current values. Placement
        // writes the real display assignment; setScreenName snaps res and
        // refresh to the display's mode on save.
        ScreenForm {
            width: parent.width
            name: root.editName
            type: root.editType
            res: root.editRes
            refresh: root.editRefresh
            pattern: root.editPattern
            placement: root.editScreenName
            locked: root.editLocked
            onNameEdited: (t) => root.editName = t
            onTypePicked: (k) => root.editType = k
            onResPicked: (v) => root.editRes = v
            onRefreshPicked: (v) => root.editRefresh = v
            onPatternPicked: (k) => root.editPattern = k
            onPlacementPicked: (s, r, f) => {
                root.editScreenName = s
                if (r !== "") {
                    root.editRes = r
                    root.editRefresh = f
                }
            }
            onLockToggled: (l) => root.editLocked = l
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: "Style"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            // Swatch+"Change" row — same language as EditScreen.qml's
            // Background row and TextItemPanel's font-color row. Opens
            // StylePickerModal (below) instead of the old inline chip Flow,
            // which didn't preview anything and got unwieldy once the
            // roster grew past a handful of styles.
            Rectangle {
                width: parent.width
                height: 46
                radius: Theme.radiusMd
                color: Theme.inset

                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        const rows = StyleListModel.rowCount()
                        if (rows === 0)
                            return qsTr("No styles yet")
                        if (root.editStyleIndex < 0 || root.editStyleIndex >= rows)
                            return qsTr("None")
                        return StyleListModel.data(StyleListModel.index(root.editStyleIndex, 0), StyleListModel.NameRole)
                    }
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 26
                    width: changeStyleLabel.implicitWidth + 22
                    radius: 13
                    color: changeStyleArea.containsMouse ? "#20242f" : "#1a1c26"
                    border.color: "#2a3140"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        id: changeStyleLabel
                        anchors.centerIn: parent
                        text: qsTr("Change")
                        color: "#aeb6c8"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: changeStyleArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            stylePicker.selectedIndex = root.editStyleIndex
                            stylePicker.open = true
                        }
                    }
                }
            }
        }

    }

    StylePickerModal {
        id: stylePicker
        onApplied: (index) => {
            root.editStyleIndex = index
            stylePicker.open = false
        }
        onCancelled: stylePicker.open = false
    }

    // ---- Add dialog (VGRPresenter · Settings · Outputs · Add) ----
    ModalCard {
        id: addDialog
        shown: root.addShown
        title: "Add Output"
        subtitle: "Create an output window, position it on a display, or send it to NDI, SDI, recording or streaming"
        cardWidth: 760
        saveText: "Add Output"
        onCancelled: root.addShown = false
        onAccepted: root.submitAdd()

        ScreenForm {
            width: parent.width
            name: root.addName
            type: root.addType
            res: root.addRes
            refresh: root.addRefresh
            pattern: root.addPattern
            placement: root.addScreenName
            locked: root.addLocked
            onNameEdited: (t) => root.addName = t
            onTypePicked: (k) => root.addType = k
            onResPicked: (v) => root.addRes = v
            onRefreshPicked: (v) => root.addRefresh = v
            onPatternPicked: (k) => root.addPattern = k
            onPlacementPicked: (s, r, f) => {
                root.addScreenName = s
                if (r !== "") {
                    root.addRes = r
                    root.addRefresh = f
                }
            }
            onLockToggled: (l) => root.addLocked = l
        }
    }

}
