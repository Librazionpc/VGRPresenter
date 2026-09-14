import QtQuick
import QtQuick.Shapes

Rectangle {
    id: vGRPresenter_Main_Screen_Edit_Text

    height: 900
    width: 1440

    clip: true
    color: "#0d0e14"

    Rectangle {
        id: hdr_bg

        height: 48
        width: 1440

        color: "#11121a"
    }
    Rectangle {
        id: hdr_border

        y: 47

        height: 1
        width: 1440

        color: "#1e2130"
    }
    Rectangle {
        id: logo_group

        x: 16
        y: 12

        height: 18
        width: 101

        color: "transparent"

        Text {
            id: logo

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
    }
    Rectangle {
        id: menu_items

        x: 154

        height: 48
        width: 182

        color: "transparent"

        Rectangle {
            id: menu_file

            y: 11.50

            height: 25
            width: 39

            color: "transparent"
            radius: 5

            Text {
                id: sys_file

                x: 10
                y: 6

                height: 13
                width: 20

                color: "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("File")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: menu_edit

            x: 43
            y: 11.50

            height: 25
            width: 41

            color: "transparent"
            radius: 5

            Text {
                id: sys_edit

                x: 10
                y: 6

                height: 13
                width: 22

                color: "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Edit")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: menu_view

            x: 88
            y: 11.50

            height: 25
            width: 46

            color: "transparent"
            radius: 5

            Text {
                id: sys_view

                x: 10
                y: 6

                height: 13
                width: 27

                color: "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("View")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: menu_help

            x: 138
            y: 11.50

            height: 25
            width: 44

            color: "transparent"
            radius: 5

            Text {
                id: sys_help

                x: 10
                y: 6

                height: 13
                width: 25

                color: "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Help")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: tabs_bg

        x: 612
        y: 9

        height: 30
        width: 216

        border.color: "#252836"
        border.width: 1
        color: "#181924"
        radius: 9

        Rectangle {
            id: tab_show

            x: 3
            y: 3

            height: 24
            width: 66

            color: "transparent"
            radius: 6

            Rectangle {
                id: tab_show_ic

                x: 10.50
                y: 7.50

                height: 9
                width: 11

                border.color: "#525a72"
                border.width: 1
                color: "#525a72"
                radius: 2
            }
            Text {
                id: tab_show_t

                x: 26.50
                y: 5.50

                height: 13
                width: 30

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Show")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tab_edit

            x: 69
            y: 3

            height: 24
            width: 66

            color: "#6c5ce7"
            radius: 6

            Rectangle {
                id: edit

                x: 15
                y: 7

                height: 10
                width: 10

                clip: true
                color: "transparent"

                Shape {
                    id: _vector

                    x: 1.25
                    y: 0.83

                    height: 7.92
                    width: 7.92

                    ShapePath {
                        id: _vector_ShapePath0

                        fillColor: "#00000000"
                        strokeColor: "#ffffff"
                        strokeWidth: 2

                        PathSvg {
                            id: _vector_ShapePath0_PathSvg0

                            path: "M 3.75 0.4150390625 L 0.8333333333333334 0.4150390625 C 0.6123195836941402 0.41503906250000017 0.4003579542040825 0.5028363938132923 0.24407764275868735 0.6591167052586874 C 0.08779733131329218 0.8153970167040825 1.8503717077085943e-16 1.0273586461941402 0 1.2483723958333335 L 0 7.081705729166667 C 1.8503717077085943e-16 7.3027194788058605 0.08779733131329218 7.5146809096137686 0.24407764275868735 7.670961221059164 C 0.4003579542040825 7.8272415325045595 0.6123195836941402 7.9150390625 0.8333333333333334 7.9150390625 L 6.666666666666667 7.9150390625 C 6.8876804163058605 7.9150390625 7.0996418471137686 7.8272415325045595 7.255922158559164 7.670961221059164 C 7.4122024700045595 7.5146809096137686 7.5 7.3027194788058605 7.5 7.081705729166667 L 7.5 4.1650390625 M 6.406288941701254 0.2588834365208944 C 6.572049247721831 0.09312313050031665 6.796868468324344 1.8503717077085943e-16 7.031288941701254 0 C 7.265709415078164 0 7.490528635680676 0.09312313050031665 7.656288941701254 0.2588834365208944 C 7.822049247721831 0.4246437425414722 7.915172576904295 0.6494629631439846 7.915172576904297 0.8838834365208944 C 7.915172576904295 1.1183039098978043 7.822049247721831 1.3431231305003168 7.656288941701254 1.5088834365208945 L 3.900872866312663 5.264716943105062 C 3.80193492397666 5.3635694831609735 3.6797078823049865 5.435930586730441 3.545455535252889 5.475133260091146 L 2.3483721415201826 5.825133323669434 C 2.312518671775858 5.835590585290144 2.274513319134712 5.836217388665925 2.2383344173431396 5.826948483784994 C 2.202155515551567 5.817679578904063 2.1691337631394467 5.798856159672141 2.142725189526876 5.77244758605957 C 2.116316615914305 5.7460390124469995 2.0974927993180854 5.713016663988432 2.0882238944371543 5.676837762196859 C 2.078954989556223 5.640658860405287 2.079581792932004 5.60265390512844 2.0900390545527143 5.566800435384115 L 2.4400389194488525 4.369716644287109 C 2.479424917449554 4.235571113725503 2.5519282557070255 4.113491053382556 2.6508724689483643 4.014716943105062 L 6.406288941701254 0.2588834365208944 Z"
                        }
                    }
                }
            }
            Text {
                id: tab_edit_t

                x: 30
                y: 5.50

                height: 13
                width: 22

                color: "#ffffff"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Edit")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tab_stage

            x: 135
            y: 3

            height: 24
            width: 66

            color: "transparent"
            radius: 6

            Rectangle {
                id: stage_grid

                x: 10
                y: 7

                height: 10
                width: 10

                color: "transparent"

                Rectangle {
                    id: tab_stage_0

                    height: 4
                    width: 4

                    color: "#525a72"
                }
                Rectangle {
                    id: tab_stage_1

                    x: 6

                    height: 4
                    width: 4

                    color: "#525a72"
                }
                Rectangle {
                    id: tab_stage_2

                    y: 6

                    height: 4
                    width: 4

                    color: "#525a72"
                }
                Rectangle {
                    id: tab_stage_3

                    x: 6
                    y: 6

                    height: 4
                    width: 4

                    color: "#525a72"
                }
            }
            Text {
                id: tab_stage_t

                x: 25
                y: 5.50

                height: 13
                width: 32

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Stage")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: status_pill

        x: 1228
        y: 14

        height: 23
        width: 91

        border.color: "#1e2a1e"
        border.width: 1
        color: "#141820"
        radius: 20

        Image {
            id: stat_dot

            x: 10
            y: 7.50

            source: Qt.resolvedUrl("assets/stat_dot_1.png")
        }
        Text {
            id: stat_txt

            x: 24
            y: 5

            height: 13
            width: 56

            color: "#6b8f7a"
            font.family: "Inter"
            font.pixelSize: 10
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Connected")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Rectangle {
        id: win_controls

        x: 1362
        y: 17

        height: 12
        width: 52

        color: "transparent"

        Image {
            id: win_close

            source: Qt.resolvedUrl("assets/win_close_1.png")
        }
        Image {
            id: win_min

            x: 20

            source: Qt.resolvedUrl("assets/win_min_1.png")
        }
        Image {
            id: win_max

            x: 40

            source: Qt.resolvedUrl("assets/win_max_1.png")
        }
    }
    Rectangle {
        id: m_hdr

        x: 280
        y: 48

        height: 44
        width: 760

        color: "#12131b"

        Rectangle {
            id: m_back

            x: 12
            y: 8

            height: 28
            width: 28

            color: "#1e2130"
            radius: 7

            Text {
                id: m_back_t

                x: 11
                y: 5

                height: 18
                width: 7

                color: "#9ba3bf"
                font.family: "Inter"
                font.pixelSize: 15
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("‹")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Text {
            id: m_show

            x: 48
            y: 14

            height: 16
            width: 99

            color: "#f0f2fa"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Sunday Service")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Rectangle {
            id: m_tpl

            x: 154
            y: 12.50

            height: 19
            width: 105

            border.color: "#3d2232"
            border.width: 1
            color: "#2a1c24"
            radius: 6

            Text {
                id: m_tpl_t

                x: 8
                y: 4

                height: 11
                width: 90

                color: "#f05252"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Template · Worship")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: spacer

            x: 267
            y: 21.50

            height: 1
            width: 270

            color: "transparent"
        }
        Rectangle {
            id: autosaved

            x: 545
            y: 16.50

            height: 11
            width: 61

            color: "transparent"

            Text {
                id: m_saved

                height: 11
                width: 49

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Autosaved")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: element

                x: 52

                height: 11
                width: 10

                color: "#34d399"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("✓")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Text {
            id: m_fit

            x: 614
            y: 16.50

            height: 11
            width: 13

            color: "#525a72"
            font.family: "Inter"
            font.pixelSize: 9
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Fit")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Rectangle {
            id: undo_redo

            x: 634
            y: 8

            height: 28
            width: 60

            color: "transparent"

            Rectangle {
                id: m_undo

                height: 28
                width: 28

                border.color: "#252836"
                border.width: 1
                color: "#181924"
                radius: 7

                Text {
                    id: m_undo_t

                    x: 7.50
                    y: 6

                    height: 16
                    width: 14

                    color: "#6b7280"
                    font.family: "Inter"
                    font.pixelSize: 13
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignLeft
                    text: qsTr("↺")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                }
            }
            Rectangle {
                id: m_redo

                x: 32

                height: 28
                width: 28

                border.color: "#252836"
                border.width: 1
                color: "#181924"
                radius: 7

                Text {
                    id: m_redo_t

                    x: 7.50
                    y: 6

                    height: 16
                    width: 14

                    color: "#6b7280"
                    font.family: "Inter"
                    font.pixelSize: 13
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignLeft
                    text: qsTr("↻")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                }
            }
        }
        Rectangle {
            id: m_zoom

            x: 702
            y: 11

            height: 22
            width: 46

            border.color: "#252836"
            border.width: 1
            color: "#181924"
            radius: 7

            Text {
                id: m_zoom_t

                x: 10
                y: 5

                height: 12
                width: 27

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("100%")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: m_hdr_line

        x: 280
        y: 91

        height: 1
        width: 760

        color: "#1a1b25"
    }
    Rectangle {
        id: left_panel_bg

        y: 48

        height: 852
        width: 280

        color: "#0f1018"
    }
    Rectangle {
        id: left_panel_border

        x: 279
        y: 48

        height: 852
        width: 1

        color: "#1a1b25"
    }
    Rectangle {
        id: so_header

        x: 12
        y: 56

        height: 58
        width: 256

        border.color: "#252836"
        border.width: 1
        color: "#171924"
        radius: 10

        Rectangle {
            id: so_ic

            x: 10
            y: 11

            height: 36
            width: 36

            border.color: "#2e3a6a"
            border.width: 1
            color: "#1a2240"
            radius: 9

            Text {
                id: so_ic_t

                x: 8
                y: 12.50

                height: 11
                width: 21

                color: "#7b9eff"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("VGR")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: so_text

            x: 56
            y: 14.50

            height: 29
            width: 174

            color: "transparent"

            Text {
                id: so_title

                height: 15
                width: 92

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Sunday Service")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: so_sub

                y: 18

                height: 11
                width: 120

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("12 slides · Worship template")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Text {
            id: so_chev

            x: 240
            y: 22.50

            height: 13
            width: 7

            color: "#525a72"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("⌄")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Rectangle {
        id: so_search

        x: 12
        y: 124

        height: 32
        width: 256

        border.color: "#252836"
        border.width: 1
        color: "#171924"
        radius: 8

        Image {
            id: so_search_ic

            x: 10
            y: 11

            source: Qt.resolvedUrl("assets/so_search_ic_1.png")
        }
        Text {
            id: so_search_t

            x: 28
            y: 9.50

            height: 13
            width: 99

            color: "#525a72"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Search this show...")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Text {
        id: so_sec

        x: 12
        y: 168

        height: 11
        width: 53

        color: "#525a72"
        font.capitalization: Font.AllUppercase
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignLeft
        text: qsTr("SLIDES · 12")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: so_scroll_track

        x: 272
        y: 100

        height: 800
        width: 3

        color: "#171924"
        radius: 2
    }
    Rectangle {
        id: so_scroll_thumb

        x: 272
        y: 100

        height: 460
        width: 3

        color: "#2e3248"
        radius: 2
    }
    Rectangle {
        id: so_s0_bg

        x: 12
        y: 186

        height: 128
        width: 256

        border.color: "#6c5ce7"
        border.width: 1
        clip: true
        color: "#171924"
        radius: 10

        Rectangle {
            id: so_s0_inner

            x: 8
            y: 8

            height: 18
            width: 240

            color: "transparent"

            Rectangle {
                id: so_s0_top

                height: 18
                width: 240

                color: "transparent"

                Text {
                    id: so_s0_num_label

                    x: 6

                    height: 10
                    width: 32

                    color: "#9b8ff5"
                    font.family: "Inter"
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignLeft
                    opacity: 0.70
                    text: qsTr("ACTIVE")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                }
                Rectangle {
                    id: so_s0_pv_num_wrap

                    x: 216

                    height: 18
                    width: 18

                    color: "#1c2030"
                    radius: 4

                    Text {
                        id: so_s0_pv_num_t

                        height: 18
                        width: 19

                        color: "#c9cedd"
                        font.family: "Inter"
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("1")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
            Rectangle {
                id: so_s0_preview

                x: 16
                y: 4

                height: 120
                width: 224

                border.color: "#00252836"
                color: "#000d0f16"
                radius: 7

                Text {
                    id: so_s0_pv_tag

                    x: 6
                    y: 27.50

                    height: 8
                    width: 213

                    color: "#9b8ff5"
                    font.family: "Inter"
                    font.pixelSize: 7
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("SUNDAY SERVICE")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                }
                Text {
                    id: so_s0_pv_title

                    x: 6
                    y: 39.50

                    height: 15
                    width: 213

                    color: "#f2f4fa"
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("WELCOME HOME")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                }
                Text {
                    id: so_s0_pv_l1

                    x: 6
                    y: 58.50

                    height: 9
                    width: 213

                    color: "#6b7280"
                    font.family: "Inter"
                    font.pixelSize: 7
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
                    y: 71.50

                    height: 9
                    width: 213

                    color: "#6b7280"
                    font.family: "Inter"
                    font.pixelSize: 7
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
                    y: 84.50

                    height: 8
                    width: 213

                    color: "#525a72"
                    font.family: "Inter"
                    font.pixelSize: 7
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("- Matthew 18:20")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    Rectangle {
        id: so_s1_bg

        x: 12
        y: 326

        height: 128
        width: 256

        border.color: "#252836"
        border.width: 1
        clip: true
        color: "#13141c"
        radius: 10

        Rectangle {
            id: so_s1_preview

            x: 16
            y: 4

            height: 120
            width: 224

            border.color: "#252836"
            border.width: 1
            color: "#0d0f16"
            radius: 7

            Text {
                id: so_s1_pv_tag

                x: 6
                y: 28.50

                height: 8
                width: 213

                color: "#f59e0b"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("WORSHIP")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s1_pv_title

                x: 6
                y: 40.50

                height: 12
                width: 213

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Great Are You Lord")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s1_pv_l1

                x: 6
                y: 56.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 69.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 82.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("to You only")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            id: so_s1_top

            x: 8
            y: 8

            height: 18
            width: 240

            color: "transparent"

            Text {
                id: s1_spacer

                x: 6

                height: 10
                width: 4

                color: "#00000000"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("·")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: so_s1_num_wrap

                x: 216

                height: 18
                width: 18

                color: "#1c2030"
                radius: 4

                Text {
                    id: so_s1_pv_num_t

                    height: 18
                    width: 19

                    color: "#c9cedd"
                    font.family: "Inter"
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("2")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    Rectangle {
        id: so_s2_bg

        x: 12
        y: 466

        height: 128
        width: 256

        border.color: "#252836"
        border.width: 1
        clip: true
        color: "#13141c"
        radius: 10

        Rectangle {
            id: so_s2_preview

            x: 16
            y: 4

            height: 120
            width: 224

            border.color: "#252836"
            border.width: 1
            color: "#0d0f16"
            radius: 7

            Text {
                id: so_s2_pv_tag

                x: 6
                y: 28.50

                height: 8
                width: 213

                color: "#f59e0b"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("WORSHIP")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s2_pv_title

                x: 6
                y: 40.50

                height: 12
                width: 213

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Way Maker")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s2_pv_l1

                x: 6
                y: 56.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 69.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 82.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Promise Keeper")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            id: so_s2_top

            x: 8
            y: 8

            height: 18
            width: 240

            color: "transparent"

            Text {
                id: s2_spacer

                x: 6

                height: 10
                width: 4

                color: "#00000000"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("·")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: so_s2_num_wrap

                x: 216

                height: 18
                width: 18

                color: "#1c2030"
                radius: 4

                Text {
                    id: so_s2_pv_num_t

                    height: 18
                    width: 19

                    color: "#c9cedd"
                    font.family: "Inter"
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("3")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    Rectangle {
        id: so_s3_bg

        x: 12
        y: 606

        height: 128
        width: 256

        border.color: "#252836"
        border.width: 1
        clip: true
        color: "#13141c"
        radius: 10

        Rectangle {
            id: so_s3_preview

            x: 16
            y: 4

            height: 120
            width: 224

            border.color: "#252836"
            border.width: 1
            color: "#0d0f16"
            radius: 7

            Text {
                id: so_s3_pv_tag

                x: 6
                y: 28.50

                height: 8
                width: 213

                color: "#f59e0b"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("WORSHIP")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s3_pv_title

                x: 6
                y: 40.50

                height: 12
                width: 213

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Oceans")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Text {
                id: so_s3_pv_l1

                x: 6
                y: 56.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 69.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
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
                y: 82.50

                height: 9
                width: 213

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("where feet may fail")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            id: so_s3_top

            x: 8
            y: 8

            height: 18
            width: 240

            color: "transparent"

            Text {
                id: s3_spacer

                x: 6

                height: 10
                width: 4

                color: "#00000000"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("·")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: so_s3_num_wrap

                x: 216

                height: 18
                width: 18

                color: "#1c2030"
                radius: 4

                Text {
                    id: so_s3_pv_num_t

                    height: 18
                    width: 19

                    color: "#c9cedd"
                    font.family: "Inter"
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("4")
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    Rectangle {
        id: so_addslide

        x: 12
        y: 888

        height: 36
        width: 256

        border.color: "#252836"
        border.width: 1
        color: "#171924"
        radius: 8

        Text {
            id: element_1

            x: 95.50
            y: 10.50

            height: 15
            width: 9

            color: "#6c5ce7"
            font.family: "Inter"
            font.pixelSize: 12
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            text: qsTr("+")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Text {
            id: so_addslide_t

            x: 109.50
            y: 11.50

            height: 13
            width: 52

            color: "#6b7280"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Add Slide")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Rectangle {
        id: m_canvas

        x: 280
        y: 92

        height: 808
        width: 760

        color: "#0b0c12"
    }
    Image {
        id: m_dot_a1

        x: 344
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a1.png")
    }
    Image {
        id: m_dot_a2

        x: 400
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a2.png")
    }
    Image {
        id: m_dot_a3

        x: 456
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a3.png")
    }
    Image {
        id: m_dot_a4

        x: 512
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a4.png")
    }
    Image {
        id: m_dot_a5

        x: 568
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a5.png")
    }
    Image {
        id: m_dot_a6

        x: 624
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a6.png")
    }
    Image {
        id: m_dot_a7

        x: 680
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a7.png")
    }
    Image {
        id: m_dot_a8

        x: 736
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a8.png")
    }
    Image {
        id: m_dot_a9

        x: 792
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a9.png")
    }
    Image {
        id: m_dot_a10

        x: 848
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a10.png")
    }
    Image {
        id: m_dot_a11

        x: 904
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a11.png")
    }
    Image {
        id: m_dot_a12

        x: 960
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a12.png")
    }
    Image {
        id: m_dot_a13

        x: 1016
        y: 156

        source: Qt.resolvedUrl("assets/m_dot_a13.png")
    }
    Image {
        id: m_dot_b1

        x: 344
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b1.png")
    }
    Image {
        id: m_dot_b2

        x: 400
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b2.png")
    }
    Image {
        id: m_dot_b3

        x: 456
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b3.png")
    }
    Image {
        id: m_dot_b4

        x: 512
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b4.png")
    }
    Image {
        id: m_dot_b5

        x: 568
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b5.png")
    }
    Image {
        id: m_dot_b6

        x: 624
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b6.png")
    }
    Image {
        id: m_dot_b7

        x: 680
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b7.png")
    }
    Image {
        id: m_dot_b8

        x: 736
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b8.png")
    }
    Image {
        id: m_dot_b9

        x: 792
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b9.png")
    }
    Image {
        id: m_dot_b10

        x: 848
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b10.png")
    }
    Image {
        id: m_dot_b11

        x: 904
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b11.png")
    }
    Image {
        id: m_dot_b12

        x: 960
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b12.png")
    }
    Image {
        id: m_dot_b13

        x: 1016
        y: 212

        source: Qt.resolvedUrl("assets/m_dot_b13.png")
    }
    Image {
        id: m_dot_c1

        x: 344
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c1.png")
    }
    Image {
        id: m_dot_c2

        x: 400
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c2.png")
    }
    Image {
        id: m_dot_c3

        x: 456
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c3.png")
    }
    Image {
        id: m_dot_c4

        x: 512
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c4.png")
    }
    Image {
        id: m_dot_c5

        x: 568
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c5.png")
    }
    Image {
        id: m_dot_c6

        x: 624
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c6.png")
    }
    Image {
        id: m_dot_c7

        x: 680
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c7.png")
    }
    Image {
        id: m_dot_c8

        x: 736
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c8.png")
    }
    Image {
        id: m_dot_c9

        x: 792
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c9.png")
    }
    Image {
        id: m_dot_c10

        x: 848
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c10.png")
    }
    Image {
        id: m_dot_c11

        x: 904
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c11.png")
    }
    Image {
        id: m_dot_c12

        x: 960
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c12.png")
    }
    Image {
        id: m_dot_c13

        x: 1016
        y: 268

        source: Qt.resolvedUrl("assets/m_dot_c13.png")
    }
    Image {
        id: m_dot_d1

        x: 344
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_d1.png")
    }
    Image {
        id: m_dot_d4

        x: 512
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_d4.png")
    }
    Image {
        id: m_dot_d7

        x: 680
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_d7.png")
    }
    Image {
        id: m_dot_d10

        x: 848
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_d10.png")
    }
    Image {
        id: m_dot_d13

        x: 1016
        y: 324

        source: Qt.resolvedUrl("assets/m_dot_d13.png")
    }
    Image {
        id: m_dot_e1

        x: 344
        y: 780

        source: Qt.resolvedUrl("assets/m_dot_e1.png")
    }
    Image {
        id: m_dot_e4

        x: 512
        y: 780

        source: Qt.resolvedUrl("assets/m_dot_e4.png")
    }
    Image {
        id: m_dot_e7

        x: 680
        y: 780

        source: Qt.resolvedUrl("assets/m_dot_e7.png")
    }
    Image {
        id: m_dot_e10

        x: 848
        y: 780

        source: Qt.resolvedUrl("assets/m_dot_e10.png")
    }
    Image {
        id: m_dot_e13

        x: 1016
        y: 780

        source: Qt.resolvedUrl("assets/m_dot_e13.png")
    }
    Text {
        id: m_crumb

        x: 296
        y: 108

        height: 12
        width: 145

        color: "#525a72"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignLeft
        text: qsTr("Sunday Service · Slide 1 - Title")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Image {
        id: slide_bg

        x: 296
        y: 286

        source: Qt.resolvedUrl("assets/slide_bg_1.png")
    }
    Rectangle {
        id: slide_logo_group

        x: 358
        y: 328

        height: 22
        width: 42

        color: "transparent"

        Image {
            id: slide_logo

            source: Qt.resolvedUrl("assets/slide_logo_1.png")
        }
        Text {
            id: slide_logo_t

            x: 26
            y: 6.50

            height: 9
            width: 17

            color: "#eef0f6"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignLeft
            text: qsTr("VGR")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Text {
        id: slide_sub

        x: 550
        y: 332

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
    Rectangle {
        id: sel_box

        x: 430
        y: 430

        height: 72
        width: 460

        border.color: "#6c5ce7"
        border.width: 1.50
        color: "#08ffffff"
        radius: 3
    }
    Rectangle {
        id: sel_h_tl

        x: 427
        y: 427

        height: 6
        width: 6

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: sel_h_tr

        x: 887
        y: 427

        height: 6
        width: 6

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: sel_h_bl

        x: 427
        y: 499

        height: 6
        width: 6

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: sel_h_br

        x: 887
        y: 499

        height: 6
        width: 6

        color: "#6c5ce7"
        radius: 1
    }
    Rectangle {
        id: sel_tag

        x: 604
        y: 412

        height: 18
        width: 111

        border.color: "#406c5ce7"
        border.width: 1
        color: "#206c5ce7"
        radius: 5

        Text {
            id: sel_tag_t

            x: 8
            y: 4

            height: 10
            width: 96

            color: "#9b8ff5"
            font.family: "Inter"
            font.pixelSize: 8
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignLeft
            text: qsTr("TEXT · WELCOME HOME")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Text {
        id: slide_title

        x: 434
        y: 440

        height: 60
        width: 453

        color: "#f2f4fa"
        font.family: "Inter"
        font.pixelSize: 42
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("WELCOME HOME")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_v0

        x: 490
        y: 520

        height: 16
        width: 341

        color: "#c7cbd8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("\"For where two or three gather in my name,")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_v1

        x: 560
        y: 542

        height: 16
        width: 201

        color: "#c7cbd8"
        font.family: "Inter"
        font.pixelSize: 13
        font.weight: Font.Normal
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("there am I with them.\"")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Text {
        id: slide_ref

        x: 605
        y: 566

        height: 12
        width: 111

        color: "#6b7080"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("- Matthew 18:20")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
        wrapMode: Text.Wrap
    }
    Shape {
        id: cam_bg

        x: 848
        y: 564

        height: 90
        width: 116

        ShapePath {
            id: cam_bgShapePath

            strokeColor: "#1e3a2a"
            strokeWidth: 1

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

                radius: 10
            }
        }
        Rectangle {
            id: cam_header

            x: 8
            y: 8

            height: 9
            width: 100

            color: "transparent"

            Image {
                id: cam_dot

                y: 1.50

                source: Qt.resolvedUrl("assets/cam_dot_1.png")
            }
            Text {
                id: cam_live

                x: 10

                height: 9
                width: 18

                color: "#e2e8f0"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("LIVE")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: cam_preview

            x: 8
            y: 23

            height: 34
            width: 100

            color: "transparent"

            Rectangle {
                id: cam_scene

                height: 34
                width: 36

                color: "#14503a"
                radius: 4
            }
            Rectangle {
                id: cam_scene2

                x: 40

                height: 34
                width: 24

                color: "#0f3a2c"
                radius: 4
            }
            Rectangle {
                id: cam_right

                x: 68

                height: 8
                width: 32

                color: "transparent"

                Image {
                    id: cam_lens

                    source: Qt.resolvedUrl("assets/cam_lens_1.png")
                }
            }
        }
        Rectangle {
            id: cam_footer

            x: 8
            y: 63

            height: 10
            width: 100

            color: "transparent"

            Text {
                id: cam_name

                height: 10
                width: 28

                color: "#eef0f6"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("CAM 1")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: cam_time

                x: 67

                height: 10
                width: 34

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("10:24:07")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Image {
        id: madd_chip

        x: 271
        y: 826

        source: Qt.resolvedUrl("assets/madd_chip_1.png")
    }
    Image {
        id: madd_menu

        x: 318
        y: 820

        source: Qt.resolvedUrl("assets/madd_menu.png")
    }
    Rectangle {
        id: m_zoombar

        x: 878
        y: 843

        height: 32
        width: 120

        border.color: "#252836"
        border.width: 1
        color: "#171924"
        radius: 9

        Text {
            id: m_zb_m

            x: 8
            y: 6.50

            height: 19
            width: 12

            color: "#525a72"
            font.family: "Inter"
            font.pixelSize: 16
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("−")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Rectangle {
            id: zoom_center

            x: 19
            y: 10

            height: 12
            width: 82

            color: "transparent"

            Text {
                id: m_zb_100

                x: 28

                height: 12
                width: 27

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("100%")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Text {
            id: m_zb_p

            x: 101
            y: 6.50

            height: 19
            width: 12

            color: "#525a72"
            font.family: "Inter"
            font.pixelSize: 16
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignLeft
            text: qsTr("+")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Rectangle {
        id: r_bg

        x: 1040
        y: 48

        height: 852
        width: 400

        color: "#0f1018"
    }
    Rectangle {
        id: r_border

        x: 1040
        y: 48

        height: 852
        width: 1

        color: "#1a1b25"
    }
    Rectangle {
        id: r_tabs

        x: 1040
        y: 56

        height: 36
        width: 400

        color: "transparent"

        Rectangle {
            id: r_tab_items

            x: 16
            y: 20.50

            height: 15.50
            width: 80

            color: "transparent"

            Text {
                id: r_tab0

                x: 22.50

                height: 13
                width: 36

                color: "#f05252"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("ITEMS")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: r_tab0_bar

                x: 18
                y: 13

                height: 2.50
                width: 44

                color: "#f05252"
                radius: 2
            }
        }
        Rectangle {
            id: r_tab_text

            x: 96
            y: 20.50

            height: 15.50
            width: 80

            color: "transparent"

            Text {
                id: r_tab1

                x: 26

                height: 13
                width: 29

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("TEXT")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: r_tab1_bar

                x: 18
                y: 13

                height: 2.50
                width: 44

                color: "#00000000"
                radius: 2
            }
        }
        Rectangle {
            id: r_tab_slide

            x: 176
            y: 20.50

            height: 15.50
            width: 80

            color: "transparent"

            Text {
                id: r_tab2

                x: 24.50

                height: 13
                width: 32

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("SLIDE")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Rectangle {
                id: r_tab2_bar

                x: 18
                y: 13

                height: 2.50
                width: 44

                color: "#00000000"
                radius: 2
            }
        }
    }
    Rectangle {
        id: r_tab_line

        x: 1040
        y: 91

        height: 1
        width: 400

        color: "#1a1b25"
    }
    Rectangle {
        id: preview_monitors_grid

        x: 1040
        y: 98

        height: 352
        width: 400

        color: "transparent"

        Rectangle {
            id: preview_row_1

            x: 14
            y: 14

            height: 155
            width: 372

            color: "transparent"

            Rectangle {
                id: preview_Main_Output

                height: 155
                width: 179

                border.color: "#f05252"
                border.width: 1
                color: "#171924"
                radius: 10

                Rectangle {
                    id: preview_screen

                    x: 7
                    y: 7

                    height: 108
                    width: 165

                    clip: true
                    color: "#0d0f16"
                    radius: 6

                    Rectangle {
                        id: frame

                        x: 6
                        y: 6

                        height: 17
                        width: 50

                        border.color: "#50f05252"
                        border.width: 1
                        color: "#30f05252"
                        radius: 4

                        Image {
                            id: ellipse

                            x: 7
                            y: 6

                            source: Qt.resolvedUrl("assets/ellipse.png")
                        }
                        Text {
                            id: lIVE_1

                            x: 16
                            y: 3

                            height: 11
                            width: 28

                            color: "#f2f4fa"
                            font.family: "Inter"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("LIVE 1")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                    Rectangle {
                        id: frame_1

                        x: 71
                        y: 42

                        height: 28
                        width: 28

                        color: "#60000000"
                        radius: 14

                        Text {
                            id: element_2

                            x: 9
                            y: 8

                            height: 12
                            width: 11

                            color: "#f05252"
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: Font.Normal
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("▶")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                }
                Rectangle {
                    id: preview_footer

                    x: 7
                    y: 120

                    height: 28
                    width: 165

                    color: "transparent"

                    Text {
                        id: main_Output

                        x: 4
                        y: 7.50

                        height: 13
                        width: 67

                        color: "#eef1f8"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Main Output")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: frame_2

                        x: 139
                        y: 3

                        height: 22
                        width: 22

                        color: "#1e2030"
                        radius: 6

                        Rectangle {
                            id: settings

                            x: 5
                            y: 5

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
                                    strokeColor: "#525a72"
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
            }
            Rectangle {
                id: preview_Stage_Screen

                x: 193

                height: 155
                width: 179

                border.color: "#252836"
                border.width: 1
                color: "#171924"
                radius: 10

                Rectangle {
                    id: preview_screen_1

                    x: 7
                    y: 7

                    height: 108
                    width: 165

                    clip: true
                    color: "#0d0f16"
                    radius: 6

                    Rectangle {
                        id: frame_3

                        x: 6
                        y: 6

                        height: 17
                        width: 51

                        color: "#60000000"
                        radius: 4

                        Text {
                            id: sTAGE_1

                            x: 7
                            y: 3

                            height: 11
                            width: 38

                            color: "#9ba3bf"
                            font.family: "Inter"
                            font.pixelSize: 9
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

                    x: 7
                    y: 120

                    height: 28
                    width: 165

                    color: "transparent"

                    Text {
                        id: stage_Screen

                        x: 4
                        y: 7.50

                        height: 13
                        width: 73

                        color: "#eef1f8"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Stage Screen")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: frame_4

                        x: 139
                        y: 3

                        height: 22
                        width: 22

                        color: "#1e2030"
                        radius: 6

                        Rectangle {
                            id: settings_1

                            x: 5
                            y: 5

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
                                    strokeColor: "#525a72"
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
        }
        Rectangle {
            id: preview_row_2

            x: 14
            y: 183

            height: 155
            width: 372

            color: "transparent"

            Rectangle {
                id: preview_Nursery_Display

                height: 155
                width: 179

                border.color: "#252836"
                border.width: 1
                color: "#171924"
                radius: 10

                Rectangle {
                    id: preview_screen_2

                    x: 7
                    y: 7

                    height: 108
                    width: 165

                    clip: true
                    color: "#0d0f16"
                    radius: 6

                    Rectangle {
                        id: frame_5

                        x: 6
                        y: 6

                        height: 17
                        width: 57

                        color: "#60000000"
                        radius: 4

                        Text {
                            id: nURSERY

                            x: 7
                            y: 3

                            height: 11
                            width: 44

                            color: "#9ba3bf"
                            font.family: "Inter"
                            font.pixelSize: 9
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

                    x: 7
                    y: 120

                    height: 28
                    width: 165

                    color: "transparent"

                    Text {
                        id: nursery_Display

                        x: 4
                        y: 7.50

                        height: 13
                        width: 86

                        color: "#eef1f8"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Nursery Display")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: frame_6

                        x: 139
                        y: 3

                        height: 22
                        width: 22

                        color: "#1e2030"
                        radius: 6

                        Rectangle {
                            id: settings_2

                            x: 5
                            y: 5

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
                                    strokeColor: "#525a72"
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
            }
            Rectangle {
                id: preview_Stream_Overlay

                x: 193

                height: 155
                width: 179

                border.color: "#252836"
                border.width: 1
                color: "#171924"
                radius: 10

                Rectangle {
                    id: preview_screen_3

                    x: 7
                    y: 7

                    height: 108
                    width: 165

                    clip: true
                    color: "#0d0f16"
                    radius: 6

                    Rectangle {
                        id: frame_7

                        x: 6
                        y: 6

                        height: 17
                        width: 58

                        color: "#60000000"
                        radius: 4

                        Text {
                            id: oBS_FEED

                            x: 7
                            y: 3

                            height: 11
                            width: 45

                            color: "#9ba3bf"
                            font.family: "Inter"
                            font.pixelSize: 9
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

                    x: 7
                    y: 120

                    height: 28
                    width: 165

                    color: "transparent"

                    Text {
                        id: stream_Overlay

                        x: 4
                        y: 7.50

                        height: 13
                        width: 83

                        color: "#eef1f8"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Stream Overlay")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Rectangle {
                        id: frame_8

                        x: 139
                        y: 3

                        height: 22
                        width: 22

                        color: "#1e2030"
                        radius: 6

                        Rectangle {
                            id: settings_3

                            x: 5
                            y: 5

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
                                    strokeColor: "#525a72"
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
    }
    Rectangle {
        id: tx_hdr_wrap

        x: 1056
        y: 438

        height: 19
        width: 65

        color: "transparent"

        Rectangle {
            id: tx_hdr

            height: 19
            width: 65

            color: "#6c5ce7"
            radius: 6

            Text {
                id: tx_hdr_t

                x: 8
                y: 4

                height: 11
                width: 50

                color: "#ffffff"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("TEXT ITEM")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: tx_fmt_bg

        x: 1056
        y: 462

        height: 44
        width: 368

        border.color: "#252836"
        border.width: 1
        color: "#13151c"
        radius: 10

        Rectangle {
            id: tx_fmt_b

            x: 8
            y: 6

            height: 32
            width: 32

            border.color: "#306c5ce7"
            border.width: 1
            color: "#206c5ce7"
            radius: 7

            Text {
                id: tx_fmt_b_t

                x: 11.50
                y: 8

                height: 16
                width: 10

                color: "#9b8ff5"
                font.family: "Inter"
                font.pixelSize: 13
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("B")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_fmt_i

            x: 43
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Text {
                id: tx_fmt_i_t

                x: 14
                y: 8

                height: 16
                width: 5

                color: "#6b7280"
                font.family: "Inter"
                font.italic: true
                font.pixelSize: 13
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignLeft
                text: qsTr("I")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_fmt_u

            x: 78
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Text {
                id: tx_fmt_u_t

                x: 11
                y: 8

                height: 16
                width: 11

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 13
                font.underline: true
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("U")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_fmt_s

            x: 113
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Text {
                id: tx_fmt_s_t

                x: 11.50
                y: 8

                height: 16
                width: 10

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 13
                font.strikeout: true
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("S")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_fmt_div

            x: 148
            y: 12

            height: 20
            width: 1

            color: "#252836"
        }
        Rectangle {
            id: tx_fmt_al

            x: 152
            y: 6

            height: 32
            width: 32

            border.color: "#306c5ce7"
            border.width: 1
            color: "#206c5ce7"
            radius: 7

            Rectangle {
                id: align_left

                x: 9
                y: 10

                height: 12
                width: 14

                clip: true
                color: "transparent"

                Shape {
                    id: _vector_5

                    x: 1.75
                    y: 2.50

                    height: 7
                    width: 10.50

                    ShapePath {
                        id: _vector_5_ShapePath0

                        fillColor: "#00000000"
                        strokeColor: "#9b8ff5"
                        strokeWidth: 2

                        PathSvg {
                            id: _vector_5_ShapePath0_PathSvg0

                            path: "M 10.5 0 L 0 0 M 7 3.500400066375732 L 0 3.500400066375732 M 8.166666666666668 7.000800132751464 L 0 7.000800132751464"
                        }
                    }
                }
            }
        }
        Rectangle {
            id: tx_fmt_ac

            x: 187
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Rectangle {
                id: align_center

                x: 9
                y: 10

                height: 12
                width: 14

                clip: true
                color: "transparent"

                Shape {
                    id: _vector_6

                    x: 1.75
                    y: 2.50

                    height: 7
                    width: 10.50

                    ShapePath {
                        id: _vector_6_ShapePath0

                        fillColor: "#00000000"
                        strokeColor: "#525a72"
                        strokeWidth: 2

                        PathSvg {
                            id: _vector_6_ShapePath0_PathSvg0

                            path: "M 10.5 0 L 0 0 M 8.166666666666668 3.500400066375732 L 2.3333333333333335 3.500400066375732 M 9.333333333333334 7.000800132751464 L 1.1666666666666667 7.000800132751464"
                        }
                    }
                }
            }
        }
        Rectangle {
            id: tx_fmt_ar

            x: 222
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Rectangle {
                id: align_right

                x: 9
                y: 10

                height: 12
                width: 14

                clip: true
                color: "transparent"

                Shape {
                    id: _vector_7

                    x: 1.75
                    y: 2.50

                    height: 7
                    width: 10.50

                    ShapePath {
                        id: _vector_7_ShapePath0

                        fillColor: "#00000000"
                        strokeColor: "#525a72"
                        strokeWidth: 2

                        PathSvg {
                            id: _vector_7_ShapePath0_PathSvg0

                            path: "M 10.5 0 L 0 0 M 10.5 3.500400066375732 L 3.5 3.500400066375732 M 10.5 7.000800132751464 L 2.3333333333333335 7.000800132751464"
                        }
                    }
                }
            }
        }
        Rectangle {
            id: tx_fmt_aj

            x: 257
            y: 6

            height: 32
            width: 32

            color: "#1a1c28"
            radius: 7

            Rectangle {
                id: align_justify

                x: 9
                y: 10

                height: 12
                width: 14

                clip: true
                color: "transparent"

                Shape {
                    id: _vector_8

                    x: 1.75
                    y: 2.50

                    height: 7
                    width: 10.50

                    ShapePath {
                        id: _vector_8_ShapePath0

                        fillColor: "#00000000"
                        strokeColor: "#525a72"
                        strokeWidth: 2

                        PathSvg {
                            id: _vector_8_ShapePath0_PathSvg0

                            path: "M 0 0 L 10.5 0 M 0 3.500400066375732 L 10.5 3.500400066375732 M 0 7.000800132751464 L 10.5 7.000800132751464"
                        }
                    }
                }
            }
        }
    }
    Rectangle {
        id: tx_font_row

        x: 1056
        y: 518

        height: 48
        width: 368

        border.color: "#252836"
        border.width: 1
        color: "#13151c"
        radius: 10

        Text {
            id: tx_font_aa

            x: 14
            y: 13

            height: 22
            width: 25

            color: "#f2f4fa"
            font.family: "Inter"
            font.pixelSize: 18
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Aa")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Text {
            id: tx_font_t

            x: 48
            y: 16.50

            height: 15
            width: 85

            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 12
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            text: qsTr("Inter SemiBold")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
        Rectangle {
            id: font_spacer

            x: 142
            y: 23.50

            height: 1
            width: 168

            color: "transparent"
        }
        Image {
            id: tx_font_sw

            x: 320
            y: 14

            source: Qt.resolvedUrl("assets/tx_font_sw.png")
        }
        Text {
            id: tx_font_chev

            x: 350
            y: 18

            height: 12
            width: 5

            color: "#6b7280"
            font.family: "Inter"
            font.pixelSize: 10
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignLeft
            text: qsTr("▾")
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
        }
    }
    Text {
        id: tx_az_l

        x: 1056
        y: 580

        height: 12
        width: 55

        color: "#6b7280"
        font.family: "Inter"
        font.pixelSize: 10
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignLeft
        text: qsTr("AUTO SIZE")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: tx_az_bg

        x: 1056
        y: 596

        height: 32
        width: 368

        border.color: "#252836"
        border.width: 1
        color: "#13151c"
        radius: 8

        Rectangle {
            id: tx_az0

            x: 4
            y: 4

            height: 24
            width: 118.67

            border.color: "#406c5ce7"
            border.width: 1
            color: "#206c5ce7"
            radius: 5

            Text {
                id: tx_az0_t

                x: 46.33
                y: 6

                height: 12
                width: 27

                color: "#f0f2fa"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("None")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_az1

            x: 126.67
            y: 4

            height: 24
            width: 116.67

            color: "#1a1c28"
            radius: 5

            Text {
                id: tx_az1_t

                x: 42.83
                y: 6

                height: 12
                width: 32

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Shrink")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: tx_az2

            x: 247.33
            y: 4

            height: 24
            width: 116.67

            color: "#1a1c28"
            radius: 5

            Text {
                id: tx_az2_t

                x: 45.33
                y: 6

                height: 12
                width: 27

                color: "#525a72"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Grow")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: tx_metrics_card

        x: 1056
        y: 642

        height: 110
        width: 368

        border.color: "#252836"
        border.width: 1
        clip: true
        color: "#13151c"
        radius: 12

        Rectangle {
            id: slider_size

            x: 16
            y: 16

            height: 32
            width: 336

            color: "transparent"

            Text {
                id: tx_size_l

                y: 0.50

                height: 13
                width: 57

                color: "#9ba3bf"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Size")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Rectangle {
                id: slider_track_wrap

                x: 56

                height: 14
                width: 250

                color: "transparent"

                Rectangle {
                    id: tx_size_fill

                    y: 5.50

                    height: 3
                    width: 150

                    color: "#6c5ce7"
                    radius: 2
                }
                Image {
                    id: tx_size_kb

                    x: 150

                    source: Qt.resolvedUrl("assets/tx_size_kb.png")
                }
                Rectangle {
                    id: tx_size_track

                    x: 164
                    y: 5.50

                    height: 3
                    width: 86

                    color: "#2a2f42"
                    radius: 2
                }
            }
            Text {
                id: tx_size_v

                x: 306
                y: 0.50

                height: 13
                width: 31

                color: "#d3d5dd"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
                text: qsTr("48")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            id: slider_height

            x: 16
            y: 48

            height: 32
            width: 336

            color: "transparent"

            Text {
                id: tx_lh_l

                y: 0.50

                height: 13
                width: 57

                color: "#9ba3bf"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Height")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Rectangle {
                id: slider_track_wrap_lh

                x: 56

                height: 14
                width: 250

                color: "transparent"

                Rectangle {
                    id: tx_lh_fill

                    y: 5.50

                    height: 3
                    width: 85

                    color: "#6c5ce7"
                    radius: 2
                }
                Image {
                    id: tx_lh_kb

                    x: 85

                    source: Qt.resolvedUrl("assets/tx_lh_kb.png")
                }
                Rectangle {
                    id: tx_lh_track

                    x: 99
                    y: 5.50

                    height: 3
                    width: 151

                    color: "#2a2f42"
                    radius: 2
                }
            }
            Text {
                id: tx_lh_v

                x: 306
                y: 0.50

                height: 13
                width: 31

                color: "#d3d5dd"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
                text: qsTr("1.2")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            id: slider_spacing

            x: 16
            y: 80

            height: 14
            width: 336

            color: "transparent"

            Text {
                id: tx_sp_l

                y: 0.50

                height: 13
                width: 57

                color: "#9ba3bf"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Spacing")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
            Rectangle {
                id: slider_track_wrap_sp

                x: 56

                height: 14
                width: 250

                color: "transparent"

                Rectangle {
                    id: tx_sp_fill

                    y: 5.50

                    height: 3
                    width: 25

                    color: "#6c5ce7"
                    radius: 2
                }
                Image {
                    id: tx_sp_kb

                    x: 25

                    source: Qt.resolvedUrl("assets/tx_sp_kb.png")
                }
                Rectangle {
                    id: tx_sp_track

                    x: 39
                    y: 5.50

                    height: 3
                    width: 211

                    color: "#2a2f42"
                    radius: 2
                }
            }
            Text {
                id: tx_sp_v

                x: 306
                y: 0.50

                height: 13
                width: 31

                color: "#d3d5dd"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
                text: qsTr("0.0")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
    }
    Text {
        id: rp_sec_pos

        x: 1056
        y: 790

        height: 11
        width: 79

        color: "#525a72"
        font.family: "Inter"
        font.pixelSize: 9
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignLeft
        text: qsTr("POSITION & SIZE")
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignTop
    }
    Rectangle {
        id: rp_pos_card

        x: 1056
        y: 808

        height: 54
        width: 368

        border.color: "#252836"
        border.width: 1
        color: "#13151c"
        radius: 10

        Rectangle {
            id: rp_pos0

            x: 12
            y: 9

            height: 36
            width: 80

            border.color: "#252836"
            border.width: 1
            color: "#1a1c28"
            radius: 7

            Text {
                id: rp_pos0_l

                x: 8
                y: 12

                height: 12
                width: 9

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("X")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: rp_pos0_v

                x: 52
                y: 12

                height: 12
                width: 21

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("332")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: rp_pos1

            x: 100
            y: 9

            height: 36
            width: 80

            border.color: "#252836"
            border.width: 1
            color: "#1a1c28"
            radius: 7

            Text {
                id: rp_pos1_l

                x: 8
                y: 12

                height: 12
                width: 9

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("Y")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: rp_pos1_v

                x: 52
                y: 12

                height: 12
                width: 21

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("436")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: rp_pos2

            x: 188
            y: 9

            height: 36
            width: 80

            border.color: "#252836"
            border.width: 1
            color: "#1a1c28"
            radius: 7

            Text {
                id: rp_pos2_l

                x: 8
                y: 12

                height: 12
                width: 12

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("W")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: rp_pos2_v

                x: 52
                y: 12

                height: 12
                width: 21

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("640")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
        Rectangle {
            id: rp_pos3

            x: 276
            y: 9

            height: 36
            width: 80

            border.color: "#252836"
            border.width: 1
            color: "#1a1c28"
            radius: 7

            Text {
                id: rp_pos3_l

                x: 8
                y: 12

                height: 12
                width: 9

                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("H")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
            Text {
                id: rp_pos3_v

                x: 52
                y: 12

                height: 12
                width: 21

                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 10
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: qsTr("360")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }
        }
    }
    Rectangle {
        id: so_s4_bg

        x: 12
        y: 746

        height: 128
        width: 256

        border.color: "#252836"
        border.width: 1
        clip: true
        color: "#13141c"
        radius: 10

        Rectangle {
            id: so_s4_pv

            x: 8
            y: 8

            height: 112
            width: 240

            border.color: "#252836"
            border.width: 1
            color: "#0d0f16"
            radius: 8
        }
        Rectangle {
            id: so_s4_num_wrap

            x: 230
            y: 14

            height: 18
            width: 18

            clip: true
            color: "#1c2030"
            radius: 4

            Text {
                id: so_s4_pv_num_t

                height: 18
                width: 19

                color: "#c9cedd"
                font.family: "Inter"
                font.pixelSize: 8
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("5")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}