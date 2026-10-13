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

    signal sectionSelected(string key)
    signal closeRequested()
    signal cancelRequested()
    signal saveRequested()

    // The design size of the card.
    implicitWidth: 1080
    implicitHeight: 640
    // The card keeps its design size and does NOT scale itself: it lives inside a
    // layer the window already scales (Main.qml's uiScale — the whole app is drawn
    // at 1440x900 and scaled to the window), so applying a second scale here would
    // shrink the Settings card twice and break the proportion with the screens
    // behind it. Anything that needs to move with the window reads that one scale.
    width: implicitWidth
    height: implicitHeight
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
