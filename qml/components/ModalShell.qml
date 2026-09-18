import QtQuick
import VGRPresenterUI

// The reusable Settings "card": titlebar + nav rail + content area + footer.
// Drop content into it as normal children; they lay out inside contentArea.
Rectangle {
    id: root

    default property alias contentData: contentArea.data

    property var navSections: null
    property string currentKey: "general"
    property bool showFooter: true
    property bool showSave: true

    // The search palette's index (see TitleBar.qml): one entry per
    // searchable setting across the app's settings sections. Defaults
    // cover the built sections' key rows plus every rail section itself;
    // a consumer can override with its own index.
    property var searchIndex: [
        { label: "General", section: "Settings", key: "general" },
        { label: "Smart Config", section: "Settings", key: "smart" },
        { label: "Outputs", section: "Settings", key: "outputs" },
        { label: "Styles", section: "Settings", key: "styles" },
        { label: "Audio & Video", section: "Settings", key: "av" },
        { label: "Recording", section: "Settings", key: "recording" },
        { label: "Plugins", section: "Settings", key: "plugins" },
        { label: "Resource profile", section: "General", key: "general" },
        { label: "Appearance", section: "General", key: "general" },
        { label: "Accent color", section: "General", key: "general" },
        { label: "Lock In Mode", section: "General", key: "general" },
        { label: "Autosave", section: "General", key: "general" },
        { label: "Backups & recovery", section: "General", key: "general" },
        { label: "Crash recovery", section: "General", key: "general" },
        { label: "Notifications & logs", section: "General", key: "general" },
        { label: "Configuration mode", section: "Smart Config", key: "smart" },
        { label: "Hardware detected", section: "Smart Config", key: "smart" },
        { label: "Resource budgets", section: "Smart Config", key: "smart" },
        { label: "Stream platform", section: "Recording", key: "recording" },
        { label: "Stream key", section: "Recording", key: "recording" },
        { label: "Video bitrate", section: "Recording", key: "recording" },
        { label: "Encoder", section: "Recording", key: "recording" },
        { label: "Screens to record", section: "Recording", key: "recording" },
        { label: "Recording & Streaming", section: "Recording", key: "recording" },
        { label: "Installed plugins", section: "Plugins", key: "plugins" },
        { label: "Browse plugin store", section: "Plugins", key: "plugins" }
    ]

    signal sectionSelected(string key)
    signal closeRequested()
    signal cancelRequested()
    signal saveRequested()

    implicitWidth: 1080
    implicitHeight: 640
    radius: Theme.radiusXl
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    clip: true

    layer.enabled: true

    // Swallows clicks anywhere on the card so a scrim behind this shell
    // (see Main.qml) only closes on a genuine outside-click.
    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    Column {
        anchors.fill: parent

        TitleBar {
            id: titleBar
            width: parent.width
            // Raised above the rail/content Row below so the search
            // suggestions dropdown paints over them, not under.
            z: 10
            searchIndex: root.searchIndex
            onSectionRequested: (key) => root.sectionSelected(key)
            onCloseRequested: root.closeRequested()
        }

        Row {
            width: parent.width
            height: parent.height - titleBar.height - (root.showFooter ? footerBar.height : 0)

            NavRail {
                id: navRail
                height: parent.height
                currentKey: root.currentKey
                onSectionSelected: (key) => root.sectionSelected(key)

                Component.onCompleted: {
                    if (root.navSections !== null)
                        navRail.sections = root.navSections
                }
            }

            Item {
                id: contentArea
                width: parent.width - navRail.width
                height: parent.height
                clip: true
            }
        }

        FooterBar {
            id: footerBar
            width: parent.width
            visible: root.showFooter
            showSave: root.showSave
            onCancelRequested: root.cancelRequested()
            onSaveRequested: root.saveRequested()
        }
    }
}
