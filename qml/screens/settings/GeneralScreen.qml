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
// Rooted in an Item (not a bare Flickable) so the scrollbar can sit fixed
// at this screen's right edge while the content beneath it scrolls — a
// scrollbar declared inside a Flickable would scroll away with the
// content. ModalShell drops this whole Item into its content area.
//
// Theme tokens throughout: this file sits shallow enough in the tree for
// the AOT compiler to resolve the singleton (see the depth rule notes in
// DropdownPanel.qml).
Item {
    id: root

    // Settings state — live and editable for now (a persistence layer is
    // future work; Cancel/Save semantics land with it).
    property bool lockInMode: false
    property int accentIndex: 3
    readonly property var accentColors: [Theme.danger, Theme.info, Theme.success, Theme.accent]
    property string resourceProfile: "Performance"
    // Row-level toggles, one flat map so the rows stay dumb.
    property var toggles: ({
        openLastProject: false, launchAtLogin: false, autosave: false,
        startMinimized: false, restoreLastSession: true, closeToTray: true,
        automaticBackups: true, crashRecovery: true, showNotifications: true
    })

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
                        color: "#266C5CE7"

                        Text {
                            id: smartLabel
                            anchors.centerIn: parent
                            text: qsTr("SMART CONFIG")
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }

                    Text {
                        text: qsTr("Recommended setup detected for this hardware")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: qsTr("GPU, encoder, audio and display capabilities analyzed — review and apply the recommended profile.")
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
                        // The Smart Config section (NavRail key "smart") is
                        // its own screen — future work.
                        onClicked: {}
                    }
                }
            }

            // ---- Appearance / Startup two-card row ----
            Row {
                width: parent.width
                spacing: 16

                // Appearance card — two value rows, one toggle row, the
                // accent swatches. Built explicitly (not a Repeater)
                // because each row's right-hand control differs.
                Rectangle {
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
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            bottomPadding: 10
                        }

                        component ValueRow: Item {
                            id: valueRow
                            property string label: ""
                            property string value: ""
                            width: appearanceCol.width
                            height: 36

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: valueRow.label
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: valueRow.value
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                        }

                        ValueRow { label: qsTr("Theme"); value: qsTr("Dark") }
                        Rectangle { width: parent.width; height: 1; color: Theme.border }
                        ValueRow { label: qsTr("Language"); value: qsTr("English (US)") }
                        Rectangle { width: parent.width; height: 1; color: Theme.border }

                        // Lock In Mode — a toggle row.
                        Item {
                            width: appearanceCol.width
                            height: 36

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Lock In Mode")
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                            SettingsToggle {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                checked: root.lockInMode
                                onToggled: root.lockInMode = !root.lockInMode
                            }
                        }
                        Rectangle { width: parent.width; height: 1; color: Theme.border }

                        // Accent color — the four swatches, selected one
                        // wrapped in an accent ring (with a 2px gap, like
                        // the reference).
                        Item {
                            width: appearanceCol.width
                            height: 38

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Accent color")
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8

                                Repeater {
                                    model: root.accentColors
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: 26
                                        height: 26
                                        radius: 13
                                        color: "transparent"
                                        border.width: root.accentIndex === index ? 2 : 0
                                        border.color: modelData

                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 18
                                            height: 18
                                            radius: 9
                                            color: parent.modelData
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.accentIndex = index
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Startup card — three toggles + the updates row.
                Rectangle {
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
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            bottomPadding: 10
                        }

                        Repeater {
                            model: [
                                { key: "openLastProject", label: qsTr("Open last project") },
                                { key: "launchAtLogin", label: qsTr("Launch at login") },
                                { key: "autosave", label: qsTr("Autosave") }
                            ]
                            delegate: Column {
                                id: startRow
                                required property var modelData
                                width: startupCol.width

                                Item {
                                    width: startRow.width
                                    height: 36

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: startRow.modelData.label
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                    SettingsToggle {
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        checked: root.toggles[startRow.modelData.key]
                                        onToggled: root.toggles[startRow.modelData.key] = !root.toggles[startRow.modelData.key]
                                    }
                                }
                                Rectangle { width: startRow.width; height: 1; color: Theme.border }
                            }
                        }

                        // Check for updates — value + button, not a toggle.
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
                                text: qsTr("v1.0.5 is current")
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
                                    font.pixelSize: 10
                                }

                                MouseArea {
                                    id: checkArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {}
                                }
                            }
                        }
                    }
                }
            }

            // ---- Resource profile strip ----
            Rectangle {
                width: parent.width
                height: 132
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
                    text: qsTr("Resource profile")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    x: 20
                    y: 38
                    text: qsTr("Runtime priority for rendering, encoding and outputs")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 12
                    text: qsTr("RECOMMENDED")
                    color: Theme.accentLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 8
                    font.weight: Font.Bold
                }

                Row {
                    id: manageRow
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 30
                    spacing: 16

                    Repeater {
                        model: [
                            { key: "Performance", label: qsTr("Performance") },
                            { key: "Balanced", label: qsTr("Balanced") },
                            { key: "Power Saver", label: qsTr("Power Saver") }
                        ]
                        delegate: Rectangle {
                            id: segRoot
                            required property var modelData
                            readonly property bool active: root.resourceProfile === segRoot.modelData.key
                            width: segLabel.width + 28
                            height: 30
                            radius: Theme.radiusMd
                            color: segRoot.active ? "#266C5CE7" : (segArea.containsMouse ? Theme.chip : Theme.inset)
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
                                onClicked: root.resourceProfile = segRoot.modelData.key
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
                        onClicked: {}
                    }
                }

                // The three usage meters.
                Row {
                    x: 20
                    y: 92
                    width: parent.width - 40
                    spacing: 24

                    // The shared meter component (see LabeledMeter.qml) —
                    // the same one Smart Config's Resource budgets use, so
                    // the two screens' meters can't drift apart.
                    LabeledMeter { label: qsTr("Rendering"); pct: 68; width: (parent.width - 48) / 3 }
                    LabeledMeter { label: qsTr("Encoding"); pct: 42; width: (parent.width - 48) / 3 }
                    LabeledMeter { label: qsTr("Output"); pct: 55; width: (parent.width - 48) / 3 }
                }
            }

            // ---- Scrolled-state sections (the _Scrolled reference) ----

            component SettingsSection: Rectangle {
                id: section
                property string title: ""
                default property alias rows: sectionCol.data
                width: layout.width
                height: sectionCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Text {
                    x: 20
                    y: 14
                    text: section.title
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Column {
                    id: sectionCol
                    x: 20
                    y: 44
                    width: parent.width - 40
                    spacing: 0
                }
            }

            component PrefRow: Column {
                id: prefRow
                property string label: ""
                property string sub: ""
                property bool toggle: false
                // Which entry of root.toggles this row flips — omitted for
                // value rows (they show valueText instead of a toggle).
                property string key: ""
                property string valueText: ""
                property bool chevron: false
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
                        visible: prefRow.toggle && prefRow.key !== ""
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: prefRow.key !== "" ? (root.toggles[prefRow.key] ?? false) : false
                        onToggled: {
                            if (prefRow.key !== "")
                                root.toggles[prefRow.key] = !root.toggles[prefRow.key]
                        }
                    }

                    Row {
                        visible: !prefRow.toggle && prefRow.valueText !== ""
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: prefRow.valueText
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            visible: prefRow.chevron
                            anchors.verticalCenter: parent.verticalCenter
                            text: "›"
                            color: Theme.textMuted
                            font.pixelSize: 12
                        }
                    }
                }
                Rectangle {
                    visible: !prefRow.last
                    width: prefRow.width
                    height: 1
                    color: Theme.border
                }
            }

            SettingsSection {
                title: qsTr("Preferences")
                PrefRow { label: qsTr("Start minimized"); toggle: true; key: "startMinimized" }
                PrefRow { label: qsTr("Restore last session"); toggle: true; key: "restoreLastSession" }
                PrefRow { label: qsTr("Close to tray"); toggle: true; key: "closeToTray"; last: true }
            }

            SettingsSection {
                title: qsTr("Backups & recovery")
                PrefRow { label: qsTr("Automatic backups"); sub: qsTr("Every 30 minutes"); toggle: true; key: "automaticBackups" }
                PrefRow { label: qsTr("Keep last"); valueText: qsTr("10 backups"); chevron: true }
                PrefRow { label: qsTr("Crash recovery"); sub: qsTr("Restores unsaved work after an unexpected exit"); toggle: true; key: "crashRecovery"; last: true }
            }

            SettingsSection {
                title: qsTr("Notifications & logs")
                PrefRow { label: qsTr("Show notifications"); toggle: true; key: "showNotifications" }
                PrefRow { label: qsTr("Log level"); valueText: qsTr("Info"); chevron: true; last: true }
            }

            // The design libraries (the dock's Overlays / Templates tabs): brings back what ships
            // after the user deleted it. The engine fills in what is missing; nothing else is touched.
            // Each row binds its library service directly - no string dispatch to read around.
            component RestoreRow: Item {
                id: restoreRow
                property string label: ""
                property var service: null          // OverlayLibraryService | TemplateLibraryService
                readonly property string noun: service === OverlayLibraryService ? qsTr("overlay") : qsTr("template")
                width: parent.width
                height: 40

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: restoreRow.label
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    height: 26
                    radius: Theme.radiusSm
                    color: restoreArea.containsMouse ? Theme.chip : Theme.inset
                    border.color: Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Restore")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }

                    MouseArea {
                        id: restoreArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const n = restoreRow.service.restoreDefaults()
                            EventBus.notify(n > 0 ? qsTr("Brought back %n default %1(s).", "", n).arg(restoreRow.noun)
                                                  : qsTr("All the default %1s are already here.").arg(restoreRow.noun),
                                            "info", qsTr("Settings"), "settings.libraries.restore")
                        }
                    }
                }
            }

            SettingsSection {
                title: qsTr("Libraries")
                RestoreRow { label: qsTr("Restore default overlays"); service: OverlayLibraryService }
                RestoreRow { label: qsTr("Restore default templates"); service: TemplateLibraryService }
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
}
