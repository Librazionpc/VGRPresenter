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

    // NO ITEM LAYER. One used to be enabled here for exactly one reason: Qt's
    // `clip` is RECTANGULAR, and a layered Rectangle is composited through its
    // own rounded material, so the layer was what made the card's corners clip
    // round instead of square. A layer is a GPU-scene-graph feature — on a
    // machine running Qt Quick's SOFTWARE backend (no GPU driver) the nodes are
    // unsupported and the card's corners came back square, or the card did not
    // compose at all, for a purely cosmetic clip. The fix costs no texture: the
    // only two strips that reach a card corner — the title bar at the top and the
    // footer at the bottom — round THEIR own corners to the card's radius, and
    // everything between them (the nav rail's straight edge, the content area)
    // never enters a corner arc in the first place. Same picture on every
    // backend, and one less full-card texture per frame on the GPU one.

    // Swallows clicks anywhere on the card so the shared scrim cannot pass
    // them through to the page behind this shell.
    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    Column {
        anchors.fill: parent

        TitleBar {
            id: titleBar
            width: parent.width
            // The card's own radius, per corner: an opaque strip that reaches the
            // card's edge would otherwise square off the top of it now that no
            // layer masks the whole card (see root's comment).
            topLeftRadius: Theme.radiusXl
            topRightRadius: Theme.radiusXl
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
            // ...and the same at the bottom edge (see titleBar's comment).
            bottomLeftRadius: Theme.radiusXl
            bottomRightRadius: Theme.radiusXl
            visible: root.showFooter
            showSave: root.showSave
            onCancelRequested: root.cancelRequested()
            onSaveRequested: root.saveRequested()
        }
    }
}
