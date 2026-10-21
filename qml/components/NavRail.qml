import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property var sections: [
        { key: "general",   label: "General",        icon: "sliders" },
        { key: "smart",     label: "Smart Config",    icon: "spark" },
        { key: "outputs",   label: "Outputs",         icon: "monitor" },
        { key: "styles",    label: "Styles",          icon: "layoutTemplate" },
        { key: "lookfeel",  label: "Appearance & Actions", icon: "sliders" },
        { key: "av",        label: "Audio & Video",   icon: "monitor" },
        { key: "recording", label: "Recording",       icon: "recordDot" },
        { key: "plugins",   label: "Plugins",         icon: "grid" }
    ]
    property string currentKey: "general"
    // The version is the RELEASE's: read from the C++ single source
    // (SettingsService.appVersion — SettingsService.cpp kAppVersion), so the
    // UI can never drift from the shipped build.
    property string appVersion: qsTr("v%1").arg(SettingsService.appVersion)

    signal sectionSelected(string key)

    implicitWidth: 206
    color: "transparent"

    Rectangle {
        // rail-edge divider
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.railDivider
    }

    Text {
        id: settingsLabel
        // The section label keeps its own small inset; the row list below
        // it goes edge-to-edge (see navPanel), so only this stays indented.
        x: 14
        y: 24
        text: "SETTINGS"
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        font.weight: Font.Medium
        color: Theme.textMuted
    }

    // The same striped-row surface Manage Actions' lists use — one ground,
    // one hairline, one pair of row colours — instead of a hand-rolled set
    // of pills with their own look. Full-bleed: x:0 and the FULL rail
    // width, square corners (rowRadius: 0) — rows run edge-to-edge instead
    // of sitting as inset pills with margin on both sides.
    StripedListPanel {
        id: navPanel
        x: 0
        y: settingsLabel.y + settingsLabel.height + Theme.space2
        width: parent.width
        rowHeight: 42
        rowRadius: 0
        padding: 0
        showPanel: false

        Repeater {
            model: root.sections
            delegate: Rectangle {
                id: navRow
                required property int index
                required property var modelData
                readonly property bool selected: modelData.key === root.currentKey
                width: parent.width
                height: navPanel.rowHeight
                radius: navPanel.rowRadius
                // The resting colour NEVER animates — HoverGlow below is
                // THE shared hover layer every surface in the app lights up
                // through (a fading overlay, not a cross-faded base colour;
                // see its own header comment for why animating color: here
                // directly strobes/feels off against the alternating stripe).
                color: navRow.selected ? Qt.alpha(Theme.accent, 0.12)
                                       : navPanel.rowColor(navRow.index, false)

                HoverGlow {
                    anchors.fill: parent
                    radius: parent.radius
                    hovered: navHover.hovered && !navRow.selected
                    glowColor: Theme.hoverBg
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 14
                    spacing: Theme.space3

                    Rectangle {
                        width: 28; height: 28
                        radius: Theme.radiusSm
                        anchors.verticalCenter: parent.verticalCenter
                        color: navRow.selected ? Qt.alpha(Theme.accent, 0.16) : Theme.navChipBg
                        NavIcon {
                            anchors.centerIn: parent
                            kind: navRow.modelData.icon
                            color: navRow.selected ? Theme.accentLight : Theme.iconMuted
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: navPanel.width - 14 - 28 - Theme.space3 - 14
                        text: navRow.modelData.label
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textMd
                        color: navRow.selected ? Theme.accentLight : Theme.navLabelMuted
                    }
                }

                HoverHandler { id: navHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.sectionSelected(navRow.modelData.key) }
            }
        }
    }

    Column {
        x: 20
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6
        spacing: 2

        Text {
            text: "Support"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            color: Theme.footerSupport
        }
        Text {
            text: root.appVersion
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
            color: Theme.footerVersion
        }
    }
}
