import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root

    // Asks the settings shell to show another section ("smart" = Smart Config).
    signal sectionRequested(string key)

    readonly property var values: SettingsService.values
    readonly property var definitions: SettingsService.definitions

    // The label of one of the resource profiles (by its engine value).
    function profileLabel(value) {
        const choices = root.definitions["resources.profile"].choices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === value)
                return choices[i].label
        return ""
    }
    function flip(key) { SettingsService.setValue(key, !root.values[key]) }

    function hardwareRenderingNote() {
        if (root.values["preferences.hardwareAcceleration"] !== true)
            return qsTr("Frames are drawn in software — it works on every machine, GPU or not.")
        if (SettingsService.rendererOnGpu)
            return qsTr("Frames are drawn on the graphics card.")
        if (SettingsService.rendererInUse === "")
            return qsTr("Asking the engine for the graphics card…")
        return qsTr("No usable GPU was found — frames are drawn in software.")
    }

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
    function backupCategoryLabel(category) {
        return category === "settings" ? qsTr("Settings")
             : category === "overlays" ? qsTr("Overlays")
             : category === "templates" ? qsTr("Templates")
             : String(category)
    }

    // ---- Backups: the answers all arrive asynchronously ----------------------
    property string restoreTarget: ""     // a show copy in flight (restoreFinished reports by target)
    property string restoreName: ""       // what that copy was named
    property string deletedBackupShow: "" // the row a Delete is waiting to answer for
    // A "Back up now" run asks TWICE (the open show, then the selected
    // categories), each answering with its own count: the notice waits for both.
    property int backupRunAsked: 0
    property int backupRunMade: 0
    function backupNow() {
        root.backupRunAsked = 0
        root.backupRunMade = 0
        if (ShowService.showPath !== "") {
            root.backupRunAsked++
            SettingsService.backupShow(ShowService.showPath)
        }
        root.backupRunAsked++
        SettingsService.backupCategories()
    }

    // ---- What a crash on THIS machine would leave behind ----------------------
    function crashToolingLabel() {
        const s = EngineBridge.crashToolingState
        if (s === "incomplete")
            return qsTr("Incomplete")
        if (s === "ready")
            return qsTr("Ready")
        if (s === "skipped")
            return qsTr("Not checked")
        return qsTr("Checking…")
    }
    function crashToolingDetail() {
        const s = EngineBridge.crashToolingState
        if (s === "incomplete")
            return EngineBridge.crashToolingProblem
        if (s === "ready")
            return qsTr("A crash or hang from this run can be captured and read.")
        if (s === "skipped")
            return qsTr("This launch skipped the check, so nothing is known about the tools that write a crash down.")
        return qsTr("Checking that a crash or hang can be captured and read…")
    }
    // Is the verdict a fault (draws in the warning ink, not the muted one)?
    readonly property bool crashToolingBroken: EngineBridge.crashToolingState === "incomplete"

    Connections {
        target: SettingsService
        // How many NEW copies the last run made (one answer per run: the open
        // show's, then the selected categories').
        function onBackupMade(made) {
            if (root.backupRunAsked === 0)
                return   // an AUTOMATIC run's answer: not this screen's notice
            root.backupRunMade += made
            root.backupRunAsked--
            if (root.backupRunAsked > 0)
                return
            const total = root.backupRunMade
            root.backupRunMade = 0
            EventBus.notify(total > 0 ? qsTr("Made %n new saved copy(s).", "", total)
                                      : qsTr("Everything selected is already saved."),
                            "info", qsTr("Backups"), "settings.backups.now")
        }
        function onAllBackupsListed(rows) { backupsBrowser.rows = rows }
        // The set of backups changed: the browser refills, and a Delete this
        // screen asked for says so now that the list has come back shorter.
        function onBackupsChanged() {
            backupsBrowser.reload()
            if (root.deletedBackupShow !== "") {
                const show = root.deletedBackupShow
                root.deletedBackupShow = ""
                EventBus.notify(qsTr("Deleted a backup of \"%1\".").arg(show),
                                "info", qsTr("Backups"), "settings.backups.deleted")
            }
        }
        // A restore finished. A category restore reports its own name; a show
        // copy reports the path it was written to.
        function onRestoreFinished(ok, what) {
            const category = what === "settings" || what === "overlays" || what === "templates"
            if (category) {
                if (!ok) {
                    EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                    return
                }
                backupsBrowser.reload()
                EventBus.notify(qsTr("%1 was put back from its backup.").arg(root.backupCategoryLabel(what)),
                                "info", qsTr("Backups"), "settings.backups.restored")
                return
            }
            if (what === "" || what !== root.restoreTarget)
                return
            const name = root.restoreName
            root.restoreTarget = ""
            root.restoreName = ""
            if (!ok) {
                EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                return
            }
            ShowService.refreshLibrary()
            backupsBrowser.reload()
            EventBus.notify(qsTr("Restored \"%1\" as a new show.").arg(name), "info", qsTr("Backups"), "settings.backups.restored")
        }
    }

    Flickable {
        id: flick
        objectName: "selfTestGeneralFlick"
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.y + layout.height + Theme.space6   // the column's own top offset counts, or the last card sits on the window edge
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
            }

            // ---- Smart Config banner ----
            Rectangle {
                width: parent.width
                height: 92
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    x: 20
                    // Centred in the card: the badge and the headline sit as one block, with
                    // the same air above and below instead of a tall empty strip under it.
                    anchors.verticalCenter: parent.verticalCenter
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
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: 116
                    height: 36
                    radius: Theme.radiusMd
                    color: reviewArea.containsMouse ? Theme.accentLight : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Review setup")
                        color: Theme.controlInk
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
                // BOTH CARDS THE SAME HEIGHT: the taller one decides, so the pair ends on one line
                // instead of a short card hanging under a tall neighbour.
                readonly property real pairHeight: Math.max(appearanceCol.height, startupCol.height) + 40

                Rectangle {
                    objectName: "selfTestCardAppearance"   // inset self-test grab target
                    width: (parent.width - 16) / 2
                    height: parent.pairHeight
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

                        // A setting label on the left and its choices on the right.
                        component ChoiceRow: Item {
                            id: choiceRow
                            property string settingKey: ""
                            width: parent.width
                            height: Math.max(36, choiceCol.height + 8)

                            Column {
                                id: choiceCol
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 140
                                spacing: 2

                                Text {
                                    text: root.definitions[choiceRow.settingKey] ? root.definitions[choiceRow.settingKey].label : ""
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }
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
                        NumberField {
                            width: parent.width
                            label: qsTr("Text size (%)")
                            compact: true
                            value: Number(root.values["appearance.textScale"] ?? 120)
                            step: 5
                            decimals: 0
                            minValue: 80
                            maxValue: 160
                            showSlider: true
                            onMoved: (v) => SettingsService.setValue("appearance.textScale", Math.round(v))
                        }
                        Rectangle { x: 2; width: parent.width - 2; height: 1; color: Theme.border }

                        // Lock In Mode — a toggle row.
                        Item {
                            width: appearanceCol.width
                            height: 38

                            Column {
                                id: lockCol
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 80
                                spacing: 0
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
                    height: parent.pairHeight
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
                                font.pixelSize: Theme.textSub
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
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
            // Resource profile selector and live usage meters.
            Rectangle {
                id: profileStrip
                width: parent.width
                height: profileCol.height + 40
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

                Column {
                    id: profileCol
                    x: 20
                    y: 20
                    width: parent.width - 40
                    spacing: 0

                    Item {
                        width: parent.width
                        height: 34

                        Text {
                            anchors.left: parent.left
                            anchors.right: profileActions.left
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.definitions["resources.profile"].label
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Row {
                            id: profileActions
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 18

                            SegmentedControl {
                                anchors.verticalCenter: parent.verticalCenter
                                options: root.definitions["resources.profile"].choices
                                             .map((c) => ({ key: c.value, label: c.label }))
                                current: String(root.values["resources.profile"])
                                marked: SettingsService.recommendedProfile
                                onPicked: (key) => SettingsService.setValue("resources.profile", key)
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Manage →")
                                color: manageArea.containsMouse ? Theme.accent : Theme.accentLight
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm

                                MouseArea {
                                    id: manageArea
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.sectionRequested("smart")
                                }
                            }
                        }
                    }

                    Row {
                        width: parent.width
                        topPadding: 16
                        spacing: 12

                        StatTile { label: qsTr("Rendering"); pct: TelemetryService.utilization.rendering; width: (parent.width - 24) / 3 }
                        StatTile { label: qsTr("Encoding"); pct: TelemetryService.utilization.encoding; width: (parent.width - 24) / 3 }
                        StatTile { label: qsTr("Output"); pct: TelemetryService.utilization.output; width: (parent.width - 24) / 3 }
                    }

                    LiveCurve {
                        width: parent.width
                        topGap: 16
                    }
                }
            }

            // ---- Scrolled-state sections (the _Scrolled reference) ----

            component SettingsSection: Rectangle {
                id: section
                property string title: ""
                default property alias rows: sectionCol.data
                width: layout.width
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
                // The engine's key supplies the setting label.
                property string key: ""
                property string choiceKey: ""
                property string label: root.definitions[prefRow.key !== "" ? prefRow.key : prefRow.choiceKey]
                                       ? root.definitions[prefRow.key !== "" ? prefRow.key : prefRow.choiceKey].label : ""
                property string sub: ""
                property bool last: false
                width: parent.width

                Item {
                    width: prefRow.width
                    height: 40   // one line: the grey sub-lines under a setting are gone

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        Text {
                            objectName: prefRow.key !== "" ? "selfTestLabel_" + prefRow.key : ""
                            text: prefRow.label
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            objectName: prefRow.key !== "" ? "selfTestNote_" + prefRow.key : ""
                            visible: false   // no sub-line under a setting (nobody read them)
                            text: prefRow.sub
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSub
                        }
                    }

                    SettingsToggle {
                        visible: prefRow.key !== ""
                        objectName: prefRow.key !== "" ? "selfTestToggle_" + prefRow.key : ""
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

            component StatusRow: Column {
                id: statusRow
                property string label: ""
                property string detail: ""
                property string status: ""
                property bool fault: false
                property bool last: false
                width: parent.width

                Item {
                    width: statusRow.width
                    // The explanation only appears when something is actually wrong; a healthy row is
                    // one line like every other.
                    height: Math.max(44, statusRow.fault ? detailText.implicitHeight + 24 : 0)

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(80, parent.width - pill.width - 16)
                        spacing: 2

                        Text {
                            text: statusRow.label
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            id: detailText
                            objectName: "selfTestCrashToolingDetail"
                            visible: statusRow.fault && statusRow.detail !== ""
                            width: parent.width
                            text: statusRow.detail
                            color: statusRow.fault ? Theme.warningLight : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSub
                            wrapMode: Text.WordWrap
                        }
                    }

                    Pill {
                        id: pill
                        objectName: "selfTestCrashToolingPill"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: statusRow.status
                        tint: statusRow.fault
                        baseColor: Theme.warning
                        lightColor: Theme.warningLight
                        fontSize: Theme.textSub
                    }
                }
                Rectangle {
                    visible: !statusRow.last
                    x: 2
                    width: statusRow.width - 2
                    height: 1
                    color: Theme.border
                }
            }

            component BackupRow: Column {
                id: backupRow
                property bool last: false
                width: parent.width

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
                    backupRow.backups = []
                    if (backupRow.fileStem !== "")
                        SettingsService.requestBackups(backupRow.fileStem)
                }
                Component.onCompleted: refresh()
                Connections {
                    target: ShowService
                    function onShowChanged() { backupRow.refresh() }
                }
                Connections {
                    target: SettingsService
                    function onBackupsListed(showName, rows) {
                        if (showName === backupRow.fileStem)
                            backupRow.backups = rows
                    }
                    // A copy was made, deleted or restored: this row's own list
                    // is one of the things that changed.
                    function onBackupsChanged() { backupRow.refresh() }
                }

                // Stamp/size formatting lives on the root now (backupStampText /
                // backupSizeText) — the all-shows browser formats the same way.

                function restore(backupPath) {
                    const name = qsTr("%1 (backup)").arg(backupRow.title)
                    const target = ShowService.newLibraryShowPath("", name)
                    if (target === "") {
                        EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                        return
                    }
                    root.restoreTarget = target
                    root.restoreName = name
                    SettingsService.restoreBackup(backupPath, target)
                }

                Item {
                    width: backupRow.width
                    height: 44

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
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                PrefRow { key: "preferences.closeToTray" }
                PrefRow { key: "preferences.singleClickLive" }
                PrefRow { key: "preferences.hardwareAcceleration"
                          sub: root.hardwareRenderingNote(); last: true }
            }

            SettingsSection {
                objectName: "selfTestCardBackups"   // inset self-test grab target
                title: qsTr("Backups & recovery")
                PrefRow { key: "backups.automatic" }
                PrefRow { choiceKey: "backups.intervalMinutes" }
                PrefRow { choiceKey: "backups.keepLast" }
                PrefRow { key: "backups.crashRecovery" }
                PrefRow { key: "backups.includeSettings" }
                PrefRow { key: "backups.includeOverlays" }
                PrefRow { key: "backups.includeTemplates" }
                BackupRow { last: true }
            }

            SettingsSection {
                objectName: "selfTestCardNotifications"   // inset self-test grab target
                title: qsTr("Notifications & logs")
                PrefRow { key: "notifications.show" }
                PrefRow { choiceKey: "notifications.logLevel"; last: true }
            }

            SettingsSection {
                objectName: "selfTestCardStability"   // inset self-test grab target
                title: qsTr("Crash reports")
                StatusRow {
                    label: qsTr("Crash and hang capture")
                    status: root.crashToolingLabel()
                    detail: root.crashToolingDetail()
                    fault: root.crashToolingBroken
                    last: true
                }
            }

            component ActionRow: Column {
                id: actionRow
                property string label: ""
                property string buttonText: ""
                property bool last: false
                signal activated()
                width: parent.width

                Item {
                    width: actionRow.width
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
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                Rectangle {
                    visible: !actionRow.last
                    x: 2
                    width: actionRow.width - 2
                    height: 1
                    color: Theme.border
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

            SettingsSection {
                title: qsTr("All backups")
                ActionRow {
                    label: qsTr("Browse every saved copy")
                    buttonText: qsTr("Browse…")
                    onActivated: backupsBrowser.open()
                }
                ActionRow {
                    label: qsTr("Open the backups folder")
                    buttonText: qsTr("Open")
                    onActivated: {
                        if (SettingsService.backupDirectory() === "")
                            EventBus.notify(qsTr("The backups folder is not ready yet."), "warning", qsTr("Backups"), "settings.backups.openFolder")
                        else
                            SettingsService.openBackupDirectory()
                    }
                }
                // Takes the same copies the automatic timer would, on demand:
                // the open show (if any) plus whichever categories are on.
                ActionRow {
                    label: qsTr("Back up the open show and everything selected")
                    buttonText: qsTr("Back up now")
                    onActivated: root.backupNow()
                    last: true
                }
            }

            SettingsSection {
                objectName: "selfTestCardLibraries"   // inset self-test grab target
                title: qsTr("Libraries")
                RestoreRow { label: qsTr("Restore default overlays"); service: OverlayLibraryService }
                RestoreRow { label: qsTr("Restore default templates"); service: TemplateLibraryService; last: true }
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
                    last: true
                }
            }

            // Breathing room below the last card — without it, "Reset" sat
            // flush against the window's own bottom edge.
            Item { width: 1; height: Theme.space4 }
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
    ModalCard {
        id: backupsBrowser
        title: qsTr("Backups")
        cardWidth: 620
        showSave: false
        cancelText: qsTr("Close")
        onCancelled: backupsBrowser.close()

        property var rows: []
        // The row a Delete is waiting to confirm (drives the dialog below).
        property var pendingDelete: null
        // The settings/overlays/templates row a Restore is waiting to confirm.
        property var pendingRestore: null

        // Rows are listed on a worker and arrive on allBackupsListed (the
        // screen's Connections below), so this only asks.
        function reload() { SettingsService.requestAllBackups() }
        onShownChanged: if (backupsBrowser.shown) backupsBrowser.reload()

        function restore(row) {
            if (!row)
                return
            if (row.category !== "show") {
                backupsBrowser.pendingRestore = row
                return
            }
            const name = qsTr("%1 (backup)").arg(row.show)
            const target = ShowService.newLibraryShowPath("", name)
            if (target === "") {
                EventBus.notify(qsTr("That backup could not be restored."), "error", qsTr("Backups"), "settings.backups.restore")
                return
            }
            root.restoreTarget = target
            root.restoreName = name
            SettingsService.restoreBackup(row.path, target)
        }

        // Puts a settings/overlays/templates copy back in place (the engine also
        // reloads whatever reads it). Only reached after the confirm dialog.
        function restoreCategory(row) {
            if (!row)
                return
            // The copy and the reload run on a worker; restoreFinished answers
            // by category name (root's onRestoreFinished).
            SettingsService.restoreCategoryBackup(row.path)
        }

        // Reveals THIS copy in the OS file manager (Explorer/Finder select it;
        // a desktop without that opens the folder).
        function showInFolder(row) {
            if (!row)
                return
            SettingsService.showBackupInFolder(row.path)
        }

        function remove(row) {
            if (!row)
                return
            root.deletedBackupShow = String(row.show)
            SettingsService.deleteBackup(row.path)
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                visible: backupsBrowser.rows.length === 0
                width: parent.width
                text: qsTr("No backups yet.")
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
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                            font.pixelSize: Theme.textSub
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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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