pragma Singleton
import QtQuick
import VGRPresenterUI

QtObject {
    id: theme

    // ---- Theme (Settings > General > Appearance > Theme) ----
    readonly property string themeName: {
        const v = SettingsService.values["appearance.theme"]
        return v === undefined || v === null ? "dark" : String(v)
    }
    readonly property bool dark: themeName !== "light"

    // ---- Brand ----
    // The accent is the user's choice (Settings > General > Accent color): the ENGINE keeps it and SettingsService hands the colours over,
    // so every Theme.accent / accentLight / accentSoft in the interface follows the choice. (Purple until the engine is up.)
    readonly property color accent: SettingsService.accent
    readonly property color accentLight: SettingsService.accentLight
    readonly property color accentSoft: Qt.alpha(SettingsService.accent, 0.15)   // the tint behind a selected chip or row

    // ---- Semantic ----
    readonly property color danger: "#ff2b2b"
    readonly property color dangerLight: "#ff6b6b"
    // GO LIVE is red in BOTH themes (a live switch reads as red the way a
    // recording dot does), so these are fixed semantic hues, not neutrals.
    readonly property color liveBg: "#b03630"        // GO LIVE pill, at rest
    readonly property color liveBgOnAir: "#7a2a24"   // GO LIVE pill, on air
    readonly property color success: "#4ade80"
    readonly property color successLight: "#7ee2a8"
    readonly property color warning: "#f5c26b"
    readonly property color warningLight: "#ffb454"
    readonly property color highlightInk: "#ffd54a"
    readonly property color markedInk: "#f5c542"
    readonly property color info: "#4da6ff"
    readonly property color infoLight: "#8ecbff"
    readonly property color meterQuiet: "#00c8c8"   // signal present, low level
    readonly property color meterLoud: "#00ff32"    // healthy speech/music
    readonly property color meterHot: "#ffc800"     // near the top of the scale
    readonly property color meterClip: "#c80000"    // at/over 0 dBFS

    // ---- Routing wires (Settings · Routing) ----
    readonly property color wireAudio: "#b45cf5"
    readonly property color wireVideo: "#ff7a45"
    readonly property color wireStyle: info

    // ---- Channel state (one vocabulary for off / trimmed / at unity) ----
    readonly property color channelOff: textMuted
    readonly property color channelTrimmed: warning
    readonly property color channelUnity: success

    function channelStateColor(state) {
        return state === "off" ? channelOff
             : state === "trimmed" ? channelTrimmed
             : channelUnity
    }

    // ---- Armed (staged) surfaces ----
    readonly property color armedReadyBg: "#1d2a4d"
    readonly property color armedReadyInk: "#8fb4ff"
    readonly property color armedStopBg: "#241a19"
    readonly property color armedLiveBg: "#2a1210"

    // ---- Neutrals ----
    readonly property color windowBg: dark ? "#0f1015" : "#f4f5f8"   // app root background
    readonly property color panelBg: dark ? "#12131a" : "#ffffff"    // app-header / side columns
    readonly property color workspacePaneBg: dark ? "#1b1d26" : "#ffffff" // alternating home workspace panes
    readonly property color referenceContentBg: dark ? "#1b1d26" : "#ffffff" // Scripture / The Table reading area
    readonly property color rowBg: dark ? "#16171e" : "#ffffff"      // list rows, cards, hero bg
    readonly property color surface: dark ? "#15161d" : "#ffffff"    // Settings modal surface
    readonly property color card: dark ? "#191b24" : "#ffffff"       // Settings card background
    readonly property color inset: dark ? "#1a1c26" : "#f1f2f6"      // input/inset fields
    readonly property color chip: dark ? "#232733" : "#e8eaf0"       // neutral tag chips
    readonly property color border: dark ? "#333948" : "#e2e5ec"     // borders, dividers, hover bg
    readonly property color borderSubtle: dark ? "#2c3140" : "#d7dbe4"
    // ---- App chrome (title bar, tab strip, menu bar, dropdowns) ----
    readonly property color hoverBg: dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.05)
    readonly property color activeBg: dark ? "#1e1f29" : "#e9ebf1"    // active tab pill
    readonly property color iconChrome: dark ? "#b4bccb" : "#5a6272"  // header icon at rest

    readonly property color textPrimary: dark ? "#e2e8f0" : "#16181f"
    readonly property color textSecondary: dark ? "#8a94a6" : "#5b6472"
    readonly property color textMuted: dark ? "#747e92" : "#7b8496"

    // ---- Nav rail (Settings) ----
    readonly property color iconMuted: dark ? "#8b93a7" : "#6b7484"      // unselected nav/glyph icon
    readonly property color navLabelMuted: dark ? "#c7cdd8" : "#3a4150"  // unselected nav item label; also the app's mid-emphasis label tone (a control's own body/knob)
    readonly property color navChipBg: dark ? "#1b1d27" : "#eceef3"      // unselected icon-chip background
    readonly property color railDivider: dark ? "#23252f" : "#e2e5ec"    // nav rail edge line
    readonly property color footerSupport: dark ? "#6f7788" : "#6b7484"  // nav rail "Support" text
    readonly property color footerVersion: dark ? "#4a5060" : "#8b93a7"  // nav rail version text
    readonly property color toggleOffTrack: dark ? "#2a2f3a" : "#d3d7e0" // ToggleSwitch off-state track; also a slider's unfilled rail

    // ---- Control surfaces ----
    readonly property color controlBg: dark ? "#3a3f55" : "#e8eaf0"      // raised neutral pill/button body
    readonly property color controlBorder: dark ? "#4a4f66" : "#cfd5e2"  // its border, and an unchecked box's
    // NOT named `onAccent`: any `onXxx` name is read by QML as a signal handler
    // (`Cannot assign a value to a signal`), which failed the build's QML load.
    readonly property color controlInk: "#ffffff"                        // ink on a filled control (a primary button, an accent-filled checkbox, a meter marker) — white in BOTH themes

    // ---- Spacing scale ----
    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space5: 20
    readonly property int space6: 24
    readonly property int space8: 32

    // ---- Hover motion ----------------------------------------------------------
    //   1. a hovered surface is lit by ONE overlay layer whose OPACITY fades
    //   2. everything times to the constants below, so changing the feel of the
    //      app's hover is a one-line change and no surface can drift.
    readonly property int hoverFade: 220        // shared hover timing, including tabs
    readonly property var hoverEasing: Easing.InOutQuad
    readonly property real hoverEdgeAlpha: 0.45

    // ---- Radius scale ----
    readonly property int radiusSm: 6
    readonly property int radiusMd: 8
    readonly property int radiusLg: 10
    readonly property int radiusXl: 16

    // ---- Typography ----
    readonly property string fontFamily: FontService.defaultFamily
    readonly property int textXs: 10
    readonly property int textSm: 13
    // ---- The SETTINGS SUBSCRIPT ----
    readonly property int textSub: 12
    readonly property int textMd: 15
    readonly property int textLg: 17
    readonly property int textXl: 23
    readonly property int textXxl: 25
}