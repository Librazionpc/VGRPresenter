import QtQuick
import QtQuick.Shapes

Rectangle {
    id: vGRPresenter_Main_Screen_Edit_Add_Media

    height: 900
    width: 1440

    clip: true
    color: "#12131a"

    Rectangle {
        id: hdr_bg

        height: 48
        width: 1440

        color: "#12131a"
    }
    Text {
        id: logo

        x: 16
        y: 12

        height: 18
        width: 102

        color: "#eef0f6"
        font.family: "Inter"
        font.pixelSize: 15
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("VGRPresenter")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: sys_file

        x: 154
        y: 16

        height: 13
        width: 19

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("File")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: sys_edit

        x: 186
        y: 16

        height: 13
        width: 21

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Edit")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: sys_view

        x: 218
        y: 16

        height: 13
        width: 26

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("View")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: sys_help

        x: 250
        y: 16

        height: 13
        width: 24

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Help")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: tabs_bg

        x: 640
        y: 8

        height: 32
        width: 268

        color: "#1e1f29"
        radius: 8
    }
    Rectangle {
        id: tab_show_ic

        x: 654
        y: 16

        height: 10
        width: 12

        border.color: "#8a94a6"
        border.width: 1.20
        color: "#d9d9d9"
        radius: 2
    }
    Text {
        id: tab_show_t

        x: 678
        y: 15

        height: 14
        width: 32

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Show")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: tab_edit_pen

        x: 749.17
        y: 18.22

        height: 11
        width: 2

        color: "#6c5ce7"
        rotation: -38
    }
    Rectangle {
        id: tab_edit_nib

        x: 742.81
        y: 15.34

        height: 4
        width: 4

        color: "#6c5ce7"
        rotation: -38
    }
    Text {
        id: tab_edit_t

        x: 764
        y: 15

        height: 14
        width: 23

        color: "#6c5ce7"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Edit")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: tab_edit_bar

        x: 734
        y: 36

        height: 3
        width: 79

        color: "#6c5ce7"
        radius: 1.50
    }
    Rectangle {
        id: tab_stage_0

        x: 824
        y: 17

        height: 5
        width: 5

        border.color: "#8a94a6"
        border.width: 1.10
        color: "#d9d9d9"
    }
    Rectangle {
        id: tab_stage_1

        x: 831
        y: 17

        height: 5
        width: 5

        border.color: "#8a94a6"
        border.width: 1.10
        color: "#d9d9d9"
    }
    Rectangle {
        id: tab_stage_2

        x: 824
        y: 24

        height: 5
        width: 5

        border.color: "#8a94a6"
        border.width: 1.10
        color: "#d9d9d9"
    }
    Rectangle {
        id: tab_stage_3

        x: 831
        y: 24

        height: 5
        width: 5

        border.color: "#8a94a6"
        border.width: 1.10
        color: "#d9d9d9"
    }
    Text {
        id: tab_stage_t

        x: 848
        y: 15

        height: 14
        width: 33

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Stage")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Image {
        id: stat_dot

        x: 1260
        y: 18

        source: Qt.resolvedUrl("assets/stat_dot_3.png")
    }
    Text {
        id: stat_txt

        x: 1278
        y: 17

        height: 13
        width: 56

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Connected")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Image {
        id: win_close

        x: 1364
        y: 18

        source: Qt.resolvedUrl("assets/win_close_3.png")
    }
    Image {
        id: win_min

        x: 1384
        y: 18

        source: Qt.resolvedUrl("assets/win_min_3.png")
    }
    Image {
        id: win_max

        x: 1404
        y: 18

        source: Qt.resolvedUrl("assets/win_max_3.png")
    }
    Rectangle {
        id: m_hdr

        x: 280
        y: 48

        height: 48
        width: 760

        color: "#15161d"
    }
    Rectangle {
        id: m_hdr_line

        x: 280
        y: 96

        height: 1
        width: 760

        color: "#232530"
    }
    Rectangle {
        id: m_back

        x: 292
        y: 58

        height: 28
        width: 28

        color: "#1e1f29"
        radius: 7
    }
    Text {
        id: m_back_t

        x: 300
        y: 64

        height: 16
        width: 6

        color: "#eef0f6"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("‹")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: m_show

        x: 330
        y: 62

        height: 16
        width: 98

        color: "#eef0f6"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Sunday Service")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_tpl

        x: 436
        y: 60

        height: 24
        width: 104

        border.color: "#3a2230"
        border.width: 1
        color: "#2a1c24"
        radius: 6
    }
    Text {
        id: m_tpl_t

        x: 442
        y: 65

        height: 11
        width: 89

        color: "#ff4d3d"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Template · Worship")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: m_saved

        x: 836
        y: 65

        height: 11
        width: 60

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Autosaved ✓")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: m_fit

        x: 918
        y: 65

        height: 11
        width: 13

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Fit")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_undo

        x: 942
        y: 58

        height: 28
        width: 28

        color: "#1a1c26"
        radius: 7
    }
    Text {
        id: m_undo_t

        x: 949
        y: 64

        height: 15
        width: 13

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("↺")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_redo

        x: 974
        y: 58

        height: 28
        width: 28

        color: "#1a1c26"
        radius: 7
    }
    Text {
        id: m_redo_t

        x: 981
        y: 64

        height: 15
        width: 13

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("↻")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_zoom

        x: 1006
        y: 58

        height: 28
        width: 34

        color: "#1a1c26"
        radius: 7
    }
    Text {
        id: m_zoom_t

        x: 1013
        y: 64

        height: 12
        width: 27

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("100%")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_canvas

        x: 286
        y: 142

        height: 760
        width: 760

        color: "#0f1015"
    }
    Image {
        id: m_dot_288_156

        x: 288
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_288_158.png")
    }
    Image {
        id: m_dot_288_212

        x: 288
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_288_214.png")
    }
    Image {
        id: m_dot_288_268

        x: 288
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_288_270.png")
    }
    Image {
        id: m_dot_288_324

        x: 288
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_288_326.png")
    }
    Image {
        id: m_dot_288_380

        x: 288
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_288_382.png")
    }
    Image {
        id: m_dot_288_436

        x: 288
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_288_438.png")
    }
    Image {
        id: m_dot_288_492

        x: 288
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_288_494.png")
    }
    Image {
        id: m_dot_288_548

        x: 288
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_288_550.png")
    }
    Image {
        id: m_dot_288_604

        x: 288
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_288_606.png")
    }
    Image {
        id: m_dot_288_660

        x: 288
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_288_662.png")
    }
    Image {
        id: m_dot_288_716

        x: 288
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_288_718.png")
    }
    Image {
        id: m_dot_288_772

        x: 288
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_288_774.png")
    }
    Image {
        id: m_dot_288_828

        x: 288
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_288_830.png")
    }
    Image {
        id: m_dot_288_884

        x: 288
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_288_886.png")
    }
    Image {
        id: m_dot_344_156

        x: 344
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_344_158.png")
    }
    Image {
        id: m_dot_344_212

        x: 344
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_344_214.png")
    }
    Image {
        id: m_dot_344_268

        x: 344
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_344_270.png")
    }
    Image {
        id: m_dot_344_324

        x: 344
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_344_326.png")
    }
    Image {
        id: m_dot_344_380

        x: 344
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_344_382.png")
    }
    Image {
        id: m_dot_344_436

        x: 344
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_344_438.png")
    }
    Image {
        id: m_dot_344_492

        x: 344
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_344_494.png")
    }
    Image {
        id: m_dot_344_548

        x: 344
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_344_550.png")
    }
    Image {
        id: m_dot_344_604

        x: 344
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_344_606.png")
    }
    Image {
        id: m_dot_344_660

        x: 344
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_344_662.png")
    }
    Image {
        id: m_dot_344_716

        x: 344
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_344_718.png")
    }
    Image {
        id: m_dot_344_772

        x: 344
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_344_774.png")
    }
    Image {
        id: m_dot_344_828

        x: 344
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_344_830.png")
    }
    Image {
        id: m_dot_344_884

        x: 344
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_344_886.png")
    }
    Image {
        id: m_dot_400_156

        x: 400
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_400_158.png")
    }
    Image {
        id: m_dot_400_212

        x: 400
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_400_214.png")
    }
    Image {
        id: m_dot_400_268

        x: 400
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_400_270.png")
    }
    Image {
        id: m_dot_400_324

        x: 400
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_400_326.png")
    }
    Image {
        id: m_dot_400_380

        x: 400
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_400_382.png")
    }
    Image {
        id: m_dot_400_436

        x: 400
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_400_438.png")
    }
    Image {
        id: m_dot_400_492

        x: 400
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_400_494.png")
    }
    Image {
        id: m_dot_400_548

        x: 400
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_400_550.png")
    }
    Image {
        id: m_dot_400_604

        x: 400
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_400_606.png")
    }
    Image {
        id: m_dot_400_660

        x: 400
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_400_662.png")
    }
    Image {
        id: m_dot_400_716

        x: 400
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_400_718.png")
    }
    Image {
        id: m_dot_400_772

        x: 400
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_400_774.png")
    }
    Image {
        id: m_dot_400_828

        x: 400
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_400_830.png")
    }
    Image {
        id: m_dot_400_884

        x: 400
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_400_886.png")
    }
    Image {
        id: m_dot_456_156

        x: 456
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_456_158.png")
    }
    Image {
        id: m_dot_456_212

        x: 456
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_456_214.png")
    }
    Image {
        id: m_dot_456_268

        x: 456
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_456_270.png")
    }
    Image {
        id: m_dot_456_324

        x: 456
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_456_326.png")
    }
    Image {
        id: m_dot_456_380

        x: 456
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_456_382.png")
    }
    Image {
        id: m_dot_456_436

        x: 456
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_456_438.png")
    }
    Image {
        id: m_dot_456_492

        x: 456
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_456_494.png")
    }
    Image {
        id: m_dot_456_548

        x: 456
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_456_550.png")
    }
    Image {
        id: m_dot_456_604

        x: 456
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_456_606.png")
    }
    Image {
        id: m_dot_456_660

        x: 456
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_456_662.png")
    }
    Image {
        id: m_dot_456_716

        x: 456
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_456_718.png")
    }
    Image {
        id: m_dot_456_772

        x: 456
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_456_774.png")
    }
    Image {
        id: m_dot_456_828

        x: 456
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_456_830.png")
    }
    Image {
        id: m_dot_456_884

        x: 456
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_456_886.png")
    }
    Image {
        id: m_dot_512_156

        x: 512
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_512_158.png")
    }
    Image {
        id: m_dot_512_212

        x: 512
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_512_214.png")
    }
    Image {
        id: m_dot_512_268

        x: 512
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_512_270.png")
    }
    Image {
        id: m_dot_512_324

        x: 512
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_512_326.png")
    }
    Image {
        id: m_dot_512_380

        x: 512
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_512_382.png")
    }
    Image {
        id: m_dot_512_436

        x: 512
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_512_438.png")
    }
    Image {
        id: m_dot_512_492

        x: 512
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_512_494.png")
    }
    Image {
        id: m_dot_512_548

        x: 512
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_512_550.png")
    }
    Image {
        id: m_dot_512_604

        x: 512
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_512_606.png")
    }
    Image {
        id: m_dot_512_660

        x: 512
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_512_662.png")
    }
    Image {
        id: m_dot_512_716

        x: 512
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_512_718.png")
    }
    Image {
        id: m_dot_512_772

        x: 512
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_512_774.png")
    }
    Image {
        id: m_dot_512_828

        x: 512
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_512_830.png")
    }
    Image {
        id: m_dot_512_884

        x: 512
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_512_886.png")
    }
    Image {
        id: m_dot_568_156

        x: 568
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_568_158.png")
    }
    Image {
        id: m_dot_568_212

        x: 568
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_568_214.png")
    }
    Image {
        id: m_dot_568_268

        x: 568
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_568_270.png")
    }
    Image {
        id: m_dot_568_324

        x: 568
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_568_326.png")
    }
    Image {
        id: m_dot_568_380

        x: 568
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_568_382.png")
    }
    Image {
        id: m_dot_568_436

        x: 568
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_568_438.png")
    }
    Image {
        id: m_dot_568_492

        x: 568
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_568_494.png")
    }
    Image {
        id: m_dot_568_548

        x: 568
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_568_550.png")
    }
    Image {
        id: m_dot_568_604

        x: 568
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_568_606.png")
    }
    Image {
        id: m_dot_568_660

        x: 568
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_568_662.png")
    }
    Image {
        id: m_dot_568_716

        x: 568
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_568_718.png")
    }
    Image {
        id: m_dot_568_772

        x: 568
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_568_774.png")
    }
    Image {
        id: m_dot_568_828

        x: 568
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_568_830.png")
    }
    Image {
        id: m_dot_568_884

        x: 568
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_568_886.png")
    }
    Image {
        id: m_dot_624_156

        x: 624
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_624_158.png")
    }
    Image {
        id: m_dot_624_212

        x: 624
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_624_214.png")
    }
    Image {
        id: m_dot_624_268

        x: 624
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_624_270.png")
    }
    Image {
        id: m_dot_624_324

        x: 624
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_624_326.png")
    }
    Image {
        id: m_dot_624_380

        x: 624
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_624_382.png")
    }
    Image {
        id: m_dot_624_436

        x: 624
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_624_438.png")
    }
    Image {
        id: m_dot_624_492

        x: 624
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_624_494.png")
    }
    Image {
        id: m_dot_624_548

        x: 624
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_624_550.png")
    }
    Image {
        id: m_dot_624_604

        x: 624
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_624_606.png")
    }
    Image {
        id: m_dot_624_660

        x: 624
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_624_662.png")
    }
    Image {
        id: m_dot_624_716

        x: 624
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_624_718.png")
    }
    Image {
        id: m_dot_624_772

        x: 624
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_624_774.png")
    }
    Image {
        id: m_dot_624_828

        x: 624
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_624_830.png")
    }
    Image {
        id: m_dot_624_884

        x: 624
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_624_886.png")
    }
    Image {
        id: m_dot_680_156

        x: 680
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_680_158.png")
    }
    Image {
        id: m_dot_680_212

        x: 680
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_680_214.png")
    }
    Image {
        id: m_dot_680_268

        x: 680
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_680_270.png")
    }
    Image {
        id: m_dot_680_324

        x: 680
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_680_326.png")
    }
    Image {
        id: m_dot_680_380

        x: 680
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_680_382.png")
    }
    Image {
        id: m_dot_680_436

        x: 680
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_680_438.png")
    }
    Image {
        id: m_dot_680_492

        x: 680
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_680_494.png")
    }
    Image {
        id: m_dot_680_548

        x: 680
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_680_550.png")
    }
    Image {
        id: m_dot_680_604

        x: 680
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_680_606.png")
    }
    Image {
        id: m_dot_680_660

        x: 680
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_680_662.png")
    }
    Image {
        id: m_dot_680_716

        x: 680
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_680_718.png")
    }
    Image {
        id: m_dot_680_772

        x: 680
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_680_774.png")
    }
    Image {
        id: m_dot_680_828

        x: 680
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_680_830.png")
    }
    Image {
        id: m_dot_680_884

        x: 680
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_680_886.png")
    }
    Image {
        id: m_dot_736_156

        x: 736
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_736_158.png")
    }
    Image {
        id: m_dot_736_212

        x: 736
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_736_214.png")
    }
    Image {
        id: m_dot_736_268

        x: 736
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_736_270.png")
    }
    Image {
        id: m_dot_736_324

        x: 736
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_736_326.png")
    }
    Image {
        id: m_dot_736_380

        x: 736
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_736_382.png")
    }
    Image {
        id: m_dot_736_436

        x: 736
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_736_438.png")
    }
    Image {
        id: m_dot_736_492

        x: 736
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_736_494.png")
    }
    Image {
        id: m_dot_736_548

        x: 736
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_736_550.png")
    }
    Image {
        id: m_dot_736_604

        x: 736
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_736_606.png")
    }
    Image {
        id: m_dot_736_660

        x: 736
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_736_662.png")
    }
    Image {
        id: m_dot_736_716

        x: 736
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_736_718.png")
    }
    Image {
        id: m_dot_736_772

        x: 736
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_736_774.png")
    }
    Image {
        id: m_dot_736_828

        x: 736
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_736_830.png")
    }
    Image {
        id: m_dot_736_884

        x: 736
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_736_886.png")
    }
    Image {
        id: m_dot_792_156

        x: 792
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_792_158.png")
    }
    Image {
        id: m_dot_792_212

        x: 792
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_792_214.png")
    }
    Image {
        id: m_dot_792_268

        x: 792
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_792_270.png")
    }
    Image {
        id: m_dot_792_324

        x: 792
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_792_326.png")
    }
    Image {
        id: m_dot_792_380

        x: 792
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_792_382.png")
    }
    Image {
        id: m_dot_792_436

        x: 792
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_792_438.png")
    }
    Image {
        id: m_dot_792_492

        x: 792
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_792_494.png")
    }
    Image {
        id: m_dot_792_548

        x: 792
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_792_550.png")
    }
    Image {
        id: m_dot_792_604

        x: 792
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_792_606.png")
    }
    Image {
        id: m_dot_792_660

        x: 792
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_792_662.png")
    }
    Image {
        id: m_dot_792_716

        x: 792
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_792_718.png")
    }
    Image {
        id: m_dot_792_772

        x: 792
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_792_774.png")
    }
    Image {
        id: m_dot_792_828

        x: 792
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_792_830.png")
    }
    Image {
        id: m_dot_792_884

        x: 792
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_792_886.png")
    }
    Image {
        id: m_dot_848_156

        x: 848
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_848_158.png")
    }
    Image {
        id: m_dot_848_212

        x: 848
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_848_214.png")
    }
    Image {
        id: m_dot_848_268

        x: 848
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_848_270.png")
    }
    Image {
        id: m_dot_848_324

        x: 848
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_848_326.png")
    }
    Image {
        id: m_dot_848_380

        x: 848
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_848_382.png")
    }
    Image {
        id: m_dot_848_436

        x: 848
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_848_438.png")
    }
    Image {
        id: m_dot_848_492

        x: 848
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_848_494.png")
    }
    Image {
        id: m_dot_848_548

        x: 848
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_848_550.png")
    }
    Image {
        id: m_dot_848_604

        x: 848
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_848_606.png")
    }
    Image {
        id: m_dot_848_660

        x: 848
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_848_662.png")
    }
    Image {
        id: m_dot_848_716

        x: 848
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_848_718.png")
    }
    Image {
        id: m_dot_848_772

        x: 848
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_848_774.png")
    }
    Image {
        id: m_dot_848_828

        x: 848
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_848_830.png")
    }
    Image {
        id: m_dot_848_884

        x: 848
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_848_886.png")
    }
    Image {
        id: m_dot_904_156

        x: 904
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_904_158.png")
    }
    Image {
        id: m_dot_904_212

        x: 904
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_904_214.png")
    }
    Image {
        id: m_dot_904_268

        x: 904
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_904_270.png")
    }
    Image {
        id: m_dot_904_324

        x: 904
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_904_326.png")
    }
    Image {
        id: m_dot_904_380

        x: 904
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_904_382.png")
    }
    Image {
        id: m_dot_904_436

        x: 904
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_904_438.png")
    }
    Image {
        id: m_dot_904_492

        x: 904
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_904_494.png")
    }
    Image {
        id: m_dot_904_548

        x: 904
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_904_550.png")
    }
    Image {
        id: m_dot_904_604

        x: 904
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_904_606.png")
    }
    Image {
        id: m_dot_904_660

        x: 904
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_904_662.png")
    }
    Image {
        id: m_dot_904_716

        x: 904
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_904_718.png")
    }
    Image {
        id: m_dot_904_772

        x: 904
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_904_774.png")
    }
    Image {
        id: m_dot_904_828

        x: 904
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_904_830.png")
    }
    Image {
        id: m_dot_904_884

        x: 904
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_904_886.png")
    }
    Image {
        id: m_dot_960_156

        x: 960
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_960_158.png")
    }
    Image {
        id: m_dot_960_212

        x: 960
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_960_214.png")
    }
    Image {
        id: m_dot_960_268

        x: 960
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_960_270.png")
    }
    Image {
        id: m_dot_960_324

        x: 960
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_960_326.png")
    }
    Image {
        id: m_dot_960_380

        x: 960
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_960_382.png")
    }
    Image {
        id: m_dot_960_436

        x: 960
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_960_438.png")
    }
    Image {
        id: m_dot_960_492

        x: 960
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_960_494.png")
    }
    Image {
        id: m_dot_960_548

        x: 960
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_960_550.png")
    }
    Image {
        id: m_dot_960_604

        x: 960
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_960_606.png")
    }
    Image {
        id: m_dot_960_660

        x: 960
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_960_662.png")
    }
    Image {
        id: m_dot_960_716

        x: 960
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_960_718.png")
    }
    Image {
        id: m_dot_960_772

        x: 960
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_960_774.png")
    }
    Image {
        id: m_dot_960_828

        x: 960
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_960_830.png")
    }
    Image {
        id: m_dot_960_884

        x: 960
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_960_886.png")
    }
    Image {
        id: m_dot_1016_156

        x: 1016
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_1016_158.png")
    }
    Image {
        id: m_dot_1016_212

        x: 1016
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_1016_214.png")
    }
    Image {
        id: m_dot_1016_268

        x: 1016
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_1016_270.png")
    }
    Image {
        id: m_dot_1016_324

        x: 1016
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_1016_326.png")
    }
    Image {
        id: m_dot_1016_380

        x: 1016
        y: 380

        source: Qt.resolvedUrl("assets/m_dot_1016_382.png")
    }
    Image {
        id: m_dot_1016_436

        x: 1016
        y: 436

        source: Qt.resolvedUrl("assets/m_dot_1016_438.png")
    }
    Image {
        id: m_dot_1016_492

        x: 1016
        y: 492

        source: Qt.resolvedUrl("assets/m_dot_1016_494.png")
    }
    Image {
        id: m_dot_1016_548

        x: 1016
        y: 548

        source: Qt.resolvedUrl("assets/m_dot_1016_550.png")
    }
    Image {
        id: m_dot_1016_604

        x: 1016
        y: 604

        source: Qt.resolvedUrl("assets/m_dot_1016_606.png")
    }
    Image {
        id: m_dot_1016_660

        x: 1016
        y: 660

        source: Qt.resolvedUrl("assets/m_dot_1016_662.png")
    }
    Image {
        id: m_dot_1016_716

        x: 1016
        y: 716

        source: Qt.resolvedUrl("assets/m_dot_1016_718.png")
    }
    Image {
        id: m_dot_1016_772

        x: 1016
        y: 772

        source: Qt.resolvedUrl("assets/m_dot_1016_774.png")
    }
    Image {
        id: m_dot_1016_828

        x: 1016
        y: 828

        source: Qt.resolvedUrl("assets/m_dot_1016_830.png")
    }
    Image {
        id: m_dot_1016_884

        x: 1016
        y: 884

        source: Qt.resolvedUrl("assets/m_dot_1016_886.png")
    }
    Text {
        id: m_crumb

        x: 292
        y: 152

        height: 12
        width: 156

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Sunday Service  ·  Slide 1 — Title")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Image {
        id: slide_bg

        x: 300
        y: 312

        source: Qt.resolvedUrl("assets/slide_bg_3.png")
    }
    Image {
        id: slide_logo

        x: 360
        y: 354

        source: Qt.resolvedUrl("assets/slide_logo_3.png")
    }
    Text {
        id: slide_logo_t

        x: 364
        y: 360

        height: 9
        width: 17

        color: "#eef0f6"
        font.family: "Inter"
        font.pixelSize: 7
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("VGR")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: slide_title

        x: 430
        y: 460

        height: 64
        width: 461

        color: "#f2f4fa"
        font.family: "Inter"
        font.pixelSize: 44
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("WELCOME HOME")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: sel_box

        x: 424
        y: 454

        height: 76
        width: 472

        border.color: "#aeb6c8"
        border.width: 1.50
        color: "#0dffffff"
    }
    Image {
        id: sel_h_421_451

        x: 421
        y: 451

        source: Qt.resolvedUrl("assets/sel_h_421_453.png")
    }
    Image {
        id: sel_h_893_451

        x: 893
        y: 451

        source: Qt.resolvedUrl("assets/sel_h_893_453.png")
    }
    Image {
        id: sel_h_421_527

        x: 421
        y: 527

        source: Qt.resolvedUrl("assets/sel_h_421_529.png")
    }
    Image {
        id: sel_h_893_527

        x: 893
        y: 527

        source: Qt.resolvedUrl("assets/sel_h_893_529.png")
    }
    Rectangle {
        id: sel_tag

        x: 598
        y: 430

        height: 18
        width: 124

        color: "#296c5ce7"
        radius: 4
    }
    Text {
        id: sel_tag_t

        x: 608
        y: 434

        height: 10
        width: 96

        color: "#6c5ce7"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("TEXT · WELCOME HOME")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: slide_sub

        x: 550
        y: 354

        height: 13
        width: 221

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("SUNDAY · AUGUST 16, 2026")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_v0

        x: 485
        y: 544

        height: 16
        width: 351

        color: "#c7cbd8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("“For where two or three gather in my name,")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_v1

        x: 560
        y: 566

        height: 16
        width: 201

        color: "#c7cbd8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("there am I with them.”")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_ref

        x: 605
        y: 590

        height: 13
        width: 111

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("— Matthew 18:20")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Shape {
        id: cam_bg

        x: 848
        y: 590

        height: 84
        width: 112

        ShapePath {
            id: cam_bgShapePath

            strokeColor: "#000"
            strokeWidth: 0

            fillGradient: LinearGradient {
                id: gradientNode
            
                x1: cam_bg.width * 0.5
                x2: cam_bg.width * 0.5
                y1: cam_bg.height * 0
                y2: cam_bg.height * 1
            
                GradientStop {
                    color: "#ff123326"
                    position: 0
                }
                GradientStop {
                    color: "#ff07130e"
                    position: 1
                }
            }

            PathRectangle {
                id: cam_bgPathRectangle

                x: 0
                y: 0

                height: cam_bg.height
                width: cam_bg.width

                radius: 8
            }
        }
    }
    Image {
        id: cam_dot

        x: 856
        y: 598

        source: Qt.resolvedUrl("assets/cam_dot_3.png")
    }
    Text {
        id: cam_live

        x: 866
        y: 597

        height: 9
        width: 18

        color: "#e2e8f0"
        font.family: "Inter"
        font.pixelSize: 7
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("LIVE")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: cam_scene

        x: 858
        y: 614

        height: 34
        width: 36

        color: "#14503a"
        radius: 4
    }
    Rectangle {
        id: cam_scene2

        x: 878
        y: 614

        height: 34
        width: 24

        color: "#0f3a2c"
        radius: 4
    }
    Image {
        id: cam_lens

        x: 944
        y: 600

        source: Qt.resolvedUrl("assets/cam_lens_3.png")
    }
    Text {
        id: cam_name

        x: 858
        y: 658

        height: 10
        width: 28

        color: "#eef0f6"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("CAM 1")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: cam_time

        x: 916
        y: 658

        height: 10
        width: 34

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("10:24:07")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: m_zoombar

        x: 882
        y: 841

        height: 32
        width: 124

        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8
    }
    Text {
        id: m_zb_m

        x: 889
        y: 849

        height: 15
        width: 9

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("−")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: m_zb_100

        x: 931
        y: 851

        height: 12
        width: 27

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("100%")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: m_zb_p

        x: 993
        y: 849

        height: 15
        width: 9

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("+")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: r_bg

        x: 1040
        y: 48

        height: 852
        width: 400

        color: "#0f1015"
    }
    Text {
        id: r_tab0

        x: 1072.95
        y: 67

        height: 13
        width: 35

        color: "#ff4d3d"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("ITEMS")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: r_tab_bar

        x: 1040
        y: 88

        height: 3
        width: 100

        color: "#ff4d3d"
        radius: 1.50
    }
    Text {
        id: r_tab1

        x: 1176.36
        y: 67

        height: 13
        width: 29

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("TEXT")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: r_tab2

        x: 1272.95
        y: 67

        height: 13
        width: 32

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("SLIDE")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: r_tab_line

        x: 1040
        y: 91

        height: 1
        width: 400

        color: "#232530"
    }
    Rectangle {
        id: so_header

        x: 12
        y: 54

        height: 64
        width: 256

        color: "#191b24"
        radius: 10
    }
    Rectangle {
        id: so_ic

        x: 20
        y: 62

        height: 34
        width: 34

        border.color: "#3a4a7a"
        border.width: 1
        color: "#1a2240"
        radius: 8
    }
    Rectangle {
        id: so_search

        x: 12
        y: 128

        height: 31
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8
    }
    Text {
        id: so_sec

        x: 12
        y: 174

        height: 12
        width: 55

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("SLIDES · 12")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_s0_bg

        x: 12
        y: 192

        height: 124
        width: 256

        border.color: "#ff4d3d"
        border.width: 1.20
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s1_bg

        x: 12
        y: 356

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s2_bg

        x: 12
        y: 520

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s3_bg

        x: 12
        y: 684

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s4_bg

        x: 12
        y: 848

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s5_bg

        x: 12
        y: 1012

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s6_bg

        x: 12
        y: 1176

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_s7_bg

        x: 12
        y: 1340

        height: 124
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#161823"
        radius: 9
    }
    Rectangle {
        id: so_addslide

        x: 12
        y: 1508

        height: 36
        width: 256

        border.color: "#2a3140"
        border.width: 1
        color: "#1a1c26"
        radius: 8
    }
    Text {
        id: so_addslide_t

        x: 100
        y: 1519

        height: 13
        width: 64

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("＋ Add slide")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_s0_bar

        x: 12
        y: 192

        height: 124
        width: 3

        color: "#ff4d3d"
        radius: 2
    }
    Text {
        id: rp_sec_slide

        x: 1056
        y: 438

        height: 12
        width: 61

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignLeft
        text: qsTr("SLIDE")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: rp_row0

        x: 1048
        y: 454

        height: 46
        width: 384

        color: "#161823"
        radius: 8
    }
    Text {
        id: rp_row0_l

        x: 1062
        y: 470

        height: 15
        width: 91

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Background")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: rp_row0_sw

        x: 1304
        y: 466

        height: 24
        width: 24

        border.color: "#3a4a7a"
        border.width: 1
        color: "#1a2240"
        radius: 5
    }
    Text {
        id: rp_row0_chev

        x: 1408
        y: 469

        height: 17
        width: 11

        color: "#6b7280"
        font.family: "Inter"
        font.pixelSize: 14
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("›")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Image {
        id: madd_chip

        x: 286
        y: 838

        source: Qt.resolvedUrl("assets/madd_chip_3.png")
    }
    Text {
        id: madd_plus

        x: 296
        y: 846

        height: 28
        width: 25

        color: "#ffffff"
        font.family: "Inter"
        font.pixelSize: 22
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        rotation: 49.23
        text: qsTr("＋")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: madd_menu

        x: 344
        y: 837

        height: 46
        width: 388

        border.color: "#2a2f3a"
        border.width: 1
        color: "#151824"
        radius: 14
    }
    Rectangle {
        id: madd_c0

        x: 354
        y: 843

        height: 34
        width: 48

        border.color: "#406c5ce7"
        border.width: 1
        color: "#206c5ce7"
        radius: 8
    }
    Text {
        id: madd_c0_ic

        x: 368
        y: 847

        height: 12
        width: 21

        color: "#9b8ff5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Aa")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c0_lb

        x: 368
        y: 865

        height: 10
        width: 21

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Text")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c1

        x: 406
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c1_ic

        x: 425
        y: 845

        height: 13
        width: 11

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("◎")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c1_lb

        x: 416
        y: 864

        height: 9
        width: 45

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Camera")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c2

        x: 458
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c2_ic

        x: 476
        y: 847

        height: 12
        width: 13

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("▶")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c2_lb

        x: 469
        y: 865

        height: 8
        width: 26

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Media")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c3

        x: 510
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c3_ic

        x: 529
        y: 846

        height: 8
        width: 10

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("♪")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c3_lb

        x: 522
        y: 864

        height: 9
        width: 23

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Audio")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c4

        x: 562
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c4_ic

        x: 580
        y: 847

        height: 12
        width: 12

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("□")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c4_lb

        x: 573
        y: 864

        height: 9
        width: 45

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Shape")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c5

        x: 614
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c5_ic

        x: 631
        y: 848

        height: 12
        width: 21

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("⏱")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: madd_c5_lb

        x: 626
        y: 864

        height: 9
        width: 45

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Timer")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: madd_c6

        x: 666
        y: 843

        height: 34
        width: 48

        color: "#1b1e2a"
        radius: 8
    }
    Text {
        id: madd_c6_ic

        x: 686
        y: 846

        height: 11
        width: 8

        color: "#9aa0b5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("◷")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: madd_c6_lb

        x: 679
        y: 864

        height: 9
        width: 27

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Clock")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: so_s4_pv

        x: 16
        y: 852

        height: 120
        width: 224

        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8
    }
    Rectangle {
        id: so_s5_pv

        x: 16
        y: 1016

        height: 120
        width: 224

        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8
    }
    Rectangle {
        id: so_s6_pv

        x: 16
        y: 1180

        height: 120
        width: 224

        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8
    }
    Rectangle {
        id: so_s7_pv

        x: 16
        y: 1344

        height: 120
        width: 224

        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8
    }
    Rectangle {
        id: so_s4_pv_num

        x: 218
        y: 858

        height: 18
        width: 18

        color: "#1c2030"
        radius: 4
    }
    Text {
        id: so_s4_pv_num_t

        x: 222
        y: 861

        height: 11
        width: 11

        color: "#c9cedd"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("5")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_s5_pv_num

        x: 218
        y: 1022

        height: 18
        width: 18

        color: "#1c2030"
        radius: 4
    }
    Text {
        id: so_s5_pv_num_t

        x: 222
        y: 1025

        height: 11
        width: 11

        color: "#c9cedd"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("6")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_s6_pv_num

        x: 218
        y: 1186

        height: 18
        width: 18

        color: "#1c2030"
        radius: 4
    }
    Text {
        id: so_s6_pv_num_t

        x: 222
        y: 1189

        height: 11
        width: 11

        color: "#c9cedd"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("7")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_s7_pv_num

        x: 218
        y: 1350

        height: 18
        width: 18

        color: "#1c2030"
        radius: 4
    }
    Text {
        id: so_s7_pv_num_t

        x: 222
        y: 1353

        height: 11
        width: 11

        color: "#c9cedd"
        font.family: "Inter"
        font.pixelSize: 8
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("8")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: preview_monitors_grid

        x: 1040
        y: 100

        height: 322
        width: 400

        color: "transparent"

        Rectangle {
            id: preview_row_1

            x: 12
            y: 12

            height: 143
            width: 376

            color: "transparent"

            Rectangle {
                id: preview_Main_Output

                height: 143
                width: 182

                border.color: "#85261f"
                border.width: 1
                color: "#16171e"
                radius: 8

                Rectangle {
                    id: preview_screen

                    x: 6
                    y: 6

                    height: 110
                    width: 170

                    clip: true
                    color: "transparent"
                    radius: 4

                    Rectangle {
                        id: checkered_grid

                        height: 110
                        width: 170

                        color: "#101116"

                        Rectangle {
                            id: rectangle

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_1

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_2

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_3

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_4

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_5

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_6

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_7

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_8

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_9

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_10

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_11

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_12

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_13

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_14

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_15

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_16

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_17

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_18

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_19

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_20

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_21

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_22

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_23

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_24

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_25

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_26

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_27

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_28

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_29

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_30

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_31

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_32

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_33

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_34

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_35

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_36

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_37

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_38

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_39

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_40

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_41

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_42

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_43

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_44

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_45

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_46

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_47

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                    }
                    Rectangle {
                        id: frame

                        x: 6
                        y: 6

                        height: 16
                        width: 42

                        color: "#b3000000"
                        radius: 4

                        Text {
                            id: lIVE_1

                            x: 6
                            y: 2

                            height: 12
                            width: 31

                            color: "#e2e8f0"
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("LIVE 1")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                    Rectangle {
                        id: frame_1

                        x: 73
                        y: 43

                        height: 24
                        width: 24

                        color: "transparent"

                        Rectangle {
                            id: play_circle

                            height: 24
                            width: 24

                            clip: true
                            color: "transparent"

                            Shape {
                                id: _vector

                                x: 2
                                y: 2

                                height: 20
                                width: 20

                                ShapePath {
                                    id: _vector_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#ff4d3d"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_ShapePath0_PathSvg0

                                        path: "M 7.00056819980632 7.003578236772546 C 7.000032676166251 6.826097734002283 7.04673782193803 6.651671274848085 7.135889199451094 6.498205810401713 C 7.225040576964157 6.3447403459553415 7.3534259169807425 6.217764425824266 7.507866248685013 6.130312721335531 C 7.662306580389283 6.042861016846795 7.837238437959556 5.998083942575108 8.014702205386856 6.00057879284036 C 8.192165972814156 6.003073643105613 8.365768376527718 6.052750658238576 8.517689301449536 6.14450929935906 L 13.515088868365273 9.141749316599816 C 13.66386500924066 9.230421896397774 13.787062452548078 9.35621261745387 13.872617549411187 9.506803090061293 C 13.958172646274296 9.657393562668716 14.00315167467906 9.827620404489785 14.003151674679064 10.000817300262678 C 14.00315167467906 10.174014196035571 13.958172646274296 10.344242945357886 13.872617549411187 10.494833417965308 C 13.787062452548078 10.645423890572731 13.66386500924066 10.771214611628828 13.515088868365273 10.859887191426786 L 8.517689301449536 13.857126731792231 C 8.365695697749775 13.94893012468023 8.192000139424579 13.998608936943883 8.014450415222381 14.001061053313423 C 7.836900691020182 14.003513169682963 7.661899388482671 13.958649052161668 7.50742847714905 13.871078195905646 C 7.352957565815429 13.783507339649624 7.224587634832346 13.656387827891548 7.135508652952511 13.502781670325929 C 7.046429671072676 13.34917551276031 6.999854473823283 13.174622532625028 7.00056819980632 12.997057309975206 L 7.00056819980632 7.003578236772546 Z M 20.00160026550293 10.000800132751465 C 20.00160026550293 15.524090163190884 15.524090163190884 20.00160026550293 10.000800132751465 20.00160026550293 C 4.477510579187356 20.00160026550293 0 15.524090163190884 0 10.000800132751465 C 0 4.477510579187356 4.477510579187356 0 10.000800132751465 0 C 15.524090163190884 0 20.00160026550293 4.477510579187356 20.00160026550293 10.000800132751465 Z"
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    id: preview_footer

                    x: 6
                    y: 120

                    height: 17
                    width: 170

                    color: "transparent"

                    Text {
                        id: main_Output

                        x: 4
                        y: 2

                        height: 13
                        width: 66

                        color: "#e2e8f0"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Main Output")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: settings

                        x: 154
                        y: 2.50

                        height: 12
                        width: 12

                        clip: true
                        color: "transparent"

                        Shape {
                            id: _vector_1

                            x: 1.52
                            y: 1.01

                            height: 9.98
                            width: 8.96

                            ShapePath {
                                id: _vector_1_ShapePath0

                                fillColor: "#00000000"
                                strokeColor: "#5c6475"
                                strokeWidth: 2

                                PathSvg {
                                    id: _vector_1_ShapePath0_PathSvg0

                                    path: "M 3.3136794567108154 1.0592867136001587 C 3.341228762641549 0.7694565653800964 3.475845903158188 0.5003089904785156 3.691230535507202 0.30442655086517334 C 3.9066151678562164 0.10854411125183105 4.187292814254761 -2.2204460492503096e-16 4.478429317474365 3.3476706134016988e-31 C 4.76956582069397 -2.2204460492503096e-16 5.050244182348251 0.10854411125183105 5.265628814697266 0.30442655086517334 C 5.48101344704628 0.5003089904785156 5.6156301107257605 0.7694565653800964 5.643179416656494 1.0592867136001587 C 5.659737911075354 1.2465139627456665 5.7211599573493 1.4269959926605225 5.822246551513672 1.5854564905166626 C 5.923333145678043 1.7439169883728027 6.061108812689781 1.875691182911396 6.223911285400391 1.9696251153945923 C 6.386713758111 2.0635590478777885 6.569751054048538 2.1168875070288777 6.7575297355651855 2.1250967979431152 C 6.945308417081833 2.1333060888573527 7.132300674915314 2.0961545184254646 7.302679538726807 2.016786575317383 C 7.567234843969345 1.8966757655143738 7.867016524076462 1.8792960420250893 8.14367961883545 1.9680296182632446 C 8.420342713594437 2.0567631945014 8.654093876481056 2.2452619075775146 8.7994384765625 2.4968390464782715 C 8.944783076643944 2.7484161853790283 8.99132301658392 3.0450726449489594 8.930000305175781 3.3290719985961914 C 8.868677593767643 3.6130713522434235 8.7038793861866 3.8640945106744766 8.467679977416992 4.0332865715026855 C 8.313869968056679 4.141210205852985 8.188315153121948 4.284590631723404 8.101635932922363 4.45129919052124 C 8.014956712722778 4.6180077493190765 7.969702243804932 4.803140565752983 7.969702243804932 4.991036891937256 C 7.969702243804932 5.178933218121529 8.014956712722778 5.364065557718277 8.101635932922363 5.530774116516113 C 8.188315153121948 5.69748267531395 8.313869968056679 5.840863101184368 8.467679977416992 5.948786735534668 C 8.7038793861866 6.117978796362877 8.868677593767643 6.36900195479393 8.930000305175781 6.653001308441162 C 8.99132301658392 6.937000662088394 8.944783076643944 7.233657598495483 8.7994384765625 7.48523473739624 C 8.654093876481056 7.736811876296997 8.420342713594437 7.925310231745243 8.14367961883545 8.014043807983398 C 7.867016524076462 8.102777384221554 7.567234843969345 8.08539754152298 7.302679538726807 7.965286731719971 C 7.132300674915314 7.885918788611889 6.945308417081833 7.848766741342843 6.7575297355651855 7.85697603225708 C 6.569751054048538 7.865185323171318 6.386713758111 7.918513424694538 6.223911285400391 8.012447357177734 C 6.061108812689781 8.10638128966093 5.923333145678043 8.238155484199524 5.822246551513672 8.396615982055664 C 5.7211599573493 8.555076479911804 5.659737911075354 8.735559463500977 5.643179416656494 8.922786712646484 C 5.6156301107257605 9.212616860866547 5.48101344704628 9.481764197349548 5.265628814697266 9.67764663696289 C 5.050244182348251 9.873529076576233 4.76956582069397 9.982072830200195 4.478429317474365 9.982072830200195 C 4.187292814254761 9.982072830200195 3.9066151678562164 9.873529076576233 3.691230535507202 9.67764663696289 C 3.475845903158188 9.481764197349548 3.341228762641549 9.212616860866547 3.3136794567108154 8.922786712646484 C 3.2971513122320175 8.73549273610115 3.2357283383607864 8.554940730333328 3.1346123218536377 8.39642333984375 C 3.033496305346489 8.237905949354172 2.895665928721428 8.106092482805252 2.7327959537506104 8.012147903442383 C 2.569925978779793 7.9182033240795135 2.386813923716545 7.864894911646843 2.1989691257476807 7.8567376136779785 C 2.0111243277788162 7.848580315709114 1.8240801990032196 7.8858146741986275 1.6536794900894165 7.965286731719971 C 1.389124184846878 8.08539754152298 1.0893427431583405 8.102777384221554 0.812679648399353 8.014043807983398 C 0.5360165536403656 7.925310231745243 0.30226586759090424 7.736811876296997 0.15692126750946045 7.48523473739624 C 0.011576667428016663 7.233657598495483 -0.03496314585208893 6.937000662088394 0.026359565556049347 6.653001308441162 C 0.08768227696418762 6.36900195479393 0.2524801194667816 6.117978796362877 0.48867952823638916 5.948786735534668 C 0.6424895375967026 5.840863101184368 0.7680445909500122 5.69748267531395 0.8547238111495972 5.530774116516113 C 0.9414030313491821 5.364065557718277 0.9866565465927128 5.178933218121529 0.9866565465927124 4.991036891937256 C 0.9866565465927128 4.803140565752983 0.9414030313491821 4.6180077493190765 0.8547238111495972 4.45129919052124 C 0.7680445909500122 4.284590631723404 0.6424895375967026 4.141210205852985 0.48867952823638916 4.0332865715026855 C 0.2528117001056671 3.8640093207359314 0.08830472454428673 3.613083988428116 0.027130253612995148 3.329277515411377 C -0.03404421731829643 3.045471042394638 0.012483805418014526 2.749056786298752 0.15767168998718262 2.497642993927002 C 0.3028595745563507 2.246229201555252 0.5363357961177826 2.0577754229307175 0.8127247095108032 1.9689069986343384 C 1.0891136229038239 1.8800385743379593 1.3886715173721313 1.8971039205789566 1.6531795263290405 2.016786575317383 C 1.8235583901405334 2.0961545184254646 2.010551244020462 2.1333060888573527 2.1983299255371094 2.1250967979431152 C 2.3861086070537567 2.1168875070288777 2.5691456645727158 2.0635590478777885 2.731948137283325 1.9696251153945923 C 2.8947506099939346 1.875691182911396 3.0325258001685143 1.7439169883728027 3.1336123943328857 1.5854564905166626 C 3.2346989884972572 1.4269959926605225 3.296121034771204 1.2465139627456665 3.3126795291900635 1.0592867136001587 M 5.97802734375 4.9912109375 C 5.97802734375 5.819638013839722 5.306454420089722 6.4912109375 4.47802734375 6.4912109375 C 3.6496002078056335 6.4912109375 2.97802734375 5.819638013839722 2.97802734375 4.9912109375 C 2.97802734375 4.1627838015556335 3.6496002078056335 3.4912109375 4.47802734375 3.4912109375 C 5.306454420089722 3.4912109375 5.97802734375 4.1627838015556335 5.97802734375 4.9912109375 Z"
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                id: preview_Stage_Screen

                x: 194

                height: 143
                width: 182

                border.color: "#232530"
                border.width: 1
                color: "#16171e"
                radius: 8

                Rectangle {
                    id: preview_screen_1

                    x: 6
                    y: 6

                    height: 110
                    width: 170

                    clip: true
                    color: "transparent"
                    radius: 4

                    Rectangle {
                        id: checkered_grid_1

                        height: 110
                        width: 170

                        color: "#101116"

                        Rectangle {
                            id: rectangle_48

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_49

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_50

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_51

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_52

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_53

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_54

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_55

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_56

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_57

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_58

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_59

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_60

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_61

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_62

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_63

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_64

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_65

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_66

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_67

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_68

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_69

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_70

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_71

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_72

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_73

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_74

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_75

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_76

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_77

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_78

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_79

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_80

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_81

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_82

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_83

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_84

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_85

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_86

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_87

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_88

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_89

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_90

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_91

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_92

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_93

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_94

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_95

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                    }
                    Rectangle {
                        id: frame_2

                        x: 6
                        y: 6

                        height: 16
                        width: 53

                        color: "#b3000000"
                        radius: 4

                        Text {
                            id: sTAGE_1

                            x: 6
                            y: 2

                            height: 12
                            width: 42

                            color: "#e2e8f0"
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("STAGE 1")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                }
                Rectangle {
                    id: preview_footer_1

                    x: 6
                    y: 120

                    height: 17
                    width: 170

                    color: "transparent"

                    Text {
                        id: stage_Screen

                        x: 4
                        y: 2

                        height: 13
                        width: 72

                        color: "#e2e8f0"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Stage Screen")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: settings_1

                        x: 154
                        y: 2.50

                        height: 12
                        width: 12

                        clip: true
                        color: "transparent"

                        Shape {
                            id: _vector_2

                            x: 1.52
                            y: 1.01

                            height: 9.98
                            width: 8.96

                            ShapePath {
                                id: _vector_2_ShapePath0

                                fillColor: "#00000000"
                                strokeColor: "#5c6475"
                                strokeWidth: 2

                                PathSvg {
                                    id: _vector_2_ShapePath0_PathSvg0

                                    path: "M 3.3136794567108154 1.0592867136001587 C 3.341228762641549 0.7694565653800964 3.475845903158188 0.5003089904785156 3.691230535507202 0.30442655086517334 C 3.9066151678562164 0.10854411125183105 4.187292814254761 -2.2204460492503096e-16 4.478429317474365 3.3476706134016988e-31 C 4.76956582069397 -2.2204460492503096e-16 5.050244182348251 0.10854411125183105 5.265628814697266 0.30442655086517334 C 5.48101344704628 0.5003089904785156 5.6156301107257605 0.7694565653800964 5.643179416656494 1.0592867136001587 C 5.659737911075354 1.2465139627456665 5.7211599573493 1.4269959926605225 5.822246551513672 1.5854564905166626 C 5.923333145678043 1.7439169883728027 6.061108812689781 1.875691182911396 6.223911285400391 1.9696251153945923 C 6.386713758111 2.0635590478777885 6.569751054048538 2.1168875070288777 6.7575297355651855 2.1250967979431152 C 6.945308417081833 2.1333060888573527 7.132300674915314 2.0961545184254646 7.302679538726807 2.016786575317383 C 7.567234843969345 1.8966757655143738 7.867016524076462 1.8792960420250893 8.14367961883545 1.9680296182632446 C 8.420342713594437 2.0567631945014 8.654093876481056 2.2452619075775146 8.7994384765625 2.4968390464782715 C 8.944783076643944 2.7484161853790283 8.99132301658392 3.0450726449489594 8.930000305175781 3.3290719985961914 C 8.868677593767643 3.6130713522434235 8.7038793861866 3.8640945106744766 8.467679977416992 4.0332865715026855 C 8.313869968056679 4.141210205852985 8.188315153121948 4.284590631723404 8.101635932922363 4.45129919052124 C 8.014956712722778 4.6180077493190765 7.969702243804932 4.803140565752983 7.969702243804932 4.991036891937256 C 7.969702243804932 5.178933218121529 8.014956712722778 5.364065557718277 8.101635932922363 5.530774116516113 C 8.188315153121948 5.69748267531395 8.313869968056679 5.840863101184368 8.467679977416992 5.948786735534668 C 8.7038793861866 6.117978796362877 8.868677593767643 6.36900195479393 8.930000305175781 6.653001308441162 C 8.99132301658392 6.937000662088394 8.944783076643944 7.233657598495483 8.7994384765625 7.48523473739624 C 8.654093876481056 7.736811876296997 8.420342713594437 7.925310231745243 8.14367961883545 8.014043807983398 C 7.867016524076462 8.102777384221554 7.567234843969345 8.08539754152298 7.302679538726807 7.965286731719971 C 7.132300674915314 7.885918788611889 6.945308417081833 7.848766741342843 6.7575297355651855 7.85697603225708 C 6.569751054048538 7.865185323171318 6.386713758111 7.918513424694538 6.223911285400391 8.012447357177734 C 6.061108812689781 8.10638128966093 5.923333145678043 8.238155484199524 5.822246551513672 8.396615982055664 C 5.7211599573493 8.555076479911804 5.659737911075354 8.735559463500977 5.643179416656494 8.922786712646484 C 5.6156301107257605 9.212616860866547 5.48101344704628 9.481764197349548 5.265628814697266 9.67764663696289 C 5.050244182348251 9.873529076576233 4.76956582069397 9.982072830200195 4.478429317474365 9.982072830200195 C 4.187292814254761 9.982072830200195 3.9066151678562164 9.873529076576233 3.691230535507202 9.67764663696289 C 3.475845903158188 9.481764197349548 3.341228762641549 9.212616860866547 3.3136794567108154 8.922786712646484 C 3.2971513122320175 8.73549273610115 3.2357283383607864 8.554940730333328 3.1346123218536377 8.39642333984375 C 3.033496305346489 8.237905949354172 2.895665928721428 8.106092482805252 2.7327959537506104 8.012147903442383 C 2.569925978779793 7.9182033240795135 2.386813923716545 7.864894911646843 2.1989691257476807 7.8567376136779785 C 2.0111243277788162 7.848580315709114 1.8240801990032196 7.8858146741986275 1.6536794900894165 7.965286731719971 C 1.389124184846878 8.08539754152298 1.0893427431583405 8.102777384221554 0.812679648399353 8.014043807983398 C 0.5360165536403656 7.925310231745243 0.30226586759090424 7.736811876296997 0.15692126750946045 7.48523473739624 C 0.011576667428016663 7.233657598495483 -0.03496314585208893 6.937000662088394 0.026359565556049347 6.653001308441162 C 0.08768227696418762 6.36900195479393 0.2524801194667816 6.117978796362877 0.48867952823638916 5.948786735534668 C 0.6424895375967026 5.840863101184368 0.7680445909500122 5.69748267531395 0.8547238111495972 5.530774116516113 C 0.9414030313491821 5.364065557718277 0.9866565465927128 5.178933218121529 0.9866565465927124 4.991036891937256 C 0.9866565465927128 4.803140565752983 0.9414030313491821 4.6180077493190765 0.8547238111495972 4.45129919052124 C 0.7680445909500122 4.284590631723404 0.6424895375967026 4.141210205852985 0.48867952823638916 4.0332865715026855 C 0.2528117001056671 3.8640093207359314 0.08830472454428673 3.613083988428116 0.027130253612995148 3.329277515411377 C -0.03404421731829643 3.045471042394638 0.012483805418014526 2.749056786298752 0.15767168998718262 2.497642993927002 C 0.3028595745563507 2.246229201555252 0.5363357961177826 2.0577754229307175 0.8127247095108032 1.9689069986343384 C 1.0891136229038239 1.8800385743379593 1.3886715173721313 1.8971039205789566 1.6531795263290405 2.016786575317383 C 1.8235583901405334 2.0961545184254646 2.010551244020462 2.1333060888573527 2.1983299255371094 2.1250967979431152 C 2.3861086070537567 2.1168875070288777 2.5691456645727158 2.0635590478777885 2.731948137283325 1.9696251153945923 C 2.8947506099939346 1.875691182911396 3.0325258001685143 1.7439169883728027 3.1336123943328857 1.5854564905166626 C 3.2346989884972572 1.4269959926605225 3.296121034771204 1.2465139627456665 3.3126795291900635 1.0592867136001587 M 5.97802734375 4.9912109375 C 5.97802734375 5.819638013839722 5.306454420089722 6.4912109375 4.47802734375 6.4912109375 C 3.6496002078056335 6.4912109375 2.97802734375 5.819638013839722 2.97802734375 4.9912109375 C 2.97802734375 4.1627838015556335 3.6496002078056335 3.4912109375 4.47802734375 3.4912109375 C 5.306454420089722 3.4912109375 5.97802734375 4.1627838015556335 5.97802734375 4.9912109375 Z"
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            id: preview_row_2

            x: 12
            y: 167

            height: 143
            width: 376

            color: "transparent"

            Rectangle {
                id: preview_Nursery_Display

                height: 143
                width: 182

                border.color: "#232530"
                border.width: 1
                color: "#16171e"
                radius: 8

                Rectangle {
                    id: preview_screen_2

                    x: 6
                    y: 6

                    height: 110
                    width: 170

                    clip: true
                    color: "transparent"
                    radius: 4

                    Rectangle {
                        id: checkered_grid_2

                        height: 110
                        width: 170

                        color: "#101116"

                        Rectangle {
                            id: rectangle_96

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_97

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_98

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_99

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_100

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_101

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_102

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_103

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_104

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_105

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_106

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_107

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_108

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_109

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_110

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_111

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_112

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_113

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_114

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_115

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_116

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_117

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_118

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_119

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_120

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_121

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_122

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_123

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_124

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_125

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_126

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_127

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_128

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_129

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_130

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_131

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_132

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_133

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_134

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_135

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_136

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_137

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_138

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_139

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_140

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_141

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_142

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_143

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                    }
                    Rectangle {
                        id: frame_3

                        x: 6
                        y: 6

                        height: 16
                        width: 60

                        color: "#b3000000"
                        radius: 4

                        Text {
                            id: nURSERY

                            x: 6
                            y: 2

                            height: 12
                            width: 49

                            color: "#e2e8f0"
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("NURSERY")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                }
                Rectangle {
                    id: preview_footer_2

                    x: 6
                    y: 120

                    height: 17
                    width: 170

                    color: "transparent"

                    Text {
                        id: nursery_Display

                        x: 4
                        y: 2

                        height: 13
                        width: 85

                        color: "#e2e8f0"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Nursery Display")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: settings_2

                        x: 154
                        y: 2.50

                        height: 12
                        width: 12

                        clip: true
                        color: "transparent"

                        Shape {
                            id: _vector_3

                            x: 1.52
                            y: 1.01

                            height: 9.98
                            width: 8.96

                            ShapePath {
                                id: _vector_3_ShapePath0

                                fillColor: "#00000000"
                                strokeColor: "#5c6475"
                                strokeWidth: 2

                                PathSvg {
                                    id: _vector_3_ShapePath0_PathSvg0

                                    path: "M 3.3136794567108154 1.0592867136001587 C 3.341228762641549 0.7694565653800964 3.475845903158188 0.5003089904785156 3.691230535507202 0.30442655086517334 C 3.9066151678562164 0.10854411125183105 4.187292814254761 -2.2204460492503096e-16 4.478429317474365 3.3476706134016988e-31 C 4.76956582069397 -2.2204460492503096e-16 5.050244182348251 0.10854411125183105 5.265628814697266 0.30442655086517334 C 5.48101344704628 0.5003089904785156 5.6156301107257605 0.7694565653800964 5.643179416656494 1.0592867136001587 C 5.659737911075354 1.2465139627456665 5.7211599573493 1.4269959926605225 5.822246551513672 1.5854564905166626 C 5.923333145678043 1.7439169883728027 6.061108812689781 1.875691182911396 6.223911285400391 1.9696251153945923 C 6.386713758111 2.0635590478777885 6.569751054048538 2.1168875070288777 6.7575297355651855 2.1250967979431152 C 6.945308417081833 2.1333060888573527 7.132300674915314 2.0961545184254646 7.302679538726807 2.016786575317383 C 7.567234843969345 1.8966757655143738 7.867016524076462 1.8792960420250893 8.14367961883545 1.9680296182632446 C 8.420342713594437 2.0567631945014 8.654093876481056 2.2452619075775146 8.7994384765625 2.4968390464782715 C 8.944783076643944 2.7484161853790283 8.99132301658392 3.0450726449489594 8.930000305175781 3.3290719985961914 C 8.868677593767643 3.6130713522434235 8.7038793861866 3.8640945106744766 8.467679977416992 4.0332865715026855 C 8.313869968056679 4.141210205852985 8.188315153121948 4.284590631723404 8.101635932922363 4.45129919052124 C 8.014956712722778 4.6180077493190765 7.969702243804932 4.803140565752983 7.969702243804932 4.991036891937256 C 7.969702243804932 5.178933218121529 8.014956712722778 5.364065557718277 8.101635932922363 5.530774116516113 C 8.188315153121948 5.69748267531395 8.313869968056679 5.840863101184368 8.467679977416992 5.948786735534668 C 8.7038793861866 6.117978796362877 8.868677593767643 6.36900195479393 8.930000305175781 6.653001308441162 C 8.99132301658392 6.937000662088394 8.944783076643944 7.233657598495483 8.7994384765625 7.48523473739624 C 8.654093876481056 7.736811876296997 8.420342713594437 7.925310231745243 8.14367961883545 8.014043807983398 C 7.867016524076462 8.102777384221554 7.567234843969345 8.08539754152298 7.302679538726807 7.965286731719971 C 7.132300674915314 7.885918788611889 6.945308417081833 7.848766741342843 6.7575297355651855 7.85697603225708 C 6.569751054048538 7.865185323171318 6.386713758111 7.918513424694538 6.223911285400391 8.012447357177734 C 6.061108812689781 8.10638128966093 5.923333145678043 8.238155484199524 5.822246551513672 8.396615982055664 C 5.7211599573493 8.555076479911804 5.659737911075354 8.735559463500977 5.643179416656494 8.922786712646484 C 5.6156301107257605 9.212616860866547 5.48101344704628 9.481764197349548 5.265628814697266 9.67764663696289 C 5.050244182348251 9.873529076576233 4.76956582069397 9.982072830200195 4.478429317474365 9.982072830200195 C 4.187292814254761 9.982072830200195 3.9066151678562164 9.873529076576233 3.691230535507202 9.67764663696289 C 3.475845903158188 9.481764197349548 3.341228762641549 9.212616860866547 3.3136794567108154 8.922786712646484 C 3.2971513122320175 8.73549273610115 3.2357283383607864 8.554940730333328 3.1346123218536377 8.39642333984375 C 3.033496305346489 8.237905949354172 2.895665928721428 8.106092482805252 2.7327959537506104 8.012147903442383 C 2.569925978779793 7.9182033240795135 2.386813923716545 7.864894911646843 2.1989691257476807 7.8567376136779785 C 2.0111243277788162 7.848580315709114 1.8240801990032196 7.8858146741986275 1.6536794900894165 7.965286731719971 C 1.389124184846878 8.08539754152298 1.0893427431583405 8.102777384221554 0.812679648399353 8.014043807983398 C 0.5360165536403656 7.925310231745243 0.30226586759090424 7.736811876296997 0.15692126750946045 7.48523473739624 C 0.011576667428016663 7.233657598495483 -0.03496314585208893 6.937000662088394 0.026359565556049347 6.653001308441162 C 0.08768227696418762 6.36900195479393 0.2524801194667816 6.117978796362877 0.48867952823638916 5.948786735534668 C 0.6424895375967026 5.840863101184368 0.7680445909500122 5.69748267531395 0.8547238111495972 5.530774116516113 C 0.9414030313491821 5.364065557718277 0.9866565465927128 5.178933218121529 0.9866565465927124 4.991036891937256 C 0.9866565465927128 4.803140565752983 0.9414030313491821 4.6180077493190765 0.8547238111495972 4.45129919052124 C 0.7680445909500122 4.284590631723404 0.6424895375967026 4.141210205852985 0.48867952823638916 4.0332865715026855 C 0.2528117001056671 3.8640093207359314 0.08830472454428673 3.613083988428116 0.027130253612995148 3.329277515411377 C -0.03404421731829643 3.045471042394638 0.012483805418014526 2.749056786298752 0.15767168998718262 2.497642993927002 C 0.3028595745563507 2.246229201555252 0.5363357961177826 2.0577754229307175 0.8127247095108032 1.9689069986343384 C 1.0891136229038239 1.8800385743379593 1.3886715173721313 1.8971039205789566 1.6531795263290405 2.016786575317383 C 1.8235583901405334 2.0961545184254646 2.010551244020462 2.1333060888573527 2.1983299255371094 2.1250967979431152 C 2.3861086070537567 2.1168875070288777 2.5691456645727158 2.0635590478777885 2.731948137283325 1.9696251153945923 C 2.8947506099939346 1.875691182911396 3.0325258001685143 1.7439169883728027 3.1336123943328857 1.5854564905166626 C 3.2346989884972572 1.4269959926605225 3.296121034771204 1.2465139627456665 3.3126795291900635 1.0592867136001587 M 5.97802734375 4.9912109375 C 5.97802734375 5.819638013839722 5.306454420089722 6.4912109375 4.47802734375 6.4912109375 C 3.6496002078056335 6.4912109375 2.97802734375 5.819638013839722 2.97802734375 4.9912109375 C 2.97802734375 4.1627838015556335 3.6496002078056335 3.4912109375 4.47802734375 3.4912109375 C 5.306454420089722 3.4912109375 5.97802734375 4.1627838015556335 5.97802734375 4.9912109375 Z"
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                id: preview_Stream_Overlay

                x: 194

                height: 143
                width: 182

                border.color: "#232530"
                border.width: 1
                color: "#16171e"
                radius: 8

                Rectangle {
                    id: preview_screen_3

                    x: 6
                    y: 6

                    height: 110
                    width: 170

                    clip: true
                    color: "transparent"
                    radius: 4

                    Rectangle {
                        id: checkered_grid_3

                        height: 110
                        width: 170

                        color: "#101116"

                        Rectangle {
                            id: rectangle_144

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_145

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_146

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_147

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_148

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_149

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_150

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_151

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_152

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_153

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_154

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_155

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_156

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_157

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_158

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_159

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_160

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_161

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_162

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_163

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_164

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_165

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_166

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_167

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_168

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_169

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_170

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_171

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_172

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_173

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_174

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_175

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_176

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_177

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_178

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_179

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_180

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_181

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_182

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_183

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_184

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_185

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_186

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_187

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_188

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_189

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                        Rectangle {
                            id: rectangle_190

                            height: 110
                            width: 170

                            color: "#101116"
                        }
                        Rectangle {
                            id: rectangle_191

                            height: 110
                            width: 170

                            color: "#1a1b22"
                        }
                    }
                    Rectangle {
                        id: frame_4

                        x: 6
                        y: 6

                        height: 16
                        width: 61

                        color: "#b3000000"
                        radius: 4

                        Text {
                            id: oBS_FEED

                            x: 6
                            y: 2

                            height: 12
                            width: 50

                            color: "#e2e8f0"
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("OBS FEED")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                }
                Rectangle {
                    id: preview_footer_3

                    x: 6
                    y: 120

                    height: 17
                    width: 170

                    color: "transparent"

                    Text {
                        id: stream_Overlay

                        x: 4
                        y: 2

                        height: 13
                        width: 82

                        color: "#e2e8f0"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Stream Overlay")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: settings_3

                        x: 154
                        y: 2.50

                        height: 12
                        width: 12

                        clip: true
                        color: "transparent"

                        Shape {
                            id: _vector_4

                            x: 1.52
                            y: 1.01

                            height: 9.98
                            width: 8.96

                            ShapePath {
                                id: _vector_4_ShapePath0

                                fillColor: "#00000000"
                                strokeColor: "#5c6475"
                                strokeWidth: 2

                                PathSvg {
                                    id: _vector_4_ShapePath0_PathSvg0

                                    path: "M 3.3136794567108154 1.0592867136001587 C 3.341228762641549 0.7694565653800964 3.475845903158188 0.5003089904785156 3.691230535507202 0.30442655086517334 C 3.9066151678562164 0.10854411125183105 4.187292814254761 -2.2204460492503096e-16 4.478429317474365 3.3476706134016988e-31 C 4.76956582069397 -2.2204460492503096e-16 5.050244182348251 0.10854411125183105 5.265628814697266 0.30442655086517334 C 5.48101344704628 0.5003089904785156 5.6156301107257605 0.7694565653800964 5.643179416656494 1.0592867136001587 C 5.659737911075354 1.2465139627456665 5.7211599573493 1.4269959926605225 5.822246551513672 1.5854564905166626 C 5.923333145678043 1.7439169883728027 6.061108812689781 1.875691182911396 6.223911285400391 1.9696251153945923 C 6.386713758111 2.0635590478777885 6.569751054048538 2.1168875070288777 6.7575297355651855 2.1250967979431152 C 6.945308417081833 2.1333060888573527 7.132300674915314 2.0961545184254646 7.302679538726807 2.016786575317383 C 7.567234843969345 1.8966757655143738 7.867016524076462 1.8792960420250893 8.14367961883545 1.9680296182632446 C 8.420342713594437 2.0567631945014 8.654093876481056 2.2452619075775146 8.7994384765625 2.4968390464782715 C 8.944783076643944 2.7484161853790283 8.99132301658392 3.0450726449489594 8.930000305175781 3.3290719985961914 C 8.868677593767643 3.6130713522434235 8.7038793861866 3.8640945106744766 8.467679977416992 4.0332865715026855 C 8.313869968056679 4.141210205852985 8.188315153121948 4.284590631723404 8.101635932922363 4.45129919052124 C 8.014956712722778 4.6180077493190765 7.969702243804932 4.803140565752983 7.969702243804932 4.991036891937256 C 7.969702243804932 5.178933218121529 8.014956712722778 5.364065557718277 8.101635932922363 5.530774116516113 C 8.188315153121948 5.69748267531395 8.313869968056679 5.840863101184368 8.467679977416992 5.948786735534668 C 8.7038793861866 6.117978796362877 8.868677593767643 6.36900195479393 8.930000305175781 6.653001308441162 C 8.99132301658392 6.937000662088394 8.944783076643944 7.233657598495483 8.7994384765625 7.48523473739624 C 8.654093876481056 7.736811876296997 8.420342713594437 7.925310231745243 8.14367961883545 8.014043807983398 C 7.867016524076462 8.102777384221554 7.567234843969345 8.08539754152298 7.302679538726807 7.965286731719971 C 7.132300674915314 7.885918788611889 6.945308417081833 7.848766741342843 6.7575297355651855 7.85697603225708 C 6.569751054048538 7.865185323171318 6.386713758111 7.918513424694538 6.223911285400391 8.012447357177734 C 6.061108812689781 8.10638128966093 5.923333145678043 8.238155484199524 5.822246551513672 8.396615982055664 C 5.7211599573493 8.555076479911804 5.659737911075354 8.735559463500977 5.643179416656494 8.922786712646484 C 5.6156301107257605 9.212616860866547 5.48101344704628 9.481764197349548 5.265628814697266 9.67764663696289 C 5.050244182348251 9.873529076576233 4.76956582069397 9.982072830200195 4.478429317474365 9.982072830200195 C 4.187292814254761 9.982072830200195 3.9066151678562164 9.873529076576233 3.691230535507202 9.67764663696289 C 3.475845903158188 9.481764197349548 3.341228762641549 9.212616860866547 3.3136794567108154 8.922786712646484 C 3.2971513122320175 8.73549273610115 3.2357283383607864 8.554940730333328 3.1346123218536377 8.39642333984375 C 3.033496305346489 8.237905949354172 2.895665928721428 8.106092482805252 2.7327959537506104 8.012147903442383 C 2.569925978779793 7.9182033240795135 2.386813923716545 7.864894911646843 2.1989691257476807 7.8567376136779785 C 2.0111243277788162 7.848580315709114 1.8240801990032196 7.8858146741986275 1.6536794900894165 7.965286731719971 C 1.389124184846878 8.08539754152298 1.0893427431583405 8.102777384221554 0.812679648399353 8.014043807983398 C 0.5360165536403656 7.925310231745243 0.30226586759090424 7.736811876296997 0.15692126750946045 7.48523473739624 C 0.011576667428016663 7.233657598495483 -0.03496314585208893 6.937000662088394 0.026359565556049347 6.653001308441162 C 0.08768227696418762 6.36900195479393 0.2524801194667816 6.117978796362877 0.48867952823638916 5.948786735534668 C 0.6424895375967026 5.840863101184368 0.7680445909500122 5.69748267531395 0.8547238111495972 5.530774116516113 C 0.9414030313491821 5.364065557718277 0.9866565465927128 5.178933218121529 0.9866565465927124 4.991036891937256 C 0.9866565465927128 4.803140565752983 0.9414030313491821 4.6180077493190765 0.8547238111495972 4.45129919052124 C 0.7680445909500122 4.284590631723404 0.6424895375967026 4.141210205852985 0.48867952823638916 4.0332865715026855 C 0.2528117001056671 3.8640093207359314 0.08830472454428673 3.613083988428116 0.027130253612995148 3.329277515411377 C -0.03404421731829643 3.045471042394638 0.012483805418014526 2.749056786298752 0.15767168998718262 2.497642993927002 C 0.3028595745563507 2.246229201555252 0.5363357961177826 2.0577754229307175 0.8127247095108032 1.9689069986343384 C 1.0891136229038239 1.8800385743379593 1.3886715173721313 1.8971039205789566 1.6531795263290405 2.016786575317383 C 1.8235583901405334 2.0961545184254646 2.010551244020462 2.1333060888573527 2.1983299255371094 2.1250967979431152 C 2.3861086070537567 2.1168875070288777 2.5691456645727158 2.0635590478777885 2.731948137283325 1.9696251153945923 C 2.8947506099939346 1.875691182911396 3.0325258001685143 1.7439169883728027 3.1336123943328857 1.5854564905166626 C 3.2346989884972572 1.4269959926605225 3.296121034771204 1.2465139627456665 3.3126795291900635 1.0592867136001587 M 5.97802734375 4.9912109375 C 5.97802734375 5.819638013839722 5.306454420089722 6.4912109375 4.47802734375 6.4912109375 C 3.6496002078056335 6.4912109375 2.97802734375 5.819638013839722 2.97802734375 4.9912109375 C 2.97802734375 4.1627838015556335 3.6496002078056335 3.4912109375 4.47802734375 3.4912109375 C 5.306454420089722 3.4912109375 5.97802734375 4.1627838015556335 5.97802734375 4.9912109375 Z"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Rectangle {
        id: rp_row0_change

        x: 1346
        y: 465

        height: 24
        width: 60

        border.color: "#2a3140"
        border.width: 1
        color: "#1a1c26"
        radius: 12
    }
    Text {
        id: rp_row0_change_t

        x: 1357
        y: 471

        height: 12
        width: 39

        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Change")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_scroll_track

        x: 272
        y: 100

        height: 800
        width: 4

        color: "#161823"
        radius: 2
    }
    Rectangle {
        id: so_scroll_thumb

        x: 272
        y: 100

        height: 470
        width: 4

        color: "#3a3f4d"
        radius: 2
    }
    Text {
        id: so_ic_t

        x: 32
        y: 70

        height: 11
        width: 20

        color: "#e2e8f0"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("VGR")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: so_title

        x: 62
        y: 61

        height: 15
        width: 93

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Sunday Service")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: so_sub

        x: 62
        y: 80

        height: 11
        width: 120

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("12 slides · Worship template")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: so_chev

        x: 248
        y: 72

        height: 13
        width: 7

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("⌄")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Image {
        id: so_search_ic

        x: 26
        y: 137

        source: Qt.resolvedUrl("assets/so_search_ic_3.png")
    }
    Text {
        id: so_search_t

        x: 44
        y: 136

        height: 13
        width: 99

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Search this show…")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_acc_size_row

        x: 1048
        y: 516

        height: 46
        width: 384

        border.color: "#232530"
        border.width: 1
        color: "#13151c"
        radius: 10
    }
    Rectangle {
        id: rp_acc_size_icon_bg

        x: 1060
        y: 528

        height: 24
        width: 24

        color: "#296c5ce7"
        radius: 6
    }
    Rectangle {
        id: rp_acc_size_icon_l1

        x: 1067
        y: 533

        height: 14
        width: 2

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: rp_acc_size_icon_l2

        x: 1075
        y: 533

        height: 14
        width: 2

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: rp_acc_size_icon_l3

        x: 1083
        y: 533

        height: 14
        width: 2

        color: "#6c5ce7"
        radius: 1
    }
    Text {
        id: rp_acc_size_label

        x: 1096
        y: 532

        height: 16
        width: 74

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Size & Style")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: rp_acc_size_chev

        x: 1408
        y: 531

        height: 17
        width: 9

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 14
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("⌄")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_bd_row

        x: 1048
        y: 686

        height: 46
        width: 384

        border.color: "#232530"
        border.width: 1
        color: "#13151c"
        radius: 10
    }
    Text {
        id: rp_bd_row_l

        x: 1062
        y: 700

        height: 16
        width: 43

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Border")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_bd_row_sw

        x: 1304
        y: 695

        height: 24
        width: 24

        color: "#eef0f6"
        radius: 5
    }
    Rectangle {
        id: rp_bd_row_change

        x: 1346
        y: 696

        height: 24
        width: 60

        color: "#1a1c26"
        radius: 12
    }
    Text {
        id: rp_bd_row_change_t

        x: 1357
        y: 703

        height: 12
        width: 38

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Change")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: rp_bd_row_chev

        x: 1408
        y: 701

        height: 17
        width: 7

        color: "#8a8fa3"
        font.family: "Inter"
        font.pixelSize: 14
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("›")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Item {
        id: group

        x: 28
        y: 194

        height: 120
        width: 224

        Rectangle {
            id: so_s0_pv

            height: 120
            width: 224

            border.color: "#262a38"
            border.width: 1
            color: "#0d0f16"
            radius: 8
        }
        Text {
            id: so_s0_pv_tag

            x: 6
            y: 18

            height: 9
            width: 213

            color: "#ff6a5e"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("SUNDAY SERVICE")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s0_pv_title

            x: 6
            y: 32

            height: 16
            width: 213

            color: "#f2f4fa"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("WELCOME HOME")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s0_pv_l1

            x: 6
            y: 54

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("For where two or three gather in my name,")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s0_pv_l2

            x: 6
            y: 68

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("there am I with them.")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s0_pv_ref

            x: 6
            y: 98

            height: 9
            width: 213

            color: "#6b7080"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("— Matthew 18:20")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
    }
    Item {
        id: group_1

        x: 28
        y: 358

        height: 120
        width: 224

        Rectangle {
            id: so_s1_pv

            height: 120
            width: 224

            border.color: "#262a38"
            border.width: 1
            color: "#0d0f16"
            radius: 8
        }
        Text {
            id: so_s1_pv_tag

            x: 6
            y: 18

            height: 9
            width: 213

            color: "#ff8a3d"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("WORSHIP")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s1_pv_title

            x: 6
            y: 32

            height: 14
            width: 213

            color: "#eef1f8"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Great Are You Lord")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s1_pv_l1

            x: 6
            y: 54

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("It's Your breath in our lungs")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s1_pv_l2

            x: 6
            y: 68

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("so we pour out our praise")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s1_pv_l3

            x: 6
            y: 82

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("to You only")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
    }
    Item {
        id: group_2

        x: 28
        y: 522

        height: 120
        width: 224

        Rectangle {
            id: so_s2_pv

            height: 120
            width: 224

            border.color: "#262a38"
            border.width: 1
            color: "#0d0f16"
            radius: 8
        }
        Text {
            id: so_s2_pv_tag

            x: 6
            y: 18

            height: 9
            width: 213

            color: "#ff8a3d"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("WORSHIP")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s2_pv_title

            x: 6
            y: 32

            height: 14
            width: 213

            color: "#eef1f8"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Way Maker")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s2_pv_l1

            x: 6
            y: 54

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("You are the Way Maker")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s2_pv_l2

            x: 6
            y: 68

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Miracle Worker")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s2_pv_l3

            x: 6
            y: 82

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Promise Keeper")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
    }
    Item {
        id: group_3

        x: 28
        y: 686

        height: 120
        width: 224

        Rectangle {
            id: so_s3_pv

            height: 120
            width: 224

            border.color: "#262a38"
            border.width: 1
            color: "#0d0f16"
            radius: 8
        }
        Text {
            id: so_s3_pv_tag

            x: 6
            y: 18

            height: 9
            width: 213

            color: "#f0b73d"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("WORSHIP")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s3_pv_title

            x: 6
            y: 32

            height: 14
            width: 213

            color: "#eef1f8"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Oceans")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Text {
            id: so_s3_pv_l1

            x: 6
            y: 54

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("You call me out upon the waters")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s3_pv_l2

            x: 6
            y: 68

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("the great unknown")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
        Text {
            id: so_s3_pv_l3

            x: 6
            y: 82

            height: 10
            width: 213

            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("where feet may fail")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
        }
    }
    Item {
        id: group_4

        x: 230
        y: 200

        height: 18
        width: 18

        Rectangle {
            id: so_s0_pv_num

            height: 18
            width: 18

            color: "#1c2030"
            radius: 4
        }
        Text {
            id: so_s0_pv_num_t

            x: 4
            y: 3

            height: 11
            width: 11

            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("1")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Item {
        id: group_5

        x: 230
        y: 364

        height: 18
        width: 18

        Rectangle {
            id: so_s1_pv_num

            height: 18
            width: 18

            color: "#1c2030"
            radius: 4
        }
        Text {
            id: so_s1_pv_num_t

            x: 4
            y: 3

            height: 11
            width: 11

            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("2")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Item {
        id: group_6

        x: 230
        y: 528

        height: 18
        width: 18

        Rectangle {
            id: so_s2_pv_num

            height: 18
            width: 18

            color: "#1c2030"
            radius: 4
        }
        Text {
            id: so_s2_pv_num_t

            x: 4
            y: 3

            height: 11
            width: 11

            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("3")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Item {
        id: group_7

        x: 230
        y: 692

        height: 18
        width: 18

        Rectangle {
            id: so_s3_pv_num

            height: 18
            width: 18

            color: "#1c2030"
            radius: 4
        }
        Text {
            id: so_s3_pv_num_t

            x: 4
            y: 3

            height: 11
            width: 11

            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("4")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Rectangle {
        id: rp_pos_editor

        x: 1048
        y: 570

        height: 104
        width: 384

        color: "#161823"
        radius: 8
    }
    Text {
        id: rp_pos_e4_l

        x: 1059
        y: 581

        height: 15
        width: 48

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Padding")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: rp_pos_e4_v

        x: 1385
        y: 584

        height: 12
        width: 14

        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("24")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_pos_e4_tr

        x: 1115
        y: 588

        height: 3
        width: 261

        color: "#2a2f3a"
        radius: 1.50
    }
    Rectangle {
        id: rp_pos_e4_fill

        x: 1115
        y: 588

        height: 3
        width: 178

        color: "#6c5ce7"
        radius: 1.50
    }
    Image {
        id: rp_pos_e4_kb

        x: 1286
        y: 582

        source: Qt.resolvedUrl("assets/rp_pos_e4_kb_2.png")
    }
    Text {
        id: rp_pos_e5_l

        x: 1059
        y: 611

        height: 15
        width: 45

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Opacity")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: rp_pos_e5_v

        x: 1385
        y: 614

        height: 12
        width: 27

        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("100%")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_pos_e5_tr

        x: 1115
        y: 618

        height: 3
        width: 261

        color: "#2a2f3a"
        radius: 1.50
    }
    Rectangle {
        id: rp_pos_e5_fill

        x: 1115
        y: 618

        height: 3
        width: 261

        color: "#6c5ce7"
        radius: 1.50
    }
    Image {
        id: rp_pos_e5_kb

        x: 1369
        y: 612

        source: Qt.resolvedUrl("assets/rp_pos_e5_kb_2.png")
    }
    Text {
        id: rp_pos_e6_l

        x: 1059
        y: 641

        height: 15
        width: 39

        color: "#eef1f8"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Radius")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: rp_pos_e6_v

        x: 1385
        y: 644

        height: 12
        width: 12

        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("12")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_pos_e6_tr

        x: 1115
        y: 648

        height: 3
        width: 261

        color: "#2a2f3a"
        radius: 1.50
    }
    Rectangle {
        id: rp_pos_e6_fill

        x: 1115
        y: 648

        height: 3
        width: 109

        color: "#6c5ce7"
        radius: 1.50
    }
    Image {
        id: rp_pos_e6_kb

        x: 1217
        y: 642

        source: Qt.resolvedUrl("assets/rp_pos_e6_kb_2.png")
    }
    Rectangle {
        id: media_modal_dim

        height: 900
        width: 1440

        color: "#99000000"
    }
    Rectangle {
        id: media_modal_card

        x: 320
        y: 170

        height: 560
        width: 800

        border.color: "#232530"
        border.width: 1
        color: "#13151c"
        radius: 14
    }
    Text {
        id: media_modal_title

        x: 344
        y: 194

        height: 19
        width: 102

        color: "#f1f3f8"
        font.family: "Inter"
        font.pixelSize: 16
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Select Media")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: media_modal_close

        x: 1080
        y: 194

        height: 19
        width: 15

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 16
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("✕")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: media_modal_sub

        x: 344
        y: 220

        height: 15
        width: 258

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Choose an image or video to add to this slide")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: media_tab_all

        x: 344
        y: 248

        height: 28
        width: 56

        border.color: "#6c5ce7"
        border.width: 1
        color: "#296c5ce7"
        radius: 8
    }
    Text {
        id: media_tab_all_t

        x: 344
        y: 256

        height: 15
        width: 57

        color: "#9b8ff5"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("All")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: media_tab_img_t

        x: 412
        y: 256

        height: 15
        width: 57

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Images")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: media_tab_vid_t

        x: 480
        y: 256

        height: 15
        width: 57

        color: "#8a94a6"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Videos")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_search

        x: 840
        y: 246

        height: 32
        width: 256

        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8
    }
    Text {
        id: media_search_t

        x: 860
        y: 254

        height: 13
        width: 81

        color: "#5c6475"
        font.family: "Inter"
        font.pixelSize: 11
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Search media...")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: media_div

        x: 344
        y: 296

        height: 1
        width: 752

        color: "#232530"
    }
    Rectangle {
        id: media_item0_bg

        x: 344
        y: 314

        height: 110
        width: 176

        border.color: "#6c5ce7"
        border.width: 1.50
        color: "#1a2240"
        radius: 8
    }
    Text {
        id: media_item0_lb

        x: 344
        y: 428

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("sunday-bg.jpg")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item1_bg

        x: 536
        y: 314

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Image {
        id: media_item1_playbg

        x: 610
        y: 349

        source: Qt.resolvedUrl("assets/media_item1_playbg.png")
    }
    Text {
        id: media_item1_play

        x: 620
        y: 356

        height: 15
        width: 12

        color: "#ffffff"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("▶")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: media_item1_lb

        x: 536
        y: 428

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("worship-loop.mp4")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item2_bg

        x: 728
        y: 314

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Text {
        id: media_item2_lb

        x: 728
        y: 428

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("cross-image.png")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item3_bg

        x: 920
        y: 314

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Text {
        id: media_item3_lb

        x: 920
        y: 428

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("stage-photo.jpg")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item4_bg

        x: 344
        y: 440

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Image {
        id: media_item4_playbg

        x: 418
        y: 475

        source: Qt.resolvedUrl("assets/media_item4_playbg.png")
    }
    Text {
        id: media_item4_play

        x: 428
        y: 482

        height: 15
        width: 12

        color: "#ffffff"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("▶")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: media_item4_lb

        x: 344
        y: 554

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("choir-video.mp4")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item5_bg

        x: 536
        y: 440

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Image {
        id: media_item5_playbg

        x: 610
        y: 475

        source: Qt.resolvedUrl("assets/media_item5_playbg.png")
    }
    Text {
        id: media_item5_play

        x: 620
        y: 482

        height: 15
        width: 12

        color: "#ffffff"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("▶")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Text {
        id: media_item5_lb

        x: 536
        y: 554

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("logo-loop.mp4")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item6_bg

        x: 728
        y: 440

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Text {
        id: media_item6_lb

        x: 728
        y: 554

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("sunset-bg.jpg")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_item7_bg

        x: 920
        y: 440

        height: 110
        width: 176

        border.color: "#262a38"
        border.width: 1
        color: "#161823"
        radius: 8
    }
    Text {
        id: media_item7_lb

        x: 920
        y: 554

        height: 12
        width: 177

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("title-card.png")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_btn_cancel

        x: 896
        y: 670

        height: 36
        width: 100

        border.color: "#2a2f3a"
        border.width: 1
        color: "#1a1c26"
        radius: 10
    }
    Text {
        id: media_btn_cancel_t

        x: 896
        y: 680

        height: 16
        width: 101

        color: "#c8cdd9"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Cancel")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_btn_insert

        x: 1004
        y: 670

        height: 36
        width: 100

        color: "#6c5ce7"
        radius: 10
    }
    Text {
        id: media_btn_insert_t

        x: 1004
        y: 680

        height: 16
        width: 101

        color: "#ffffff"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Insert")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Rectangle {
        id: media_scroll_track

        x: 1102
        y: 314

        height: 236
        width: 4

        color: "#1a1c26"
        radius: 2
    }
    Rectangle {
        id: media_scroll_thumb

        x: 1102
        y: 314

        height: 120
        width: 4

        color: "#3a3f4d"
        radius: 2
    }
}