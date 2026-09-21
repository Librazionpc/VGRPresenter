import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The Media library tab — a sidebar (All / Inputs / Playlists) and a content
// area. Under Inputs (and All), Video sources / Audio inputs / Buses are a
// HORIZONTAL tab row across the top of the content, like the reference's
// Cameras / Screens / NDI / Blackmagic row, with the selected roster's cards
// below it. The CONTENT IS LIVE: Buses, Video Sources and Audio Inputs are the
// exact same rosters Settings · Audio & Video creates (BusListModel /
// VideoSourceListModel / AudioInputListModel singletons), so anything made
// in Settings shows up here and vice versa. Playlists is the session's
// show/playlist list — fed from the shows library.
//
// Sidebar rows carry live counts like the reference (All 1927).
Item {
    id: root

    // Summary of the session's playlists (shows) — bound by the host from
    // SlideListModel so this pane stays model-free on that front.
    property var playlists: []   // [{ name, modified }]

    // Reactivity bridge (same pattern as AudioVideoScreen): plain
    // Q_INVOKABLE rowCount()/get*() reads aren't tracked by QML's binding
    // system, so this counter is the honest dependency — bumped by all
    // three models, referenced by the count/section bindings below.
    property int modelsRev: 0
    Connections {
        target: VideoSourceListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: BusListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    property int selection: 0    // 0=All 1=Inputs 2=Playlists
    property int inputTab: 0     // 0=Video sources 1=Audio inputs 2=Buses
    readonly property var filters: [
        { label: qsTr("All"),       icon: "layoutDashboard" },
        { label: qsTr("Inputs"),    icon: "camera" },
        { label: qsTr("Playlists"), icon: "playCircle" }
    ]
    // The horizontal tab row.
    readonly property var inputTabs: [
        { label: qsTr("Video sources"), icon: "camera",  kind: "video" },
        { label: qsTr("Audio inputs"),  icon: "mic",     kind: "audio" },
        { label: qsTr("Buses"),         icon: "volume2", kind: "bus" }
    ]
    readonly property var inputCounts: {
        const _ = root.modelsRev   // reactivity dependency
        return [VideoSourceListModel.rowCount(), AudioInputListModel.rowCount(), BusListModel.rowCount()]
    }
    readonly property int inputTotal: inputCounts[0] + inputCounts[1] + inputCounts[2]
    readonly property var counts: [inputTotal + playlists.length, inputTotal, playlists.length]
    readonly property bool showInputs: selection !== 2
    readonly property bool showPlaylists: selection !== 1
    readonly property int tabRowHeight: 44

    // ---- Sidebar ----------------------------------------------------------
    Rectangle {
        id: sidebar
        x: 0; y: 0
        width: 170; height: parent.height
        color: "#0f1015"

        Column {
            x: 8; y: 8
            width: parent.width - 16
            spacing: 2

            Repeater {
                model: root.filters.length
                delegate: Rectangle {
                    required property int index
                    width: parent.width
                    height: 30
                    radius: 6
                    color: root.selection === index ? "#1e1f28" : (filterMouse.containsMouse ? "#16171e" : "transparent")
                    Rectangle {
                        visible: root.selection === index
                        x: 0; y: 5; width: 3; height: 20
                        color: Theme.danger; radius: 1.5
                    }
                    IconGlyph {
                        name: root.filters[index].icon
                        color: root.selection === index ? Theme.danger : Theme.textSecondary
                        x: 12; y: 8; width: 14; height: 14
                    }
                    Text {
                        x: 34; y: 8; width: parent.width - 74
                        text: root.filters[index].label
                        color: root.selection === index ? Theme.textPrimary : Theme.textSecondary
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.counts[index]
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 11
                    }
                    MouseArea {
                        id: filterMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selection = index
                    }
                }
            }
        }
    }
    Rectangle { x: 170; y: 0; width: 2; height: parent.height; color: Theme.border }

    // ---- Horizontal input tabs: Video sources | Audio inputs | Buses ------------
    Item {
        id: tabRow
        x: 172; y: 0
        width: parent.width - 172
        height: root.tabRowHeight
        visible: root.showInputs

        Row {
            anchors.fill: parent

            Repeater {
                model: root.inputTabs.length
                delegate: Item {
                    id: tabItem
                    required property int index
                    readonly property bool selected: root.inputTab === index
                    width: tabRow.width / root.inputTabs.length
                    height: tabRow.height

                    Rectangle {
                        anchors.fill: parent
                        color: !tabItem.selected && tabMouse.containsMouse ? "#14151c" : "transparent"
                    }
                    Row {
                        anchors.centerIn: parent
                        spacing: 9
                        IconGlyph {
                            anchors.verticalCenter: parent.verticalCenter
                            name: root.inputTabs[tabItem.index].icon
                            color: tabItem.selected ? Theme.textPrimary : Theme.textSecondary
                            width: 16; height: 16
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.inputTabs[tabItem.index].label
                            color: tabItem.selected ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontFamily; font.pixelSize: 13
                            font.weight: tabItem.selected ? Font.DemiBold : Font.Medium
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.inputCounts[tabItem.index]
                            color: Theme.textMuted
                            font.family: Theme.fontFamily; font.pixelSize: 11
                        }
                    }
                    // Selected underline (full tab width, like the reference).
                    Rectangle {
                        visible: tabItem.selected
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 2
                        color: Theme.danger
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.inputTab = tabItem.index
                    }
                }
            }
        }
        // Hairline under the whole row.
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    // One roster's cards, three to a row.
    component CardGrid: Flow {
        id: grid
        property string kind: "video"
        property int count: 0
        width: parent.width
        spacing: 8
        Repeater {
            model: grid.count
            delegate: Rectangle {
                required property int index
                width: (grid.width - 16) / 3
                height: 44
                radius: 6
                color: cardMouse.containsMouse ? "#1e1f28" : "#16171e"
                // Kind chip — small colored square with the roster's icon.
                Rectangle {
                    x: 8; y: 10
                    width: 24; height: 24
                    radius: 5
                    color: grid.kind === "video" ? "#20304a"
                         : grid.kind === "audio" ? "#173326"
                         : grid.kind === "bus"   ? "#3a1e18"
                         : "#2a2438"
                    IconGlyph {
                        anchors.centerIn: parent
                        name: grid.kind === "video" ? "camera"
                            : grid.kind === "audio" ? "mic"
                            : grid.kind === "bus"   ? "volume2"
                            : "playCircle"
                        color: grid.kind === "video" ? "#7fb3ff"
                             : grid.kind === "audio" ? "#6fe0a0"
                             : grid.kind === "bus"   ? "#ff8d7f"
                             : "#c3a7ff"
                        width: 13; height: 13
                    }
                }
                Column {
                    x: 40; y: 6
                    width: parent.width - 52
                    spacing: 1
                    Text {
                        width: parent.width
                        text: root.cardName(index, grid.kind)
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                    Text {
                        width: parent.width
                        text: root.cardSub(index, grid.kind)
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 10
                    }
                }
                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
    }

    // ---- Content ------------------------------------------------------------
    Flickable {
        x: 172
        y: root.showInputs ? root.tabRowHeight : 0
        width: parent.width - 172
        height: parent.height - y
        clip: true
        contentHeight: mediaGrid.height + 16

        Column {
            id: mediaGrid
            x: 0; y: 10
            width: parent.width - 16
            spacing: 14

            // The selected input roster.
            Column {
                visible: root.showInputs
                width: parent.width
                spacing: 8

                CardGrid {
                    kind: root.inputTabs[root.inputTab].kind
                    count: root.inputCounts[root.inputTab]
                }
                Text {
                    visible: root.inputCounts[root.inputTab] === 0
                    width: parent.width
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing here yet — add %1 in Settings · Audio & Video.")
                              .arg(root.inputTabs[root.inputTab].label.toLowerCase())
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }

            // Playlists.
            Column {
                visible: root.showPlaylists
                width: parent.width
                spacing: 6
                Text {
                    x: 2
                    text: qsTr("Playlists") + "  ·  " + root.playlists.length
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 11; font.bold: true
                }
                CardGrid {
                    kind: "playlist"
                    count: root.playlists.length
                }
            }
        }
    }

    // Card label helpers — one lookup per roster so the delegates stay dumb.
    function cardName(i, kind) {
        if (kind === "video") {
            const v = VideoSourceListModel.getSource(i); return v.name !== undefined ? v.name : ""
        }
        if (kind === "audio") {
            const a = AudioInputListModel.getInput(i); return a.name !== undefined ? a.name : ""
        }
        if (kind === "bus") {
            const b = BusListModel.getBus(i); return b.name !== undefined ? b.name : ""
        }
        return root.playlists[i] !== undefined ? root.playlists[i].name : ""
    }
    function cardSub(i, kind) {
        if (kind === "video") {
            const v = VideoSourceListModel.getSource(i)
            return (v.kind !== undefined ? v.kind : "") + (v.sublabel !== undefined && v.sublabel !== "" ? " · " + v.sublabel : "")
        }
        if (kind === "audio") {
            const a = AudioInputListModel.getInput(i)
            return (a.kind !== undefined ? a.kind : "") + (a.sublabel !== undefined && a.sublabel !== "" ? " · " + a.sublabel : "")
        }
        if (kind === "bus") {
            const b = BusListModel.getBus(i)
            return (b.type !== undefined ? b.type : "") + (b.muted !== undefined && b.muted ? " · muted" : "")
        }
        return root.playlists[i] !== undefined ? root.playlists[i].modified : ""
    }
}
