import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// Action roster and editor share one ModalCard. The editor and catalog replace
// the content in place, so choosing an action never stacks another popup.
//
// The editor has TWO shapes, matching the design:
//   ADD  (a blank action, straight from the roster's + Add) — labels sit
//        ABOVE their fields, a centered "— shortcut —" placeholder, and no
//        icon box: a brand-new action takes its icon from whatever catalog
//        entry it runs;
//   EDIT (an action that already exists) — the same fields with FLOATING
//        labels inside their boxes, an Icon box beside the name, and the
//        keypress field carrying its clear (×) and capture (↵) marks.
// `editLayout` is fixed when the editor opens, so the shape never changes
// under the pointer while somebody types.
ModalCard {
    id: root
    objectName: "manageActionsModal"
    title: view === "list" ? qsTr("Manage actions")
          : view === "catalog" ? qsTr("Choose action") : qsTr("Action")
    subtitle: view === "list" ? qsTr("Create shortcuts that run one or more actions.") : ""
    cardWidth: 660
    showFooter: false
    // The top-right door this view has: the icon picker while editing an
    // action, the reveal eye on the trigger list AND the action catalog
    // (closed = common only, same shape for both — see showAllActions/
    // showAllTriggers). Living in the header beside the close X instead of
    // floating inline next to the search box frees that box to use the
    // dialog's full width.
    headerActionIcon: root.view === "triggers"
                      ? (root.showAllTriggers ? "eye" : "eyeOff")
                      : root.view === "catalog"
                      ? (root.showAllActions ? "eye" : "eyeOff")
                      : (root.view === "editor" && root.editLayout ? "sliders" : "")
    headerActionActive: root.view === "triggers" ? root.showAllTriggers
                       : root.view === "catalog" ? root.showAllActions
                       : root.iconPickerOpen
    onHeaderActionClicked: {
        if (root.view === "triggers") root.showAllTriggers = !root.showAllTriggers
        else if (root.view === "catalog") root.showAllActions = !root.showAllActions
        else root.iconPickerOpen = !root.iconPickerOpen
    }
    // The trigger list is a sub-view of the editor, so its arrow goes back
    // there rather than closing the whole dialog.
    headerBackVisible: root.view === "triggers"
    onHeaderBackClicked: root.view = "editor"
    onCancelled: {
        root.iconPickerOpen = false
        if (view === "catalog" || view === "triggers") view = "editor"
        else if (view === "editor") { view = "list"; editingRow = -1 }
        else root.close()
    }

    property string view: "list"
    property int editingRow: -1
    property int editingStep: -1
    property int shortcutCaptureRow: -1
    property string editingShortcut: ""
    property string catalogSearch: ""
    property bool showAllActions: false
    // true while editing an action that already exists (see the header note).
    property bool editLayout: false
    property bool iconPickerOpen: false
    // false = the eye is CLOSED and only the common triggers are listed (the
    // default); true reveals the long tail. Same shape as the catalog's eye.
    property bool showAllTriggers: false
    // THE TRIGGERS (FreeShow's custom activations): the app events an action
    // can run on by itself. The table lives in ActionListModel (one source for
    // this picker AND the Functions pane's summary).
    readonly property var triggerChoices: { void ActionListModel.rosterChanged; return ActionListModel.triggerEntries() }
    // The icons an action may wear. Every name here is one of IconGlyph's
    // own glyphs, so the grid can never offer a blank swatch.
    property var iconChoices: ["flag", "star", "play", "pause", "stop", "trash",
                               "lock", "unlocked", "sliders", "bookOpen", "music",
                               "mic", "camera", "eye", "image", "folder", "layers",
                               "overlay", "calendar", "clock", "timerFill", "search",
                               "link", "copy", "download", "info", "scripture",
                               "bible", "shows", "media"]

    function actionAt(row) {
        void ActionListModel.rosterChanged
        return row >= 0 ? ActionListModel.getAction(row) : ({})
    }
    function currentIcon() {
        return root.actionAt(root.editingRow).icon || "flag"
    }
    function startEdit(row, existing) {
        editingRow = row
        editingShortcut = String(root.actionAt(row).shortcut || "")
        editLayout = existing !== false
        iconPickerOpen = false
        shortcutCaptureRow = -1
        view = "editor"
        Qt.callLater(function() {
            if (root.view === "editor" && !(root.actionAt(row).name || ""))
                nameInput.forceActiveFocus()
        })
    }
    function stepsFor(row) {
        const action = actionAt(row)
        return action.steps || []
    }
    function chooseStep(stepIndex) {
        editingStep = stepIndex
        catalogSearch = ""
        view = "catalog"
    }
    function selectCatalogEntry(entry) {
        if (!entry || !entry.supported)
            return
        const action = actionAt(editingRow)
        const steps = (action.steps || []).slice()
        const value = { effectId: entry.id, effectValue: "" }
        if (editingStep < steps.length) steps[editingStep] = value
        else steps.push(value)
        ActionListModel.setSteps(editingRow, steps)
        ActionListModel.setIcon(editingRow, entry.icon || "flag")
        view = "editor"
    }
    function filteredCatalog() {
        void ActionListModel.rosterChanged
        const entries = ActionListModel.catalogEntries()
        const query = catalogSearch.trim().toLowerCase()
        const out = []
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (!showAllActions && !entry.common) continue
            if (query && (String(entry.name).toLowerCase().indexOf(query) < 0
                          && String(entry.id).toLowerCase().indexOf(query) < 0
                          && String(entry.section).toLowerCase().indexOf(query) < 0)) continue
            out.push(entry)
        }
        return out
    }
    function isSectionStart(entries, index) {
        return index === 0 || entries[index - 1].section !== entries[index].section
    }
    function updateSteps(row, steps) { ActionListModel.setSteps(row, steps) }

    // ---- custom activation (triggers) ------------------------------------
    function triggerEntry(id) {
        for (let i = 0; i < root.triggerChoices.length; ++i)
            if (root.triggerChoices[i].id === id) return root.triggerChoices[i]
        return ({})
    }
    function currentTrigger() {
        return root.actionAt(root.editingRow).trigger || ""
    }
    function triggerName(id) {
        const entry = root.triggerEntry(id)
        return entry.name || ""
    }
    // The name of ANOTHER action already using this trigger, "" when free.
    // The picker shows it, so two actions silently waking on the same event is
    // something the operator sees while choosing, not afterwards.
    function usedElsewhere(triggerId) {
        void ActionListModel.rosterChanged
        for (let i = 0; i < ActionListModel.rowCount(); ++i) {
            if (i === root.editingRow) continue
            const a = ActionListModel.getAction(i)
            if (String(a.trigger || "") === String(triggerId))
                return a.name !== "" ? a.name : qsTr("Untitled")
        }
        return ""
    }
    function shownTriggers() {
        if (root.showAllTriggers)
            return root.triggerChoices
        const out = []
        for (let i = 0; i < root.triggerChoices.length; ++i) {
            const entry = root.triggerChoices[i]
            // A trigger this action already runs on stays listed even when the
            // eye is closed, or the current choice would vanish from its own
            // picker.
            if (entry.common !== false || entry.id === root.currentTrigger())
                out.push(entry)
        }
        return out
    }
    function chooseTrigger(id) {
        ActionListModel.setTrigger(root.editingRow, id)
        root.view = "editor"
    }

    // The "+ Add" strip the roster and the editor share — a solid accent
    // fill, the same primary-button purple as the app's other primary
    // actions (e.g. Save Changes), not a translucent wash.
    component AddActionRow: Rectangle {
        id: addRow
        signal picked()
        width: parent ? parent.width : 200
        // 34 — AppButton's own implicitHeight (the "Edit" buttons beside the
        // rows above use it), not a bespoke taller number that made this the
        // most visually dominant control in the dialog.
        height: 34
        radius: 7
        color: mouseArea.pressed ? Qt.darker(Theme.accent, 1.15) : Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            hovered: mouseArea.containsMouse
            glowColor: Theme.hoverBg
        }

        Text {
            anchors.centerIn: parent
            text: qsTr("+ Add")
            color: Theme.controlInk
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            font.weight: Font.Medium
        }

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: addRow.picked()
        }
    }

    Column {
        width: parent.width
        spacing: Theme.space2

        // ---- the roster --------------------------------------------------
        Column {
            visible: root.view === "list"
            width: parent.width
            spacing: Theme.space2

            // The roster lives in the SAME striped panel as the two pickers
            // (StripedListPanel) — one ground, one hairline, one pair of row
            // colours for all three lists.
            StripedListPanel {
                id: rosterPanel
                width: parent.width
                visible: ActionListModel.rowCount() > 0

                Repeater {
                    model: ActionListModel
                    delegate: Rectangle {
                        id: actionRow
                        required property int index
                        required property string name
                        required property string icon
                        required property string shortcut
                        required property string trigger
                        required property var steps
                        width: parent.width
                        height: 52
                        radius: rosterPanel.rowRadius
                        color: rosterPanel.rowColor(index, actionRowHover.hovered)

                        readonly property string summary: {
                            const list = steps || []
                            const base = !list.length
                                ? qsTr("Choose action")
                                : (ActionListModel.catalogEntry(list[0].effectId).name || qsTr("Action"))
                                  + (list.length > 1 ? qsTr("  + %1 more").arg(list.length - 1) : "")
                            // A custom activation is part of what the action IS —
                            // visible from the roster, not only inside the editor.
                            return trigger !== ""
                                   ? base + "  ·  " + root.triggerName(trigger)
                                   : base
                        }

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10

                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                name: actionRow.icon || "flag"
                                color: Theme.accentLight
                                width: 17; height: 17; fit: true
                            }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 235
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: actionRow.name || qsTr("Unnamed action")
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: actionRow.summary
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                    elide: Text.ElideRight
                                }
                            }
                            ShortcutCaptureField {
                                width: 88; height: 30
                                anchors.verticalCenter: parent.verticalCenter
                                shortcut: actionRow.shortcut
                                capturing: root.shortcutCaptureRow === actionRow.index
                                onCaptureRequested: root.shortcutCaptureRow = capturing ? -1 : actionRow.index
                                onCaptureCancelled: root.shortcutCaptureRow = -1
                                onShortcutEdited: (value) => {
                                    ActionListModel.setShortcut(actionRow.index, value)
                                    root.shortcutCaptureRow = -1
                                }
                            }
                            AppButton {
                                text: qsTr("Edit")
                                variant: "secondary"
                                anchors.verticalCenter: parent.verticalCenter
                                onClicked: root.startEdit(actionRow.index, true)
                            }
                            Rectangle {
                                width: 26; height: 28; radius: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: removeHover.hovered ? Qt.alpha(Theme.danger, 0.16) : "transparent"
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: "close"
                                    color: removeHover.hovered ? Theme.dangerLight : Theme.textMuted
                                    width: 12; height: 12
                                }
                                HoverHandler { id: removeHover }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: ActionListModel.removeAction(actionRow.index)
                                }
                            }
                        }
                        HoverHandler { id: actionRowHover }
                    }
                }
            }
            Text {
                visible: ActionListModel.rowCount() === 0
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                topPadding: Theme.space4
                text: qsTr("No actions yet.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            AddActionRow {
                onPicked: root.startEdit(ActionListModel.addAction(), false)
            }
        }

        // ---- the action editor -------------------------------------------
        Column {
            visible: root.view === "editor"
            width: parent.width
            spacing: Theme.space3

            // Name (+ Icon when editing an existing action)
            Row {
                width: parent.width
                spacing: Theme.space2

                Column {
                    width: parent.width - (iconBox.visible ? iconBox.width + Theme.space2 : 0)
                    spacing: 6

                    Text {
                        visible: !root.editLayout
                        text: qsTr("Name")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }

                    Rectangle {
                        id: nameField
                        width: parent.width
                        // 44 — the app's standard row/field height (matches
                        // ManageGroupsModal's rows and SettingsField's own
                        // family), for BOTH layouts. The EDIT layout's
                        // floating label still fits above the baseline at
                        // this height, just with tighter padding than the
                        // old 58px box.
                        height: 44
                        radius: 7
                        color: Theme.inset
                        border.width: 1
                        border.color: nameInput.activeFocus ? Theme.accent : Theme.border

                        Text {
                            visible: root.editLayout
                            x: 13; y: 5
                            text: qsTr("Name")
                            color: nameInput.activeFocus || nameInput.text !== ""
                                   ? Theme.accentLight : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }

                        TextInput {
                            id: nameInput
                            anchors.fill: parent
                            anchors.leftMargin: 13; anchors.rightMargin: 13
                            verticalAlignment: root.editLayout ? TextInput.AlignBottom
                                                               : TextInput.AlignVCenter
                            topPadding: root.editLayout ? 18 : 0
                            bottomPadding: root.editLayout ? 7 : 0
                            text: root.actionAt(root.editingRow).name || ""
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            selectByMouse: true
                            onTextEdited: ActionListModel.setName(root.editingRow, text)
                        }
                    }
                }

                Rectangle {
                    id: iconBox
                    visible: root.editLayout
                    width: 128
                    height: nameField.height
                    radius: 7
                    color: Theme.inset
                    border.width: 1
                    border.color: iconBoxHover.hovered || root.iconPickerOpen
                                  ? Theme.borderSubtle : Theme.border

                    Text {
                        x: 13; y: 5
                        text: qsTr("Icon")
                        color: Theme.accentLight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28; height: 28; radius: 6
                        // Neutral until the picker is actually open/hovered —
                        // an always-on accent tint here was one more place
                        // adding to "too many purples" on screen at once; it
                        // now mirrors the Icon box's OWN border highlight
                        // (iconBoxHover.hovered || root.iconPickerOpen) above.
                        color: (iconBoxHover.hovered || root.iconPickerOpen)
                               ? Qt.alpha(Theme.accentLight, 0.16) : Theme.card
                        IconGlyph {
                            anchors.centerIn: parent
                            name: root.currentIcon()
                            color: (iconBoxHover.hovered || root.iconPickerOpen)
                                   ? Theme.accentLight : Theme.textSecondary
                            width: 15; height: 15; fit: true
                        }
                    }
                    HoverHandler { id: iconBoxHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.iconPickerOpen = !root.iconPickerOpen }
                }
            }

            // The icon grid the header button and the Icon box both open.
            Rectangle {
                visible: root.iconPickerOpen && root.editLayout
                width: parent.width
                height: iconFlow.height + 20
                radius: Theme.radiusMd
                color: Theme.card
                border.width: 1
                border.color: Theme.border

                Flow {
                    id: iconFlow
                    x: 10; y: 10
                    width: parent.width - 20
                    spacing: 4

                    Repeater {
                        model: root.iconChoices
                        delegate: Rectangle {
                            id: iconChoice
                            required property string modelData
                            width: 34; height: 34; radius: 7
                            color: root.currentIcon() === iconChoice.modelData
                                   ? Qt.alpha(Theme.accentLight, 0.18)
                                   : (iconChoiceHover.hovered ? Theme.hoverBg : "transparent")
                            IconGlyph {
                                anchors.centerIn: parent
                                name: iconChoice.modelData
                                color: root.currentIcon() === iconChoice.modelData
                                       ? Theme.accentLight : Theme.textSecondary
                                width: 16; height: 16; fit: true
                            }
                            HoverHandler { id: iconChoiceHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    ActionListModel.setIcon(root.editingRow, iconChoice.modelData)
                                    root.iconPickerOpen = false
                                }
                            }
                        }
                    }
                }
            }

            // Activate by keypress
            Column {
                width: parent.width
                spacing: 6

                Text {
                    visible: !root.editLayout
                    text: qsTr("Activate by keypress")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                ShortcutCaptureField {
                    width: parent.width
                    variant: root.editLayout ? "boxed" : "field"
                    label: qsTr("Activate by keypress")
                    shortcut: root.editingShortcut
                    capturing: root.shortcutCaptureRow === root.editingRow
                    onCaptureRequested: root.shortcutCaptureRow = capturing ? -1 : root.editingRow
                    onCaptureCancelled: root.shortcutCaptureRow = -1
                    onShortcutEdited: (value) => {
                        ActionListModel.setShortcut(root.editingRow, value)
                        // Read back the accepted value immediately. The model can
                        // reject a duplicate key, and model notifications alone
                        // should not leave this open editor showing stale text.
                        root.editingShortcut = String(
                            ActionListModel.getAction(root.editingRow).shortcut || "")
                        root.shortcutCaptureRow = -1
                    }
                }
            }

            // Custom activation — opens the trigger list (the one event that
            // runs this action by itself). The row shows what is picked.
            Rectangle {
                width: parent.width
                height: 44
                radius: root.editLayout ? 7 : Theme.radiusMd
                color: triggerHover.hovered ? Theme.hoverBg
                     : (root.editLayout ? Theme.inset : Theme.card)
                border.width: root.editLayout ? 1 : 0
                border.color: Theme.border

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 13
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Custom activation")
                    color: root.editLayout ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
                Text {
                    anchors.right: bolt.left
                    anchors.rightMargin: root.currentTrigger() !== "" ? 8 : 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.currentTrigger() !== ""
                          ? root.triggerName(root.currentTrigger())
                          : qsTr("Choose trigger")
                    color: root.currentTrigger() !== "" ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                IconGlyph {
                    id: bolt
                    anchors.right: parent.right
                    anchors.rightMargin: 13
                    anchors.verticalCenter: parent.verticalCenter
                    name: "zap"
                    color: root.currentTrigger() !== "" ? Theme.accentLight : Theme.textMuted
                    width: 14; height: 14; fit: true
                }
                HoverHandler { id: triggerHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        root.iconPickerOpen = false
                        root.showAllTriggers = false
                        root.view = "triggers"
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border; opacity: 0.75 }

            // ---- Start action #n -------------------------------------------
            // What the action DOES comes before how it's triggered (FreeShow's
            // own order) — a single standard-height row per step (the "#n"
            // badge folds into the row itself, rather than a separate header
            // strip stacked above it — that made a plain step read as a tall
            // two-part mini-card instead of one normal row). A step that
            // takes a parameter (jump to group) carries a second standard
            // row below it for that choice. The empty card below is the
            // same shape with nothing picked yet, so a step never changes
            // shape the moment an action lands in it.
            Repeater {
                model: root.stepsFor(root.editingRow)
                delegate: Rectangle {
                    id: stepCard
                    required property int index
                    required property var modelData
                    readonly property var entry: ActionListModel.catalogEntry(modelData.effectId)
                    width: parent.width
                    height: stepCard.entry.needsGroup === true ? 44 + 1 + 44 : 44
                    radius: 6
                    color: Theme.inset
                    border.width: 1
                    border.color: Theme.border
                    clip: true

                    // The select row itself — the whole card's business.
                    Rectangle {
                        id: stepSelectRow
                        width: parent.width
                        height: 44
                        color: stepRowHover.hovered ? Theme.hoverBg : "transparent"

                        Text {
                            id: stepNum
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: "#" + (stepCard.index + 1)
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            font.weight: Font.Bold
                        }
                        IconGlyph {
                            id: stepIcon
                            anchors.left: stepNum.right
                            anchors.leftMargin: 9
                            anchors.verticalCenter: parent.verticalCenter
                            name: stepCard.entry.icon || "flag"
                            color: stepCard.entry.id ? Theme.accentLight : Theme.textMuted
                            width: 15; height: 15; fit: true
                        }
                        Text {
                            anchors.left: stepIcon.right
                            anchors.leftMargin: 9
                            anchors.right: parent.right
                            anchors.rightMargin: 70
                            anchors.verticalCenter: parent.verticalCenter
                            text: stepCard.entry.name || qsTr("Choose action")
                            color: stepCard.entry.id ? Theme.textPrimary : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            elide: Text.ElideRight
                        }

                        // Reorder / remove, only while the row is under the
                        // pointer — their own small hit targets sit on top
                        // of the row's own tap area, so a click on one of
                        // these takes it before "choose action" does.
                        Row {
                            id: stepActions
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            opacity: stepRowHover.hovered ? 1 : 0

                            Rectangle {
                                visible: stepCard.index > 0
                                width: 24; height: 24; radius: 6
                                color: upHover.hovered ? Qt.alpha(Theme.textPrimary, 0.08) : "transparent"
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: "chevronUp"
                                    color: Theme.textSecondary
                                    width: 11; height: 8
                                }
                                HoverHandler { id: upHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        const steps = root.stepsFor(root.editingRow).slice()
                                        const value = steps[stepCard.index - 1]
                                        steps[stepCard.index - 1] = steps[stepCard.index]
                                        steps[stepCard.index] = value
                                        root.updateSteps(root.editingRow, steps)
                                    }
                                }
                            }
                            Rectangle {
                                visible: root.stepsFor(root.editingRow).length > 1
                                width: 24; height: 24; radius: 6
                                color: dropHover.hovered ? Qt.alpha(Theme.danger, 0.16) : "transparent"
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: "close"
                                    color: dropHover.hovered ? Theme.dangerLight : Theme.textMuted
                                    width: 11; height: 11
                                }
                                HoverHandler { id: dropHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        const steps = root.stepsFor(root.editingRow).slice()
                                        steps.splice(stepCard.index, 1)
                                        root.updateSteps(root.editingRow, steps)
                                    }
                                }
                            }
                        }

                        HoverHandler { id: stepRowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.chooseStep(stepCard.index) }
                    }

                    // ---- the step's own parameter (jump to group) ----------
                    // A second band, shaped exactly like the row above it:
                    // the group's own colour mark, its name (or "Choose
                    // group") and a chevron into the group roster.
                    Rectangle {
                        visible: stepCard.entry.needsGroup === true
                        y: 44
                        width: parent.width
                        height: 1
                        color: Theme.border
                    }
                    Rectangle {
                        visible: stepCard.entry.needsGroup === true
                        y: 45
                        width: parent.width
                        height: 44
                        color: Theme.surface

                        GroupPickerField {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            flat: true
                            selectedGroup: stepCard.modelData.effectValue || ""
                            onGroupPicked: (name) => {
                                const steps = root.stepsFor(root.editingRow).slice()
                                steps[stepCard.index] = { effectId: stepCard.modelData.effectId,
                                                          effectValue: name }
                                root.updateSteps(root.editingRow, steps)
                            }
                        }
                    }
                }
            }

            // The empty card — the same shape, waiting for a pick.
            Rectangle {
                visible: root.stepsFor(root.editingRow).length === 0
                width: parent.width
                height: 44
                radius: 6
                color: chooserHover.hovered ? Theme.hoverBg : Theme.inset
                border.width: 1
                border.color: Theme.border
                clip: true

                Text {
                    id: emptyStepNum
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "#1"
                    color: Theme.accentLight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Bold
                }
                IconGlyph {
                    id: emptyStepIcon
                    anchors.left: emptyStepNum.right
                    anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    name: "flag"
                    color: chooserHover.hovered ? Theme.accentLight : Theme.textMuted
                    width: 15; height: 15; fit: true
                }
                Text {
                    anchors.left: emptyStepIcon.right
                    anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Choose action")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
                HoverHandler { id: chooserHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.chooseStep(0) }
            }

            AddActionRow {
                onPicked: root.chooseStep(root.stepsFor(root.editingRow).length)
            }
        }

        // ---- custom activations: the trigger list -------------------------
        // The design's own screen: one flat, quiet list inside a single
        // rounded panel, every row an event this action could run on. The
        // header keeps "Action" and gains a back arrow; the eye beside the X
        // narrows the list to the triggers already in use (see shownTriggers).
        Column {
            visible: root.view === "triggers"
            width: parent.width
            spacing: 8

            StripedListPanel {
                id: triggerPanel
                width: parent.width

                Repeater {
                    model: root.shownTriggers()
                    delegate: Rectangle {
                        id: triggerRow
                        required property var modelData
                        required property int index
                        width: parent.width
                        height: triggerPanel.rowHeight
                        radius: triggerPanel.rowRadius
                        // Both tints come from the panel, so this list and the
                        // action catalog cannot drift apart.
                        color: triggerPanel.rowColor(index, triggerRowHover.hovered)

                        readonly property string usedBy: root.usedElsewhere(modelData.id)
                        readonly property bool current: root.currentTrigger() === modelData.id

                        Row {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                name: triggerRow.modelData.icon || "flag"
                                color: triggerRow.current ? Theme.accentLight : Theme.textSecondary
                                width: 14; height: 14; fit: true
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: triggerRow.modelData.name
                                color: triggerRow.current ? Theme.accentLight : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                        }

                        Text {
                            visible: triggerRow.usedBy !== ""
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("used by %1").arg(triggerRow.usedBy)
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            elide: Text.ElideRight
                            width: Math.min(implicitWidth, parent.width * 0.4)
                            horizontalAlignment: Text.AlignRight
                        }

                        HoverHandler { id: triggerRowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.chooseTrigger(triggerRow.modelData.id) }
                    }
                }
            }

            // Clearing the trigger: a custom activation is optional, so a
            // picked one has to be removable without deleting the action.
            Rectangle {
                visible: root.currentTrigger() !== ""
                width: parent.width
                height: 36
                radius: 7
                color: clearHover.hovered ? Theme.hoverBg : "transparent"
                border.width: 1
                border.color: Theme.border

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Remove custom activation")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { ActionListModel.setTrigger(root.editingRow, ""); root.view = "editor" } }
            }
        }

        // ---- the searchable, sectioned action catalog ---------------------
        // Unsupported actions stay visible as Coming soon and cannot be
        // selected yet.
        Column {
            visible: root.view === "catalog"
            width: parent.width
            spacing: 8

            // The app's standard search look (TabSearchBox's own visual
            // language: magnifier, clear ×, accent underline — no boxed
            // border) instead of a bordered Theme.inset field, so this reads
            // like every other search in the app rather than a one-off
            // settings input. The reveal eye lives in the header beside the
            // close X (see headerActionIcon above), so this box gets the
            // dialog's full width instead of sharing it with an inline button.
            Item {
                id: catalogSearchBox
                width: parent.width
                height: 36

                Rectangle {
                    anchors.fill: parent
                    anchors.bottomMargin: 1
                    radius: Theme.radiusSm
                    color: Theme.inset
                    opacity: 0.75
                }

                    IconGlyph {
                        id: catalogSearchGlass
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 13; height: 13
                        name: "search"
                        color: Theme.textMuted
                        fit: true
                    }
                    TextInput {
                        id: catalogSearchInput
                        anchors.left: catalogSearchGlass.right
                        anchors.leftMargin: 10
                        anchors.right: catalogSearchClear.left
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        clip: true
                        selectByMouse: true
                        onTextChanged: root.catalogSearch = text
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Search actions")
                            color: Theme.textMuted
                            font: parent.font
                            visible: parent.text.length === 0
                        }
                    }
                    Rectangle {
                        id: catalogSearchClear
                        visible: catalogSearchInput.text.length > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18; height: 18; radius: 9
                        color: catalogClearHover.hovered ? Theme.hoverBg : "transparent"
                        IconGlyph {
                            anchors.centerIn: parent
                            name: "close"
                            color: Theme.textSecondary
                            width: 9; height: 9
                        }
                        HoverHandler { id: catalogClearHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { catalogSearchInput.text = ""; catalogSearchInput.forceActiveFocus() }
                        }
                    }
                    // Always the app's accent colour, not just on focus —
                    // the same always-on underline TabSearchBox draws (the
                    // app's one other search box), not a conditional one.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        radius: 1
                        color: Theme.accent
                    }
                }

            // Same StripedListPanel as the trigger list and the roster: one
            // ground, one hairline, one pair of row colours for all three.
            StripedListPanel {
                id: catalogPanel
                width: parent.width

                    Repeater {
                        model: root.filteredCatalog()
                        delegate: Column {
                            id: catalogItem
                            required property int index
                            required property var modelData
                            width: parent.width
                            spacing: 0

                            Text {
                                visible: root.isSectionStart(root.filteredCatalog(), index)
                                width: parent.width
                                height: visible ? 34 : 0
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignHCenter
                                text: modelData.section
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.DemiBold
                            }

                            Rectangle {
                                width: parent.width
                                height: catalogPanel.rowHeight
                                radius: catalogPanel.rowRadius
                                // The panel decides both tints, so this list can
                                // never drift from the others. The dim of a
                                // "Coming soon" row lives on its CONTENTS, not on
                                // the row — scaling the row faded its stripe too,
                                // which is what made the alternation look uneven.
                                color: catalogPanel.rowColor(catalogItem.index, itemHover.hovered)

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10; anchors.rightMargin: 12
                                    spacing: 10
                                    opacity: modelData.supported ? 1 : 0.58
                                    IconGlyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: modelData.icon || "flag"
                                        color: modelData.supported ? Theme.accentLight : Theme.textMuted
                                        width: 14; height: 14; fit: true
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 120
                                        text: modelData.name
                                        color: modelData.supported ? Theme.textPrimary : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        visible: !modelData.supported
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("Coming soon")
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                                HoverHandler { id: itemHover; enabled: modelData.supported }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: modelData.supported
                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: root.selectCatalogEntry(modelData)
                                }
                            }
                        }
                    }
            }
        }
    }
}
