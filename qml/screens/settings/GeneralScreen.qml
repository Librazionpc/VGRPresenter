import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · General — a faithful rebuild of the two reference exports
// (1BBTIwaya's VGRPresenter_Settings_General.qml and its _Scrolled state):
// one scrollable page holding the Smart Config banner, the Appearance /
// Startup two-card row, the Resource profile strip, then (scrolling, per
// the _Scrolled reference) Preferences, Backups & recovery and
// Notifications & logs. The page scrolls via the shared AppScrollBar —
// deliberately NOT Qt's ScrollBar, same visual language as the rest
// of the app.
//
// EVERY control here is live and drives the ENGINE (bps::settings::AppSettings, through SettingsService): what each setting is
// called, what it defaults to, what it may be set to and what it does are the engine's. This page carries no state and no list
// of options of its own - a toggle reads SettingsService.values[key] and writes SettingsService.setValue(key, ...), a
// "pick one" is a SettingsChoice, and the engine refuses anything it does not allow. The app then acts on the change: the
// accent recolours the interface, notifications, autosave, backups, crash recovery, start-up and tray behaviour follow the
// switches, and the resource profile, Lock In Mode and log level are applied to the engine itself.
//
// Rooted in an Item (not a bare Flickable) so the scrollbar can sit fixed
// at this screen's right edge while the content beneath it scrolls — a
// scrollbar declared inside a Flickable would scroll away with the
// content. ModalShell drops this whole Item into its content area.
//
// Theme tokens throughout, so it follows the Appearance > Theme choice with
// the rest of the app.
Item {
    id: root

    // Asks the settings shell to show another section ("smart" = Smart Config).
    signal sectionRequested(string key)

    readonly property var values: SettingsService.values
    readonly property var definitions: SettingsService.definitions

    // The label of the choice a "pick one" setting is on ("Every 30 minutes"). Reads `values`, so a binding using it follows the setting.
    function choiceText(key) {
        const def = root.definitions[key]
        const current = root.values[key]
        if (def)
            for (let i = 0; i < def.choices.length; ++i)
                if (def.choices[i].value === current)
                    return def.choices[i].label
        return current === undefined ? "" : String(current)
    }
    // The label of one of the resource profiles (by its engine value).
    function profileLabel(value) {
        const choices = root.definitions["resources.profile"].choices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === value)
                return choices[i].label
        return ""
    }
    function flip(key) { SettingsService.setValue(key, !root.values[key]) }

    // A backup's engine stamp ("yyyymmdd-hhmmss") as a person reads a time, and
    // a byte count as KB/MB. Shared by the open-show row and the all-shows
    // browser below so both format identically.
    function backupStampText(stamp) {
        const s = String(stamp)
        if (s.length !== 15 || s.charAt(8) !== "-")
            return s
        const d = new Date(Number(s.substr(0, 4)), Number(s.substr(4, 2)) - 1, Number(s.substr(6, 2)),
                           Number(s.substr(9, 2)), Number(s.substr(11, 2)))
        return Qt.formatDateTime(d, "d MMM yyyy, hh:mm")
    }
    function backupSizeText(bytes) {
        const kb = Number(bytes) / 1024
        return kb < 1024 ? Math.max(1, Math.round(kb)) + qsTr(" KB") : (kb / 1024).toFixed(1) + qsTr(" MB")
    }
    // A backup's category as a person reads it (the browser row's title and the
    // restore dialogs). "show" is not a real category here — those rows name
    // the show itself.
    function backupCategoryLabel(category) {
        return category === "settings" ? qsTr("Settings")
             : category === "overlays" ? qsTr("Overlays")
             : category === "templates" ? qsTr("Templates")
             : String(category)
    }

    Flickable {
        id: flick
        objectName: "selfTestGeneralFlick"
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
                    text: qsTr("General")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Application appearance, startup behavior and smart setup.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Smart Config banner ----
            Rectangle {
                width: parent.width
                height: 106
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    x: 20
                    y: 16
                    width: parent.width - 180
                    spacing: 6

                    Rectangle {
                        width: smartLabel.width + 20
                        height: 20
                        radius: Theme.radiusSm
                        color: Theme.accentSoft

                        Text {
                            id: smartLabel
                            anchors.centerIn: parent
                            text: qsTr("SMART CONFIG")
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                    }

                    // What the engine found on this machine and what it recommends (see Smart Config for the details).
                    Text {
                        text: SettingsService.hardwareHeadline !== "" ? SettingsService.hardwareHeadline : qsTr("Analyzing this hardware…")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: SettingsService.hardwareDetail
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.Wrap
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: 116
                    height: 36
                    radius: Theme.radiusMd
                    color: reviewArea.containsMouse ? Theme.accentLight : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Review setup")
                        color: "#ffffff"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: reviewArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.sectionRequested("smart")
                    }
                }
            }

            // ---- Appearance / Startup two-card row ----
            Row {
                width: parent.width
                spacing: 16

                // Appearance card — two "pick one" rows, one toggle row, the
                // accent swatches. Built explicitly (not a Repeater)
                // because each row's right-hand control differs.
                Rectangle {
                    objectName: "selfTestCardAppearance"   // inset self-test grab target
                    width: (parent.width - 16) / 2
                    height: appearanceCol.height + 40
                    radius: Theme.radiusLg
                    color: Theme.card
                    border.color: Theme.border
                    border.width: 1

                    Column {
                        id: appearanceCol
                        x: 20
                        y: 20
                        width: parent.width - 40
                        spacing: 0

                        Text {
                            text: qsTr("Appearance")
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            bottomPadding: 10
                        }

                        // A label on the left and the engine's list of choices on the right.
                        component ChoiceRow: Item {
                            id: choiceRow
                            property string settingKey: ""
                            width: parent.width
                            height: 36

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.definitions[choiceRow.settingKey] ? root.definitions[choiceRow.settingKey].label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                            SettingsChoice {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                settingKey: choiceRow.settingKey
                            }
                        }

                        ChoiceRow { settingKey: "appearance.theme" }
                        Rectangle { x: 2; width: parent.width - 2; height: 1; color: Theme.border }
                        ChoiceRow { settingKey: "appearance.language" }
                        Rectangle { x: 2; width: parent.width - 2; height: 1; color: Theme.border }

                        // Lock In Mode — a toggle row.
                        Item {
                            width: appearanceCol.width
                            height: 36

                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1
                                Text {
                                    text: root.definitions["appearance.lockInMode"].label
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }
                            }
                            SettingsToggle {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                checked: root.values["appearance.lockInMode"] === true
                                onToggled: root.flip("appearance.lockInMode")
                            }
                        }
                        Rectangle { x: 2; width: parent.width - 2; height: 1; color: Theme.border }

                        // Accent color — the engine's swatches, the selected one
                        // wrapped in a ring (with a 2px gap, like the reference).
                        Item {
                            width: appearanceCol.width
                            height: 38

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.definitions["appearance.accent"].label
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8

                                Repeater {
                                    model: root.definitions["appearance.accent"].choices
                                    delegate: Rectangle {
                                        id: swatch
                                        required property var modelData
                                        readonly property bool selected: root.values["appearance.accent"] === swatch.modelData.value
                                        width: 26
                                        height: 26
                                        radius: 13
                                        color: "transparent"
                                        border.width: swatch.selected ? 2 : 0
                                        border.color: swatch.modelData.color

                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 18
                                            height: 18
                                            radius: 9
                                            color: swatch.modelData.color
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: SettingsService.setValue("appearance.accent", swatch.modelData.value)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Startup card — the toggles, the autosave interval and the updates row.
                Rectangle {
                    objectName: "selfTestCardStartup"   // inset self-test grab target
                    width: (parent.width - 16) / 2
                    height: startupCol.height + 40
                    radius: Theme.radiusLg
                    color: Theme.card
                    border.color: Theme.border
                    border.width: 1

                    Column {
                        id: startupCol
                        x: 20
                        y: 20
                        width: parent.width - 40
                        spacing: 0

                        Text {
                            text: qsTr("Startup")
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            bottomPadding: 10
                        }

                        Repeater {
                            model: ["startup.openLastProject", "startup.launchAtLogin", "startup.autosave"]
                            delegate: Column {
                                id: startRow
                                required property string modelData
                                width: startupCol.width

                                Item {
                                    width: startRow.width
                                    height: 36

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.definitions[startRow.modelData].label
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                    SettingsToggle {
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        checked: root.values[startRow.modelData] === true
                                        onToggled: root.flip(startRow.modelData)
                                    }
                                }
                                Rectangle { x: 2; width: startRow.width - 2; height: 1; color: Theme.border }
                            }
                        }

                        // How often autosave runs - only meaningful while Autosave is on.
                        Item {
                            width: startupCol.width
                            height: 36
                            opacity: root.values["startup.autosave"] === true ? 1 : 0.45

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.definitions["startup.autosaveSeconds"].label
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                            SettingsChoice {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                settingKey: "startup.autosaveSeconds"
                                enabled: root.values["startup.autosave"] === true
                            }
                        }
                        Rectangle { x: 2; width: parent.width - 2; height: 1; color: Theme.border }

                        // Check for updates — the version in use + a button, not a toggle.
                        Item {
                            width: startupCol.width
                            height: 36

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Check for updates")
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 130
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("v%1").arg(SettingsService.appVersion)
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }

                            Rectangle {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 56
                                height: 26
                                radius: Theme.radiusSm
                                color: checkArea.containsMouse ? Theme.chip : Theme.inset
                                border.color: Theme.border
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("Check")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }

                                MouseArea {
                                    id: checkArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    // There is no update server to ask yet, so the honest answer is which version this is.
                                    onClicked: EventBus.notify(qsTr("No update service is set up for this build yet. You are running v%1.").arg(SettingsService.appVersion),
                                                               "info", qsTr("Updates"), "settings.updates.check")
                                }
                            }
                        }
                    }
                }
            }

            // ---- Resource profile strip ----
            Rectangle {
                width: parent.width
                height: 152
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1
                // The purple accent bar hugging the card's left edge.
                Rectangle {
                    x: 0
                    y: 16
                    width: 3
                    height: parent.height - 32
                    radius: 1.5
                    color: Theme.accent
                }

                Text {
                    x: 20
                    y: 16
                    text: root.definitions["resources.profile"].label
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Text {
                    x: 20
                    y: 38
                    text: root.definitions["resources.profile"].description
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // Which profile the engine recommends for this machine.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 12
                    visible: SettingsService.recommendedProfile !== ""
                    text: qsTr("RECOMMENDED · %1").arg(root.profileLabel(SettingsService.recommendedProfile).toUpperCase())
                    color: Theme.accentLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }

                Row {
                    id: manageRow
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 30
                    spacing: 16

                    Repeater {
                        model: root.definitions["resources.profile"].choices
                        delegate: Rectangle {
                            id: segRoot
                            required property var modelData
                            readonly property bool active: root.values["resources.profile"] === segRoot.modelData.value
                            width: segLabel.width + 28
                            height: 30
                            radius: Theme.radiusMd
                            color: segRoot.active ? Theme.accentSoft : (segArea.containsMouse ? Theme.chip : Theme.inset)
                            border.width: 1
                            border.color: segRoot.active ? Theme.accent : Theme.border
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: segLabel
                                anchors.centerIn: parent
                                text: segRoot.modelData.label
                                color: segRoot.active ? Theme.accentLight : Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                                font.weight: segRoot.active ? Font.DemiBold : Font.Medium
                            }

                            MouseArea {
                                id: segArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: SettingsService.setValue("resources.profile", segRoot.modelData.value)
                            }
                        }
                    }
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 66
                    text: qsTr("Manage →")
                    color: Theme.accentLight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.sectionRequested("smart")
                    }
                }

                // The three LIVE usage meters — the engine's Telemetry module
                // (real frame times, encoder load, output presents), not the
                // profile's static budget. The allocation split stays visible
                // as the per-profile targets under the card title.
                Row {
                    x: 20
                    y: 92
                    width: parent.width - 40
                    spacing: 24

                    // The shared meter component (see LabeledMeter.qml) —
                    // the same one Smart Config's Resource budgets use, so
                    // the two screens' meters can't drift apart.
                    LabeledMeter { label: qsTr("Rendering"); pct: TelemetryService.utilization.rendering; width: (parent.width - 48) / 3 }
                    LabeledMeter { label: qsTr("Encoding"); pct: TelemetryService.utilization.encoding; width: (parent.width - 48) / 3 }
                    LabeledMeter { label: qsTr("Output"); pct: TelemetryService.utilization.output; width: (parent.width - 48) / 3 }
                }

                // The profile's budget split (was the meters' only source
                // before the live feed existed — kept as the stated targets).
                Text {
                    x: 20
                    y: 128
                    text: qsTr("Budget for this profile · Rendering %1% · Encoding %2% · Output %3%")
                        .arg(SettingsService.allocation.rendering)
                        .arg(SettingsService.allocation.encoding)
                        .arg(SettingsService.allocation.output)
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
            }

            // ---- Scrolled-state sections (the _Scrolled reference) ----

            component SettingsSection: Rectangle {
                id: section
                property string title: ""
                default property alias rows: sectionCol.data
                width: layout.width
                // The SAME 20px internal padding as the Appearance / Startup cards
                // above, so every card on the page shares one set of insets: title
                // at y: 20, rows at y: 51 (= 20 + the ~21px title + its 10px gap -
                // exactly where those cards put their first row), and 20px under the
                // last row. (It used to be 14px top and bottom, which read a step
                // tighter than the two cards above it.)
                height: sectionCol.height + sectionCol.y + 20
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Text {
                    x: 20
                    y: 20
                    text: section.title
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Column {
                    id: sectionCol
                    x: 20
                    y: 51
                    width: parent.width - 40
                    spacing: 0
                }
            }

            // One row of a section: a label (and a line under it), with a toggle (`key`), a "pick one" (`choiceKey`) or a value on the right.
            component PrefRow: Column {
                id: prefRow
                // The engine's key of the setting this row is. The label and description come from its definition unless overridden.
                property string key: ""
                property string choiceKey: ""
                property string label: root.definitions[prefRow.key !== "" ? prefRow.key : prefRow.choiceKey]
                                       ? root.definitions[prefRow.key !== "" ? prefRow.key : prefRow.choiceKey].label : ""
                property string sub: ""
                property bool last: false
                // parent = the SettingsSection's inner column this row is
                // declared in (PrefRow can't see that column's id from its
                // own definition scope).
                width: parent.width

                Item {
                    width: prefRow.width
                    height: prefRow.sub !== "" ? 52 : 40

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        Text {
                            text: prefRow.label
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            visible: prefRow.sub !== ""
                            text: prefRow.sub
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                    }

                    SettingsToggle {
                        visible: prefRow.key !== ""
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: prefRow.key !== "" && root.values[prefRow.key] === true
                        onToggled: root.flip(prefRow.key)
                    }

                    SettingsChoice {
                        visible: prefRow.choiceKey !== ""
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        settingKey: prefRow.choiceKey
                    }
                }
                Rectangle {
                    visible: !prefRow.last
                    // Start at the row label's left edge, not the section's
                    // padded edge (the divider used to lead the text by 2px).
                    x: 2
                    width: prefRow.width - 2
                    height: 1
                    color: Theme.border
                }
            }

            // The OPEN SHOW's dated backups. The engine has always kept them (DataProtection's BackupStore)
            // and could list them (SettingsService.backupsOf), but nothing ever showed one - they were
            // write-only. This row is the read side: it lists them and hands one back as a NEW show.
            component BackupRow: Column {
                id: backupRow
                property bool last: false
                width: parent.width

                // Backups are keyed by the SOURCE FILE's name without its extension (the engine's
                // completeBaseName) - NOT by the show document's title, which usually differs. Deriving it
                // from the path is what makes the list actually find this show's copies.
                readonly property string fileStem: {
                    const p = String(ShowService.showPath ?? "")
                    if (p === "")
                        return ""
                    // The file name (after the last separator) without its extension — the character-class
                    // regex a path split usually uses reads badly inside a QML string, so index arithmetic it is.
                    const slash = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"))
                    const file = slash >= 0 ? p.substring(slash + 1) : p
                    const dot = file.lastIndexOf(".")
                    return dot > 0 ? file.substring(0, dot) : file
                }
                readonly property string title: ShowService.hasShow && String(ShowService.showName ?? "") !== ""
                                                ? String(ShowService.showName) : backupRow.fileStem
                // Re-read from the engine when the list is opened and after a restore (backups are taken on
                // a timer, so there is no signal to bind to; reading on demand is always current).
                property var backups: []
                function refresh() {
                    backupRow.backups = backupRow.fileStem !== "" ? SettingsService.backupsOf(backupRow.fileStem) : []
                }
                Component.onCompleted: refresh()
                Connections {
                    target: ShowService
                    function onShowChanged() { backupRow.refresh() }
                }

                // Stamp/size formatting lives on the root now (backupStampText /
                // backupSizeText) — the all-shows browser formats the same way.

                // Copies a backup out as a NEW library show; the show it came from is never touched.
                function restore(backupPath) {
                    const name = qsTr("%1 (backup)").arg(backupRow.title)
                    const target = ShowService.newLibraryShowPath("", name)
                    if (target === "" || !SettingsService.restoreBackup(backupPath, target)) {
                        EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                        return
                    }
                    ShowService.refreshLibrary()
                    backupRow.refresh()
                    EventBus.notify(qsTr("Restored \"%1\" as a new show.").arg(name), "info", qsTr("Backups"), "settings.backups.restored")
                }

                Item {
                    width: backupRow.width
                    height: 52

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        Text {
                            text: backupRow.fileStem === "" ? qsTr("Backups")
                                                             : qsTr("Backups of \"%1\"").arg(backupRow.title)
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            text: backupRow.fileStem === "" ? qsTr("Open a show to see its backups.")
                                : (backupRow.backups.length === 0 ? qsTr("None yet — a copy is taken while it is open.")
                                                                  : qsTr("%n saved copy(s).", "", backupRow.backups.length))
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                    }

                    Rectangle {
                        id: restoreBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(72, restoreText.width + 24)
                        height: 26
                        radius: Theme.radiusSm
                        // Nothing to hand back with an empty list: dimmed and inert.
                        opacity: backupRow.backups.length > 0 ? 1 : 0.45
                        color: restoreArea.containsMouse ? Theme.chip : Theme.inset
                        border.color: Theme.border
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            id: restoreText
                            anchors.centerIn: parent
                            text: qsTr("Restore")
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: restoreArea
                            anchors.fill: parent
                            enabled: backupRow.backups.length > 0
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // Re-read first: the list is only as current as the moment it opens.
                            onClicked: {
                                backupRow.refresh()
                                backupMenu.openAt(restoreBtn, restoreBtn.width - backupMenu.width, restoreBtn.height + 4, root.Window.contentItem)
                            }
                        }
                    }

                    // The engine's own list, newest first; the trailing text is that copy's size.
                    DropdownPanel {
                        id: backupMenu
                        visible: false
                        fitContentWidth: true
                        maxHeight: 320
                        model: backupRow.backups.map((b) => ({ label: root.backupStampText(b.stamp),
                                                               trailing: root.backupSizeText(b.sizeBytes),
                                                               payload: b.path }))
                        onItemPicked: (index, payload) => {
                            backupMenu.visible = false
                            backupRow.restore(payload)
                        }
                    }
                    MenuCatcher { menu: backupMenu }
                }

                Rectangle {
                    visible: !backupRow.last
                    // Inset like PrefRow's divider: start at the row text's
                    // left edge rather than 2px to its left.
                    x: 2
                    width: backupRow.width - 2
                    height: 1
                    color: Theme.border
                }
            }

            SettingsSection {
                objectName: "selfTestCardPreferences"   // inset self-test grab target
                title: qsTr("Preferences")
                PrefRow { key: "preferences.startMinimized" }
                PrefRow { key: "preferences.restoreLastSession" }
                PrefRow { key: "preferences.closeToTray"; last: true }
            }

            SettingsSection {
                objectName: "selfTestCardBackups"   // inset self-test grab target
                title: qsTr("Backups & recovery")
                PrefRow { key: "backups.automatic"; sub: root.choiceText("backups.intervalMinutes") }
                PrefRow { choiceKey: "backups.intervalMinutes" }
                PrefRow { choiceKey: "backups.keepLast" }
                PrefRow { key: "backups.crashRecovery"; sub: root.definitions["backups.crashRecovery"].description }
                // What else a backup carries beyond the open show — each opt-in,
                // so the user decides how much a backup takes.
                PrefRow { key: "backups.includeSettings"; sub: root.definitions["backups.includeSettings"].description }
                PrefRow { key: "backups.includeOverlays"; sub: root.definitions["backups.includeOverlays"].description }
                PrefRow { key: "backups.includeTemplates"; sub: root.definitions["backups.includeTemplates"].description }
                BackupRow { last: true }
            }

            SettingsSection {
                objectName: "selfTestCardNotifications"   // inset self-test grab target
                title: qsTr("Notifications & logs")
                PrefRow { key: "notifications.show" }
                PrefRow { choiceKey: "notifications.logLevel"; last: true }
            }

            // A row with a label on the left and a small action button on the right.
            component ActionRow: Item {
                id: actionRow
                property string label: ""
                property string buttonText: ""
                signal activated()
                width: parent.width
                height: 40

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: actionRow.label
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(64, actionText.width + 24)
                    height: 26
                    radius: Theme.radiusSm
                    color: actionArea.containsMouse ? Theme.chip : Theme.inset
                    border.color: Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        id: actionText
                        anchors.centerIn: parent
                        text: actionRow.buttonText
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: actionArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: actionRow.activated()
                    }
                }
            }

            // The design libraries (the dock's Overlays / Templates tabs): brings back what ships
            // after the user deleted it. The engine fills in what is missing; nothing else is touched.
            component RestoreRow: ActionRow {
                id: restoreRow
                property var service: null          // OverlayLibraryService | TemplateLibraryService
                readonly property string noun: service === OverlayLibraryService ? qsTr("overlay") : qsTr("template")
                buttonText: qsTr("Restore")
                onActivated: {
                    const n = restoreRow.service.restoreDefaults()
                    EventBus.notify(n > 0 ? qsTr("Brought back %n default %1(s).", "", n).arg(restoreRow.noun)
                                          : qsTr("All the default %1s are already here.").arg(restoreRow.noun),
                                    "info", qsTr("Settings"), "settings.libraries.restore")
                }
            }

            // EVERY backup, not just the open show's (the row in "Backups &
            // recovery" above): a show copy restores as a new show, a settings /
            // overlays / templates copy rolls back over its live file — or any
            // of them can be deleted. The list lives in the browser modal at the
            // root, which reads it on open.
            SettingsSection {
                title: qsTr("All backups")
                ActionRow {
                    label: qsTr("Browse every saved copy")
                    buttonText: qsTr("Browse…")
                    onActivated: backupsBrowser.open()
                }
                Rectangle {
                    x: 2
                    width: parent.width - 2
                    height: 1
                    color: Theme.border
                }
                // The backups live outside the app's document folders, so the browser
                // below is not the only way to reach them: hand the OS file manager
                // the folder itself (SettingsService.backupDirectory()).
                ActionRow {
                    label: qsTr("Open the backups folder")
                    buttonText: qsTr("Open")
                    onActivated: {
                        if (!SettingsService.openBackupDirectory())
                            EventBus.notify(qsTr("The backups folder is not ready yet."), "warning", qsTr("Backups"), "settings.backups.openFolder")
                    }
                }
                Rectangle {
                    x: 2
                    width: parent.width - 2
                    height: 1
                    color: Theme.border
                }
                // Takes the same copies the automatic timer would, on demand:
                // the open show (if any) plus whichever categories are on.
                ActionRow {
                    label: qsTr("Back up the open show and everything selected")
                    buttonText: qsTr("Back up now")
                    onActivated: {
                        let made = 0
                        if (ShowService.showPath !== "" && SettingsService.backupShow(ShowService.showPath) !== "")
                            ++made
                        made += SettingsService.backupCategories()
                        EventBus.notify(made > 0 ? qsTr("Made %n new saved copy(s).", "", made)
                                                 : qsTr("Everything selected is already saved."),
                                        "info", qsTr("Backups"), "settings.backups.now")
                    }
                }
            }

            SettingsSection {
                objectName: "selfTestCardLibraries"   // inset self-test grab target
                title: qsTr("Libraries")
                RestoreRow { label: qsTr("Restore default overlays"); service: OverlayLibraryService }
                RestoreRow { label: qsTr("Restore default templates"); service: TemplateLibraryService }
            }

            SettingsSection {
                objectName: "selfTestCardReset"   // inset self-test grab target
                title: qsTr("Reset")
                ActionRow {
                    label: qsTr("Reset all settings to their defaults")
                    buttonText: qsTr("Reset")
                    onActivated: {
                        const n = SettingsService.resetAll()
                        EventBus.notify(n > 0 ? qsTr("%n setting(s) went back to their defaults.", "", n)
                                              : qsTr("Every setting is already at its default."),
                                        "info", qsTr("Settings"), "settings.reset")
                    }
                }
            }
        }
    }

    // Shared app scrollbar at the screen's fixed right edge (a sibling of
    // the Flickable, not a child — see the root Item note above).
    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }

    // ---- The all-shows backups browser --------------------------------------
    // EVERY backup the engine holds (the modal the "All backups" card opens),
    // newest first. Read on each open — backups are taken on a timer, so there
    // is no signal to bind to. Restore hands a copy back as a NEW show; Delete
    // removes the copy after the confirm dialog below.
    ModalCard {
        id: backupsBrowser
        title: qsTr("Backups")
        subtitle: qsTr("Every saved copy the engine has taken, newest first.")
        cardWidth: 620
        showSave: false
        cancelText: qsTr("Close")
        onCancelled: backupsBrowser.close()

        property var rows: []
        // The row a Delete is waiting to confirm (drives the dialog below).
        property var pendingDelete: null
        // The settings/overlays/templates row a Restore is waiting to confirm.
        property var pendingRestore: null

        function reload() { backupsBrowser.rows = SettingsService.allBackups() }
        onShownChanged: if (backupsBrowser.shown) backupsBrowser.reload()

        // A category copy (settings / overlays / templates) rolls back OVER its
        // live file, so it goes through the confirm dialog below. A show copy
        // only ever ADDS a new show (the engine refuses to overwrite one), so it
        // needs no confirm and restores straight away.
        function restore(row) {
            if (!row)
                return
            if (row.category !== "show") {
                backupsBrowser.pendingRestore = row
                return
            }
            const name = qsTr("%1 (backup)").arg(row.show)
            const target = ShowService.newLibraryShowPath("", name)
            if (target === "" || !SettingsService.restoreBackup(row.path, target)) {
                EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                return
            }
            ShowService.refreshLibrary()
            backupsBrowser.reload()
            EventBus.notify(qsTr("Restored \"%1\" as a new show.").arg(name), "info", qsTr("Backups"), "settings.backups.restored")
        }

        // Puts a settings/overlays/templates copy back in place (the engine also
        // reloads whatever reads it). Only reached after the confirm dialog.
        function restoreCategory(row) {
            if (!row)
                return
            if (!SettingsService.restoreCategoryBackup(row.path)) {
                EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                return
            }
            backupsBrowser.reload()
            EventBus.notify(qsTr("%1 was put back from its backup.").arg(root.backupCategoryLabel(row.category)),
                            "info", qsTr("Backups"), "settings.backups.restored")
        }

        // Reveals THIS copy in the OS file manager (Explorer/Finder select it;
        // a desktop without that opens the folder).
        function showInFolder(row) {
            if (!row || SettingsService.showBackupInFolder(row.path))
                return
            EventBus.notify(qsTr("That backup could not be shown in the file manager."),
                            "warning", qsTr("Backups"), "settings.backups.showInFolder")
        }

        function remove(row) {
            if (row && SettingsService.deleteBackup(row.path)) {
                backupsBrowser.reload()
                EventBus.notify(qsTr("Deleted a backup of \"%1\".").arg(row.show), "info", qsTr("Backups"), "settings.backups.deleted")
            } else {
                EventBus.notify(qsTr("That backup could not be deleted."), "error", qsTr("Backups"), "settings.backups.deleteRefused")
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                visible: backupsBrowser.rows.length === 0
                width: parent.width
                text: qsTr("No backups yet. A copy is taken while a show is open.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }

            Repeater {
                model: backupsBrowser.rows

                delegate: Rectangle {
                    id: backupItem
                    required property var modelData
                    width: parent.width
                    height: 46
                    radius: Theme.radiusSm
                    color: rowHover.containsMouse ? Theme.inset : "transparent"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    // Hover-only wash under the row (no button accepted), so it
                    // never eats the Restore/Delete clicks above it.
                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1

                        Text {
                            text: backupItem.modelData.category === "show"
                                  ? backupItem.modelData.show
                                  : root.backupCategoryLabel(backupItem.modelData.category)
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            text: root.backupStampText(backupItem.modelData.stamp) + "  ·  "
                                  + root.backupSizeText(backupItem.modelData.sizeBytes)
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Rectangle {
                            width: Math.max(96, revealLbl.width + 20)
                            height: 26
                            radius: Theme.radiusSm
                            color: revealArea.containsMouse ? Theme.chip : Theme.inset
                            border.color: Theme.border
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: revealLbl
                                anchors.centerIn: parent
                                text: qsTr("Show in folder")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: revealArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backupsBrowser.showInFolder(backupItem.modelData)
                            }
                        }

                        Rectangle {
                            width: Math.max(64, restoreLbl.width + 20)
                            height: 26
                            radius: Theme.radiusSm
                            color: restoreArea.containsMouse ? Theme.chip : Theme.inset
                            border.color: Theme.border
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: restoreLbl
                                anchors.centerIn: parent
                                text: qsTr("Restore")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: restoreArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backupsBrowser.restore(backupItem.modelData)
                            }
                        }

                        Rectangle {
                            width: Math.max(56, deleteLbl.width + 20)
                            height: 26
                            radius: Theme.radiusSm
                            color: deleteArea.containsMouse ? Qt.alpha(Theme.danger, 0.16) : "transparent"
                            border.color: Theme.border
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: deleteLbl
                                anchors.centerIn: parent
                                text: qsTr("Delete")
                                color: Theme.dangerLight
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: deleteArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backupsBrowser.pendingDelete = backupItem.modelData
                            }
                        }
                    }
                }
            }
        }
    }

    // A settings/overlays/templates restore REPLACES the live file, so it is
    // confirmed first. Driven off pendingRestore, same contract as the delete
    // dialog below.
    ConfirmDialog {
        id: restoreCategoryConfirm
        shown: backupsBrowser.pendingRestore !== null
        title: qsTr("Put this backup back?")
        message: backupsBrowser.pendingRestore
                 ? qsTr("Your current %1 will be replaced by the copy from %2. This cannot be undone.")
                       .arg(root.backupCategoryLabel(backupsBrowser.pendingRestore.category))
                       .arg(root.backupStampText(backupsBrowser.pendingRestore.stamp))
                 : ""
        confirmLabel: qsTr("Restore")
        cancelLabel: qsTr("Cancel")
        confirmVariant: "danger"
        onConfirmed: {
            const row = backupsBrowser.pendingRestore
            backupsBrowser.pendingRestore = null
            backupsBrowser.restoreCategory(row)
        }
        onDismissed: backupsBrowser.pendingRestore = null
    }

    // Delete is destructive — confirm before removing the copy. Driven off
    // pendingDelete so the browser closes the dialog by clearing it.
    ConfirmDialog {
        id: deleteBackupConfirm
        shown: backupsBrowser.pendingDelete !== null
        title: qsTr("Delete this backup?")
        message: backupsBrowser.pendingDelete
                 ? qsTr("The saved copy of \"%1\" from %2 will be removed. This cannot be undone.")
                       .arg(backupsBrowser.pendingDelete.show)
                       .arg(root.backupStampText(backupsBrowser.pendingDelete.stamp))
                 : ""
        confirmLabel: qsTr("Delete")
        cancelLabel: qsTr("Cancel")
        confirmVariant: "danger"
        onConfirmed: {
            const row = backupsBrowser.pendingDelete
            backupsBrowser.pendingDelete = null
            backupsBrowser.remove(row)
        }
        onDismissed: backupsBrowser.pendingDelete = null
    }
}
