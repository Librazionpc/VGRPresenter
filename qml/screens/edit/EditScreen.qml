import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../main"

// The Edit screen: the slide canvas editor shown when the "Edit" tab is
// active. Faithfully matches VGRPresenter_Main_Screen_Edit.qml (the
// Figma-to-Qt ground truth), with the shared header chrome swapped for the
// AppMenuBar/ViewTabs/HeaderStatus components already used by the Show
// screen, and the repeated slide-list / output-monitor rows built from data
// (same content, less duplication) instead of copy-pasted blocks.
//
// Literal colors throughout, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml/DropdownPanel.qml at this nesting depth.
Rectangle {
    id: root

    height: 900
    width: 1440

    clip: true
    color: "#12131a"

    signal tabSelected(string tab)

    // Mock CRUD backend (src/SlideListModel.{h,cpp}) — stands in for the
    // real show/slide data source. Seeded with the same ground-truth
    // content the static array used to hold, but "Add slide" and selecting
    // a row now mutate real model state instead of pointing at fixed data.
    SlideListModel {
        id: slideModel
    }

    readonly property var outputs: [
        { name: "Main Output", badge: "LIVE 1", active: true },
        { name: "Stage Screen", badge: "STAGE 1", active: false },
        { name: "Nursery Display", badge: "NURSERY", active: false },
        { name: "Stream Overlay", badge: "OBS FEED", active: false }
    ]

    // ---- Header ----
    Rectangle {
        id: hdrBg
        height: 48
        width: 1440
        color: "#12131a"

        ViewTabs {
            x: 639.50
            y: 8
            activeTab: "edit"
            onTabSelected: (tab) => root.tabSelected(tab)
        }
        HeaderStatus {
            x: 1260
            y: 17
        }
    }

    // ---- Middle toolbar ----
    Rectangle {
        id: mHdr
        x: 280
        y: 48
        height: 48
        width: 760
        color: "#15161d"

        Rectangle {
            id: mBack
            x: 12
            y: 10
            height: 28
            width: 28
            color: "#1e1f29"
            radius: 7

            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -2
                color: "#eef0f6"
                font.family: "Inter"
                font.pixelSize: 13
                text: "‹"
            }
        }
        Text {
            x: 50
            y: 14
            color: "#eef0f6"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.Medium
            text: qsTr("Sunday Service")
        }
        Rectangle {
            x: 156
            y: 12
            height: 24
            width: 104
            border.color: "#3a2230"
            border.width: 1
            color: "#2a1c24"
            radius: 6

            Text {
                anchors.centerIn: parent
                color: "#ff4d3d"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Medium
                text: qsTr("Template · Worship")
            }
        }
        Text {
            x: 556
            y: 17
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Autosaved ✓")
        }
        Text {
            x: 638
            y: 17
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Fit")
        }
        Row {
            x: 662
            y: 10
            spacing: 4

            Rectangle {
                height: 28
                width: 28
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↺" }
            }
            Rectangle {
                height: 28
                width: 28
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↻" }
            }
            Rectangle {
                height: 28
                width: 34
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 10; text: qsTr("100%") }
            }
        }
    }
    Rectangle {
        x: 280
        y: 96
        height: 1
        width: 760
        color: "#232530"
    }

    // ---- Canvas ----
    Rectangle {
        id: mCanvas
        x: 286
        y: 142
        height: 760
        width: 760
        clip: true
        color: "#0f1015"

        Repeater {
            model: 14 * 14
            delegate: Rectangle {
                required property int index
                x: (index % 14) * 56 + 2
                y: Math.floor(index / 14) * 14 + 14
                width: 2
                height: 2
                radius: 1
                color: "#232530"
            }
        }

        Image {
            x: 14
            y: 170
            source: Qt.resolvedUrl("assets/slide_bg.png")
        }
        Image {
            x: 74
            y: 212
            source: Qt.resolvedUrl("assets/slide_logo.png")
        }
        Text {
            x: 78
            y: 218
            color: "#eef0f6"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Bold
            text: qsTr("VGR")
        }
        Text {
            x: 264
            y: 212
            width: 221
            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("SUNDAY · AUGUST 16, 2026")
        }

        Text {
            x: 144
            y: 318
            width: 461
            color: "#f2f4fa"
            font.family: "Inter"
            font.pixelSize: 44
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("WELCOME HOME")
        }

        Rectangle {
            x: 138
            y: 312
            height: 76
            width: 472
            border.color: "#aeb6c8"
            border.width: 1.50
            color: "#0dffffff"
        }
        Image { x: 135; y: 309; source: Qt.resolvedUrl("assets/sel_h_421_451.png") }
        Image { x: 607; y: 309; source: Qt.resolvedUrl("assets/sel_h_893_451.png") }
        Image { x: 135; y: 385; source: Qt.resolvedUrl("assets/sel_h_421_527.png") }
        Image { x: 607; y: 385; source: Qt.resolvedUrl("assets/sel_h_893_527.png") }
        Rectangle {
            x: 312
            y: 288
            height: 18
            width: 124
            color: "#296c5ce7"
            radius: 4

            Text {
                anchors.centerIn: parent
                color: "#6c5ce7"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Medium
                text: qsTr("TEXT · WELCOME HOME")
            }
        }

        Text {
            x: 199
            y: 402
            width: 351
            color: "#c7cbd8"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("“For where two or three gather in my name,")
        }
        Text {
            x: 274
            y: 424
            width: 201
            color: "#c7cbd8"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("there am I with them.”")
        }
        Text {
            x: 319
            y: 448
            width: 111
            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("— Matthew 18:20")
        }

        Rectangle {
            id: camPanel
            x: 562
            y: 448
            height: 84
            width: 112

            Shape {
                anchors.fill: parent
                ShapePath {
                    fillGradient: LinearGradient {
                        x1: camPanel.width * 0.5; x2: camPanel.width * 0.5
                        y1: 0; y2: camPanel.height
                        GradientStop { color: "#ff123326"; position: 0 }
                        GradientStop { color: "#ff07130e"; position: 1 }
                    }
                    strokeColor: "#000"
                    strokeWidth: 0
                    PathRectangle { width: camPanel.width; height: camPanel.height; radius: 8 }
                }
            }
            Image { x: 8; y: 8; source: Qt.resolvedUrl("assets/cam_dot.png") }
            Text {
                x: 18; y: 7
                color: "#e2e8f0"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Medium
                text: qsTr("LIVE")
            }
            Rectangle { x: 10; y: 24; height: 34; width: 36; color: "#14503a"; radius: 4 }
            Rectangle { x: 30; y: 24; height: 34; width: 24; color: "#0f3a2c"; radius: 4 }
            Image { x: 96; y: 10; source: Qt.resolvedUrl("assets/cam_lens.png") }
            Text {
                x: 10; y: 68
                color: "#eef0f6"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Medium
                text: qsTr("CAM 1")
            }
            Text {
                x: 68; y: 68
                color: "#5c6475"
                font.family: "Inter"
                font.pixelSize: 8
                text: qsTr("10:24:07")
            }
        }

        Text {
            x: 6
            y: 10
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 10
            text: qsTr("Sunday Service  ·  Slide 1 — Title")
        }
    }

    Rectangle {
        x: 882
        y: 841
        height: 32
        width: 124
        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8

        Row {
            anchors.fill: parent
            Text { width: 41; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.pixelSize: 12; text: "−" }
            Text { width: 42; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 10; text: qsTr("100%") }
            Text { width: 41; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.pixelSize: 12; text: "+" }
        }
    }

    Rectangle {
        id: addContentChip
        x: 656
        y: 838
        height: 46
        width: 46
        radius: 23
        color: "#6c5ce7"

        Text {
            anchors.centerIn: parent
            color: "#ffffff"
            font.family: "Inter"
            font.pixelSize: 22
            rotation: 45
            text: "＋"
        }
    }

    // ---- Left panel: slide list ----
    Rectangle {
        id: leftPanel
        y: 48
        height: 852
        width: 280
        border.color: "#232530"
        border.width: 1
        color: "#12131a"

        Rectangle {
            x: 12
            y: 6
            height: 64
            width: 256
            border.color: "#252836"
            border.width: 1
            color: "#171924"
            radius: 10

            Rectangle {
                x: 8
                y: 8
                height: 34
                width: 34
                border.color: "#3a4a7a"
                border.width: 1
                color: "#1a2240"
                radius: 8

                Text {
                    anchors.centerIn: parent
                    color: "#7b9eff"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                    text: qsTr("VGR")
                }
            }
            Text {
                x: 50
                y: 13
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                text: qsTr("Sunday Service")
            }
            Text {
                x: 50
                y: 32
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 9
                text: qsTr("12 slides · Worship template")
            }
            Text {
                x: 236
                y: 24
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 11
                text: "⌄"
            }
        }

        Rectangle {
            x: 12
            y: 80
            height: 31
            width: 256
            border.color: "#232530"
            border.width: 1
            color: "#1a1c26"
            radius: 8

            Image { x: 10; y: 9; source: Qt.resolvedUrl("assets/so_search_ic.png") }
            Text {
                x: 32
                y: 8
                color: "#5c6475"
                font.family: "Inter"
                font.pixelSize: 11
                text: qsTr("Search this show…")
            }
        }

        Text {
            x: 12
            y: 122
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 10
            text: qsTr("SLIDES · 12")
        }

        Column {
            x: 12
            y: 140
            spacing: 8

            Repeater {
                model: slideModel
                delegate: Rectangle {
                    id: slideRow
                    required property int index
                    required property int num
                    required property bool active
                    required property string tag
                    required property string tagColor
                    required property string title
                    required property string line1
                    required property string line2
                    required property string ref

                    height: 124
                    width: 256
                    border.color: active ? "#6c5ce7" : "#232530"
                    border.width: active ? 1.2 : 1
                    color: "#161823"
                    radius: 9

                    Rectangle {
                        visible: slideRow.active
                        height: parent.height
                        width: 3
                        radius: 2
                        color: "#6c5ce7"
                    }

                    Rectangle {
                        x: 16
                        y: 4
                        height: 108
                        width: 224
                        border.color: "#262a38"
                        border.width: 1
                        color: "#0d0f16"
                        radius: 8

                        Column {
                            visible: slideRow.tag !== ""
                            anchors.centerIn: parent
                            width: parent.width - 12
                            spacing: 3

                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                color: slideRow.tagColor || "#9b8ff5"
                                font.family: "Inter"
                                font.pixelSize: 7
                                font.weight: Font.Normal
                                text: slideRow.tag
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                color: "#f2f4fa"
                                font.family: "Inter"
                                font.pixelSize: 12
                                text: slideRow.title
                                wrapMode: Text.Wrap
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                color: "#8a8fa3"
                                font.family: "Inter"
                                font.pixelSize: 8
                                text: slideRow.line1
                                wrapMode: Text.Wrap
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                color: "#8a8fa3"
                                font.family: "Inter"
                                font.pixelSize: 8
                                text: slideRow.line2
                                wrapMode: Text.Wrap
                            }
                            Text {
                                width: parent.width
                                visible: slideRow.ref !== ""
                                horizontalAlignment: Text.AlignHCenter
                                color: "#6b7080"
                                font.family: "Inter"
                                font.pixelSize: 7
                                text: slideRow.ref
                            }
                        }
                    }

                    Rectangle {
                        x: 218
                        y: 8
                        height: 18
                        width: 18
                        color: "#1c2030"
                        radius: 4

                        Text {
                            anchors.centerIn: parent
                            color: "#c9cedd"
                            font.family: "Inter"
                            font.pixelSize: 8
                            text: String(slideRow.num)
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: slideModel.selectSlide(slideRow.index)
                    }
                }
            }

            Rectangle {
                id: addSlideButton
                height: 36
                width: 256
                border.color: "#2a3140"
                border.width: 1
                color: addSlideArea.containsMouse ? "#20242f" : "#1a1c26"
                radius: 8
                Behavior on color { ColorAnimation { duration: 100 } }

                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    Text { color: "#6c5ce7"; font.family: "Inter"; font.pixelSize: 12; font.weight: Font.Medium; text: "+" }
                    Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("Add slide") }
                }

                MouseArea {
                    id: addSlideArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: slideModel.addSlide()
                }
            }
        }
    }

    // ---- Right panel ----
    Rectangle {
        id: rightPanel
        x: 1040
        y: 48
        height: 852
        width: 400
        color: "#0f1015"

        Row {
            x: 32
            y: 19
            spacing: 56

            Text { color: "#ff4d3d"; font.family: "Inter"; font.pixelSize: 11; font.weight: Font.Medium; text: qsTr("ITEMS") }
            Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("TEXT") }
            Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("SLIDE") }
        }
        Rectangle { y: 40; height: 3; width: 100; color: "#ff4d3d"; radius: 1.50 }
        Rectangle { y: 43; height: 1; width: 400; color: "#232530" }

        Column {
            x: 12
            y: 62
            spacing: 11

            Repeater {
                model: [ [root.outputs[0], root.outputs[1]], [root.outputs[2], root.outputs[3]] ]
                delegate: Row {
                    required property var modelData
                    spacing: 12

                    Repeater {
                        model: modelData
                        delegate: Rectangle {
                            required property var modelData
                            height: 143
                            width: 182
                            border.color: modelData.active ? "#85261f" : "#232530"
                            border.width: 1
                            color: "#16171e"
                            radius: 8

                            Rectangle {
                                x: 6
                                y: 6
                                height: 110
                                width: 170
                                clip: true
                                color: "#101116"
                                radius: 4

                                Rectangle {
                                    x: 6
                                    y: 6
                                    height: 16
                                    width: parent.width * 0.32
                                    color: "#b3000000"
                                    radius: 4

                                    Text {
                                        anchors.centerIn: parent
                                        color: "#e2e8f0"
                                        font.family: "Inter"
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        text: modelData.badge
                                    }
                                }
                            }
                            Row {
                                x: 6
                                y: 120
                                width: 170
                                Text {
                                    color: "#e2e8f0"
                                    font.family: "Inter"
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                    text: modelData.name
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            x: 16
            y: 390
            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 10
            font.weight: Font.Bold
            text: qsTr("SLIDE")
        }
        Rectangle {
            x: 8
            y: 406
            height: 46
            width: 384
            color: "#161823"
            radius: 8

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                text: qsTr("Background")
            }
            Rectangle {
                x: 256
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                border.color: "#3a4a7a"
                border.width: 1
                color: "#1a2240"
                radius: 5
            }
            Rectangle {
                x: 298
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                border.color: "#2a3140"
                border.width: 1
                color: "#1a1c26"
                radius: 12

                Text {
                    anchors.centerIn: parent
                    color: "#aeb6c8"
                    font.family: "Inter"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    text: qsTr("Change")
                }
            }
            Text {
                x: 368
                anchors.verticalCenter: parent.verticalCenter
                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 14
                text: "›"
            }
        }
    }

    AppMenuBar {
        anchors.fill: parent
    }
}
