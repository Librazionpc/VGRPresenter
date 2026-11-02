import QtQuick
import VGRPresenterUI

Item {
    id: root

    // Roster-change revision: rowCount() isn't notifyable from QML.
    property int _wallRev: 0
    Connections {
        target: OutputListModel
        function onRowsInserted() { root._wallRev++ }
        function onRowsRemoved() { root._wallRev++ }
        function onDataChanged() { root._wallRev++ }
    }

    readonly property int frameRev: LiveOutputService.frameRev
    readonly property bool live: LiveOutputService.live
    readonly property bool inputTaken: LiveOutputService.inputLabel !== ""
    readonly property bool staged: LiveOutputService.stagedSlide.valid === true
    readonly property bool hasRealOnAirContent: !!(LiveOutputService.onAirSlide.blocks
        && LiveOutputService.onAirSlide.blocks.length > 0)
    readonly property bool anythingOnAir: root.live || root.inputTaken || root.staged
        || LiveOutputService.mediaOnAir || LiveOutputService.activeOverlays.length > 0
        || LiveOutputService.canRestoreCleared

    readonly property real toolIconSize: 14
    // ...and one stroke weight, or a shipped 2-unit stroke reads heavier at
    // one size than the other even with equal boxes.
    readonly property real toolIconStroke: 1.6
    // Keep the wall's icon actions compact and consistent with its text rows.
    readonly property int actionButtonHeight: 26

    // External contract: hosts position content under the wall and drive
    // the page dots from these.
    readonly property alias pageCount: wall.pageCount
    readonly property alias currentPage: wall.currentPage
    property bool showClearControls: true
    readonly property bool scrollMode: LiveOutputService.scrollMode
    readonly property string modeKey: {
        const t = String(LiveOutputService.wallStepperId || "")
        return (t === "scripture" ? "scripture" : t === "show" ? "shows" : "table") + ".scrollWalk"
    }
    function adoptMode() {
        const stored = String(SettingsService.values[root.modeKey] ?? "")
        if (stored !== "scroll" && stored !== "slide")
            return
        LiveOutputService.scrollMode = stored === "scroll"
    }
    onModeKeyChanged: root.adoptMode()
    Component.onCompleted: root.adoptMode()
    Connections {
        target: LiveOutputService
        function onScrollModeChanged() {
            SettingsService.setValue(root.modeKey, LiveOutputService.scrollMode ? "scroll" : "slide")
        }
    }
    function goTo(page) {
        wall.goTo(page)
    }
    function setOutputEnabled(index, enabled) {
        if (OutputListModel.setEnabled(index, enabled))
            return true
        EventBus.notify(qsTr("At least one output card must remain enabled."),
                        "warning", qsTr("Outputs"), "outputs.lastEnabled")
        return false
    }

    // The wall's total height: the paged tiles, then (when shown) the transport toolbar. One expression, used by the declarative height AND the re-assertion at the
    // bottom of this file, so the two can never disagree.
    readonly property real belowWallY: wall.height + (wall.pageCount > 1 ? 22 : 8)
    readonly property real wallHeight: {
        if (!toolbar.visible)
            return wall.height + (wall.pageCount > 1 ? 12 : 0)
        return root.belowWallY + toolbar.height
    }
    height: root.wallHeight

    // ---- Paged tile wall -----------------------------------------------------
    Flickable {
        id: wall

        anchors.horizontalCenter: parent.horizontalCenter
        y: 0

        readonly property int count: {
            root._wallRev
            let enabled = 0
            for (let i = 0; i < OutputListModel.rowCount(); ++i)
                if (OutputListModel.getOutput(i).isEnabled === true) ++enabled
            return enabled
        }
        // Keep the same tile grid on every paged view; a sparse last page should
        // not enlarge its tiles or shorten the wall.
        readonly property int perPage: count === 1 ? 1 : 4
        readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
        readonly property real pageW: root.width
        readonly property real columnGap: 0
        // A single-output wall uses one full-width preview. Multi-output walls
        // retain two columns, including on a sparse final page.
        function itemsOnPage(page) {
            return Math.max(0, Math.min(perPage, count - page * perPage))
        }
        function enabledIndex(row) {
            root._wallRev
            const current = OutputListModel.getOutput(row)
            if (!current || current.isEnabled !== true) return -1
            let slot = 0
            for (let i = 0; i < row; ++i)
                if (OutputListModel.getOutput(i).isEnabled === true) ++slot
            return slot
        }
        function columnsForPage(page) {
            return count === 1 ? 1 : 2
        }
        function slotWidthForPage(page) {
            const columns = columnsForPage(page)
            return (pageW - (columns - 1) * columnGap) / columns
        }
        function slotXForPage(page, slot) {
            const tileWidth = slotWidthForPage(page)
            const items = itemsOnPage(page)
            const loneLastTile = columnsForPage(page) === 2 && items % 2 === 1 && slot === items - 1
            const column = loneLastTile ? (pageW - tileWidth) / 2 : (slot % columnsForPage(page)) * (tileWidth + columnGap)
            return page * pageW + column
        }
        function rowsForPage(page) {
            if (pageCount > 1) return 2
            return Math.max(1, Math.ceil(itemsOnPage(page) / columnsForPage(page)))
        }
        function slotYForPage(page, slot) {
            const slotHeight = slotHeightForPage(page)
            if (pageCount > 1 && itemsOnPage(page) === 1)
                return (pageHeightForPage(page) - slotHeight) / 2
            return Math.floor(slot / columnsForPage(page)) * slotHeight
        }
        // Every tile wears a NAME CARD above its picture.
        readonly property int headerH: 20
        function slotHeightForPage(page) {
            return headerH + 2 + (slotWidthForPage(page) - 2) * 9 / 16
        }
        function pageHeightForPage(page) {
            if (itemsOnPage(page) === 0) return 0
            const rows = rowsForPage(page)
            return rows * slotHeightForPage(page)
        }
        width: root.width
        height: {
            let tallestPage = 0
            for (let page = 0; page < pageCount; ++page)
                tallestPage = Math.max(tallestPage, pageHeightForPage(page))
            return tallestPage
        }
        contentWidth: pageCount * pageW
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 2000
        interactive: pageCount > 1

        // The page the wall has SETTLED on (what hosts read) ...
        property int currentPage: 0
        // ... and the page it is nearest to RIGHT NOW, which is what the dots show, so they follow a
        // drag and the snap animation instead of waiting for it to finish.
        readonly property int shownPage: Math.max(0, Math.min(pageCount - 1, Math.round(contentX / pageW)))
        function snapToPage() {
            const delta = contentX - currentPage * pageW
            let target = currentPage
            if (Math.abs(delta) > pageW * 0.16)
                target = currentPage + (delta > 0 ? 1 : -1)
            root.goTo(target)
        }
        onDragEnded: snapToPage()
        onFlickEnded: snapToPage()

        // The mouse wheel turns the page too (one page per notch, with a short cool-down so a fast
        // wheel does not skip several).
        WheelHandler {
            enabled: wall.pageCount > 1
            onWheel: (ev) => {
                if (wheelCool.running)
                    return
                const d = ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.angleDelta.x
                if (d === 0)
                    return
                root.goTo(wall.currentPage + (d < 0 ? 1 : -1))
                wheelCool.restart()
            }
        }
        Timer { id: wheelCool; interval: 280 }

        NumberAnimation {
            id: snapAnim
            target: wall
            property: "contentX"
            duration: 180
            easing.type: Easing.OutCubic
        }

        function goTo(page) {
            currentPage = Math.max(0, Math.min(pageCount - 1, page))
            snapAnim.to = currentPage * pageW
            snapAnim.restart()
        }
        // Keep the selected page aligned if its viewport changes size.
        onPageWChanged: {
            snapAnim.stop()
            contentX = currentPage * pageW
        }
        onPageCountChanged: root.goTo(Math.min(currentPage, pageCount - 1))

        Repeater {
            model: OutputListModel

            delegate: OutputMonitorTile {
                required property int index
                objectName: "selfTestMonitorTile" + index
                readonly property int displayIndex: wall.enabledIndex(index)
                visible: displayIndex >= 0
                readonly property int page: wall.count === 1 || displayIndex < 0 ? 0 : Math.floor(displayIndex / wall.perPage)
                readonly property int slot: wall.count === 1 || displayIndex < 0 ? 0 : displayIndex % wall.perPage
                readonly property real tileWidth: wall.slotWidthForPage(page)

                x: wall.slotXForPage(page, slot)
                y: wall.slotYForPage(page, slot) + wall.headerH
                width: tileWidth
            }
        }

        // ---- NAME STRIPS ---------------------------------------------------------
        // One slim label above each enabled tile. Click it or the picture to select the output;
        // the small switch at the right stops or starts that output.
        Repeater {
            model: OutputListModel

            delegate: Item {
                id: nameCard
                required property int index
                required property string name
                required property bool active
                required property bool isEnabled
                readonly property int displayIndex: wall.enabledIndex(index)
                visible: displayIndex >= 0

                readonly property int page: wall.count === 1 || displayIndex < 0 ? 0 : Math.floor(displayIndex / wall.perPage)
                readonly property int slot: wall.count === 1 || displayIndex < 0 ? 0 : displayIndex % wall.perPage
                readonly property real tileWidth: wall.slotWidthForPage(page)

                x: wall.slotXForPage(page, slot)
                y: wall.slotYForPage(page, slot)
                width: tileWidth
                height: wall.headerH

                HoverHandler { id: stripHover }

                // A darker ground than the tile beside it, so the name reads as its own strip.
                Rectangle {
                    anchors.fill: parent
                    anchors.bottomMargin: 1
                    radius: 4
                    color: Theme.windowBg
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade } }
                }

                // Select (strip).
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: OutputListModel.setActive(nameCard.index)
                }

                Rectangle {
                    id: stateDot
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6; height: 6; radius: 3
                    color: !nameCard.isEnabled ? Theme.border
                         : root.live ? Theme.success : Theme.textMuted
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade } }
                    SequentialAnimation on opacity {
                        running: nameCard.isEnabled && root.live
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.35; duration: 800 }
                        NumberAnimation { to: 1.0; duration: 800 }
                    }
                }

                HoverDetail {
                    anchors.left: stateDot.right
                    anchors.leftMargin: 7
                    anchors.right: powerBtn.left
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height
                    title: nameCard.name
                    detail: nameCard.active
                            ? qsTr("Selected output. Click another output to switch the monitor.")
                            : qsTr("Click to select this output for the monitor controls.")
                    Text {
                        anchors.fill: parent
                        text: nameCard.name
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        color: nameCard.isEnabled ? (nameCard.active ? Theme.textPrimary : Theme.textSecondary)
                                                  : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: nameCard.active ? Font.DemiBold : Font.Medium
                    }
                }

                // Stop / start this output. Dimmed glyph = the output is off.
                Item {
                    id: powerBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18; height: 18
                    opacity: (stripHover.hovered || !nameCard.isEnabled) ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    IconGlyph {
                        anchors.centerIn: parent
                        name: nameCard.isEnabled ? "eye" : "eyeOff"
                        color: powerArea.containsMouse ? Theme.textPrimary
                             : nameCard.isEnabled ? Theme.textSecondary : Theme.textMuted
                        width: 12; height: 12; fit: true
                        strokeWidth: root.toolIconStroke
                    }
                    MouseArea {
                        id: powerArea
                        anchors.fill: parent
                        anchors.margins: -2
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setOutputEnabled(nameCard.index, !nameCard.isEnabled)
                    }
                    HoverDetail {
                        anchors.fill: parent
                        title: nameCard.isEnabled ? qsTr("Stop this output") : qsTr("Start this output")
                        detail: nameCard.isEnabled
                                ? qsTr("Take this output off the wall and the screen")
                                : qsTr("Bring this output back")
                    }
                }
            }
        }
    }

    // Page dots — one per page, current page lit; click to swipe. Drawn
    // BELOW wall.height (outside bounds; root doesn't clip).
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + wall.height + 6
        visible: wall.pageCount > 1
        spacing: 6

        Repeater {
            model: wall.pageCount

            delegate: Rectangle {
                required property int index
                readonly property bool here: index === wall.shownPage

                width: here ? 18 : 6; height: 6; radius: 3
                color: here ? Theme.accent : Theme.border
                Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.goTo(parent.index)
                }
            }
        }
    }

    // ---- Preview chrome (FreeShow's ShowActions + ClearButtons, rebuilt) ----
    Rectangle {
        id: toolbar
        visible: root.anythingOnAir
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + root.belowWallY
        width: root.width
        height: transportRow.height + 10
        radius: 8
        color: Theme.panelBg
        border.color: Theme.border
        border.width: 1

        readonly property bool atStart: LiveOutputService.onAirIndex <= 0
        readonly property bool atEnd: LiveOutputService.onAirTotal > 0 && LiveOutputService.onAirIndex >= LiveOutputService.onAirTotal - 1
        readonly property bool onAir: LiveOutputService.onAirTotal > 0
        readonly property bool multiSlide: LiveOutputService.onAirTotal > 1
        readonly property bool canStep: toolbar.multiSlide || LiveOutputService.wallSteppable

        component ToolButton: Item {
            id: toolBtn
            property string icon: ""
            property string tipTitle: ""
            property string tipDetail: ""
            property bool enabled2: true
            property bool danger: false
            property bool lit: false
            property bool active: enabled2
            signal picked()
            width: 36; height: root.actionButtonHeight
            Rectangle {
                anchors.fill: parent; radius: 8
                color: {
                    if (!toolBtn.enabled2) return Theme.inset
                    if (toolBtn.danger)
                        return toolBtn.active
                            ? (toolArea.hovered ? Qt.alpha(Theme.danger, 0.20) : Qt.alpha(Theme.danger, 0.15))
                            : (toolArea.hovered ? Theme.chip : Theme.inset)
                    if (toolBtn.lit)
                        return toolArea.hovered ? Qt.alpha(Theme.accent, 0.22) : Qt.alpha(Theme.accent, 0.15)
                    return toolArea.hovered ? Theme.chip : Theme.inset
                }
                border.color: {
                    if (!toolBtn.enabled2) return Theme.chip
                    if (toolBtn.danger)
                        return toolBtn.active
                            ? (toolArea.hovered ? Qt.alpha(Theme.danger, 0.40) : Qt.alpha(Theme.danger, 0.20))
                            : (toolArea.hovered ? Qt.alpha(Theme.danger, 0.22) : Qt.alpha(Theme.danger, 0.14))
                    if (toolBtn.lit)
                        return toolArea.hovered ? Qt.alpha(Theme.accent, 0.42) : Qt.alpha(Theme.accent, 0.28)
                    return toolArea.hovered ? Theme.borderSubtle : Theme.chip
                }
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            }
            IconGlyph {
                x: Math.round(toolBtn.x + (toolBtn.width - width) / 2) - toolBtn.x
                y: Math.round((toolBtn.height - height) / 2)
                name: toolBtn.icon
                color: !toolBtn.enabled2 ? Theme.textMuted
                     : (toolBtn.danger ? (toolBtn.active ? Theme.dangerLight : Theme.textMuted)
                                       : (toolBtn.lit ? Theme.accent : Theme.textPrimary))
                width: root.toolIconSize; height: root.toolIconSize; fit: true
                strokeWidth: root.toolIconStroke
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            }
            HoverHandler { id: toolArea; cursorShape: toolBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (toolBtn.enabled2) toolBtn.picked() }
            HoverDetail {
                anchors.fill: parent
                title: toolBtn.tipTitle
                detail: toolBtn.tipDetail
            }
        }

        component RowButton: Item {
            id: rowBtn
            property string icon: ""
            property string caption: ""
            property string tipTitle: ""
            property string tipDetail: ""
            property color tint: Theme.dangerLight
            property bool enabled2: true
            signal picked()
            height: root.actionButtonHeight
            Rectangle {
                anchors.fill: parent; radius: 8
                color: !rowBtn.enabled2 ? Theme.inset : Qt.alpha(Theme.accent, 0.11)
                border.color: !rowBtn.enabled2 ? Theme.chip : Qt.alpha(Theme.accent, 0.20)
                border.width: 1

                // The pointer's light is the app's shared hover layer (the
                // disabled state above stays the button's own).
                HoverGlow {
                    anchors.fill: parent
                    radius: parent.radius
                    hovered: rowArea.hovered && rowBtn.enabled2
                    glowColor: Qt.alpha(Theme.accent, 0.07)
                    edge: true
                    edgeColor: Qt.alpha(Theme.accent, 0.20)
                }
            }
            Row {
                anchors.centerIn: parent
                spacing: 4
                IconGlyph {
                    name: rowBtn.icon
                    color: rowBtn.enabled2 ? rowBtn.tint : Theme.textMuted
                    width: root.toolIconSize; height: root.toolIconSize; fit: true
                    strokeWidth: root.toolIconStroke
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: rowBtn.caption
                    color: rowBtn.enabled2 ? rowBtn.tint : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            HoverHandler { id: rowArea; cursorShape: rowBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (rowBtn.enabled2) rowBtn.picked() }
            HoverDetail {
                anchors.fill: parent
                title: rowBtn.tipTitle !== "" ? rowBtn.tipTitle : rowBtn.caption
                detail: rowBtn.tipDetail
            }
        }

        Column {
            id: transportRow
            x: 8; y: 6
            width: parent.width - 16
            spacing: 4

            Row {
                width: parent.width
                spacing: 4
                readonly property real btnW: (width - spacing * 4) / 5

                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "previous"
                    enabled2: toolbar.canStep && (root.scrollMode || !(toolbar.multiSlide && toolbar.atStart))
                    onPicked: {
                        if (root.scrollMode) {
                            LiveOutputService.lineStep(-1)
                            return
                        }
                        LiveOutputService.stepPassage(-1)
                    }
                    objectName: "selfTestWallPrev"
                    tipTitle: qsTr("Previous")
                    tipDetail: root.scrollMode
                               ? qsTr("Scroll back one line (at the passage's start: the previous passage)")
                               : qsTr("Step back to the previous slide or passage")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "next"
                    enabled2: toolbar.canStep && (root.scrollMode || !(toolbar.multiSlide && toolbar.atEnd))
                    onPicked: {
                        if (root.scrollMode) {
                            LiveOutputService.lineStep(1)
                            return
                        }
                        LiveOutputService.stepPassage(1)
                    }
                    objectName: "selfTestWallNext"
                    tipTitle: qsTr("Next")
                    tipDetail: root.scrollMode
                               ? qsTr("Scroll forward one line (at the passage's end: the next passage)")
                               : qsTr("Step forward to the next slide or passage")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "play"
                    enabled2: true
                    onPicked: {
                        if (!LiveOutputService.live) { LiveOutputService.goLive(); return }
                        LiveOutputService.jumpTo(0)
                    }
                    tipTitle: qsTr("Play")
                    tipDetail: LiveOutputService.live
                               ? qsTr("Restart the on-air set from its first slide")
                               : qsTr("Go live with whatever is staged")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "unlocked"
                    enabled2: false
                    tipTitle: qsTr("Lock output")
                    tipDetail: qsTr("Not available yet")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "textLines"
                    enabled2: toolbar.canStep
                    lit: root.scrollMode
                    objectName: "selfTestWallScrollMode"
                    onPicked: {
                        const enabled = !root.scrollMode
                        LiveOutputService.scrollMode = enabled
                        EventBus.notify(enabled
                            ? qsTr("Scroll mode on — the arrows move through the reading text.")
                            : qsTr("Slide mode on — the arrows change slides."),
                            "info", qsTr("Monitor"), "ui.monitor.mode")
                    }
                    tipTitle: root.scrollMode ? qsTr("Scroll mode") : qsTr("Slide mode")
                    tipDetail: root.scrollMode
                               ? qsTr("The arrows scroll the passage. Click to switch to slides.")
                               : qsTr("The arrows change slides. Click to switch to scrolling.")
                }
            }

            Row {
                width: parent.width; height: visible ? root.actionButtonHeight : 0
                visible: root.showClearControls
                spacing: 4

                RowButton {
                    width: (parent.width - parent.spacing) / 2
                    icon: "clearIcon"
                    caption: qsTr("Clear all")
                    onPicked: LiveOutputService.clearAll()
                    tipDetail: qsTr("Clear everything on air — slide, media, overlays and audio")
                }
                RowButton {
                    width: (parent.width - parent.spacing) / 2
                    icon: "layers"
                    caption: qsTr("Show all")
                    // Lit only while a Clear all is undoable; consumed by the
                    // press, so the pair can't be pressed twice for one clear.
                    enabled2: LiveOutputService.canRestoreCleared
                    tint: Theme.textPrimary
                    onPicked: LiveOutputService.restoreCleared()
                    tipDetail: qsTr("Bring back what the last Clear all took off air")
                }
            }

            RowButton {
                width: parent.width
                height: visible ? root.actionButtonHeight : 0
                visible: root.showClearControls
                icon: "ban"
                caption: LiveOutputService.blackedOut ? qsTr("End blackout")
                                                      : qsTr("Blackout")
                tint: LiveOutputService.blackedOut ? Theme.textPrimary : Theme.dangerLight
                enabled2: root.live
                objectName: "selfTestWallBlackout"
                onPicked: {
                    if (LiveOutputService.blackedOut) LiveOutputService.endBlackout()
                    else LiveOutputService.blackout()
                }
                tipDetail: LiveOutputService.blackedOut
                           ? qsTr("Bring the picture back — the show keeps running")
                           : qsTr("Hide the picture without stopping the show")
            }

            Row {
                width: parent.width; height: visible ? root.actionButtonHeight : 0
                visible: root.showClearControls
                spacing: 4
                readonly property real btnW: (width - spacing * 4) / 5

                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "image"
                    danger: true
                    enabled2: (LiveOutputService.mediaOnAir && !LiveOutputService.mediaIsAudio) || root.inputTaken
                    onPicked: {
                        if (LiveOutputService.mediaOnAir && !LiveOutputService.mediaIsAudio) LiveOutputService.clearMedia()
                        if (root.inputTaken) LiveOutputService.clearInput()
                    }
                    tipTitle: qsTr("Clear background")
                    tipDetail: qsTr("Clear the on-air media, or a taken camera/screen input")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "scripture"
                    danger: true
                    enabled2: root.hasRealOnAirContent || root.staged
                    onPicked: {
                        if (root.staged) LiveOutputService.clearStaged()
                        if (root.hasRealOnAirContent) LiveOutputService.clearOnAirSlide()
                    }
                    tipTitle: qsTr("Clear slide")
                    tipDetail: qsTr("Clear the on-air text, scripture or song content")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "overlays"
                    danger: true
                    enabled2: LiveOutputService.activeOverlays.length > 0
                    onPicked: LiveOutputService.clearAllOverlays()
                    tipTitle: qsTr("Clear overlays")
                    tipDetail: qsTr("Take every active overlay off air")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "audio"
                    danger: true
                    enabled2: (LiveOutputService.mediaOnAir && LiveOutputService.mediaIsAudio)
                              || root.inputTaken
                    onPicked: {
                        if (LiveOutputService.mediaOnAir) LiveOutputService.clearMedia()
                    }
                    tipTitle: qsTr("Clear audio")
                    tipDetail: qsTr("Clear the on-air audio file or a taken input")
                }
                ToolButton {
                    width: parent.btnW; height: root.actionButtonHeight
                    icon: "timerFill"
                    danger: true
                    enabled2: false
                    tipTitle: qsTr("Clear timer")
                    tipDetail: qsTr("Not available yet")
                }
            }

            // ---- Audio transport -----------------------------------------
            Item {
                id: audioBar
                objectName: "selfTestWallAudioBar"
                width: parent.width
                height: visible ? root.actionButtonHeight : 0
                visible: root.showClearControls && LiveOutputService.mediaOnAir && LiveOutputService.mediaIsAudio

                readonly property bool playing: LiveOutputService.mediaState === "playing"
                readonly property real progress: LiveOutputService.mediaDuration > 0
                    ? Math.max(0, Math.min(1, LiveOutputService.mediaPosition / LiveOutputService.mediaDuration))
                    : 0

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: Theme.inset
                    border.color: Theme.chip
                    border.width: 1
                }

                Item {
                    id: audioToggle
                    x: 8; y: 0
                    width: 22; height: parent.height
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "pause"
                        color: Theme.dangerLight
                        width: root.toolIconSize; height: root.toolIconSize; fit: true
                        strokeWidth: root.toolIconStroke
                        opacity: audioBar.playing ? 1 : 0
                        scale: audioBar.playing ? 1 : 0.6
                        Behavior on opacity { NumberAnimation { duration: 140 } }
                        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "play"
                        color: Theme.dangerLight
                        width: root.toolIconSize; height: root.toolIconSize; fit: true
                        strokeWidth: root.toolIconStroke
                        opacity: audioBar.playing ? 0 : 1
                        scale: audioBar.playing ? 0.6 : 1
                        Behavior on opacity { NumberAnimation { duration: 140 } }
                        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: LiveOutputService.mediaTogglePlay() }
                }

                Text {
                    id: audioElapsed
                    anchors.left: audioToggle.right
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: LiveOutputService.formatMediaTime(LiveOutputService.mediaPosition)
                    color: Theme.dangerLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                Text {
                    id: audioTotal
                    anchors.right: audioMute.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: LiveOutputService.formatMediaTime(LiveOutputService.mediaDuration)
                    color: Theme.dangerLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                Item {
                    id: seekTrack
                    anchors.left: audioElapsed.right
                    anchors.right: audioTotal.left
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    height: 16

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 4; radius: 2
                        color: Theme.chip
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * audioBar.progress
                        height: 4; radius: 2
                        color: Theme.danger
                    }
                    Rectangle {
                        x: Math.max(0, Math.min(parent.width - width, parent.width * audioBar.progress - width / 2))
                        anchors.verticalCenter: parent.verticalCenter
                        width: 9; height: 9; radius: 4.5
                        color: Theme.dangerLight
                    }

                    function seekTo(x) {
                        if (seekTrack.width <= 0) return
                        LiveOutputService.mediaSeek(Math.max(0, Math.min(1, x / seekTrack.width)))
                    }
                    TapHandler {
                        onTapped: function (point) { seekTrack.seekTo(point.position.x) }
                    }
                    DragHandler {
                        onCentroidChanged: if (active) seekTrack.seekTo(centroid.position.x)
                    }
                }

                // Mute — dimmed glyph means muted, the same read the Media
                // pane's speaker uses.
                Item {
                    id: audioMute
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    width: 22; height: parent.height
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "volume2"
                        color: LiveOutputService.mediaMuted ? Theme.textMuted : Theme.dangerLight
                        width: root.toolIconSize; height: root.toolIconSize; fit: true
                        strokeWidth: root.toolIconStroke
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: LiveOutputService.mediaToggleMuted() }
                }
            }
        }
    }

    onVisibleChanged: if (visible)
        root.height = Qt.binding(() => root.wallHeight)
}