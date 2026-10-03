pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ---- Theme (Settings > General > Appearance > Theme) ----
    // The ENGINE owns the choice ("appearance.theme": "dark" | "light"). This
    // singleton only turns it into token values, so every token below is a LIVE
    // binding: picking Light in Settings recolours every surface that reads a
    // Theme token, with no restart. Until the engine is up the choice reads
    // undefined and the app stays dark (its default).
    //
    // Every surface follows the choice now, including the app chrome (title
    // bar, tab strip, menu bar, dropdowns): those files used to carry their
    // own dark hex literals because this singleton was registered as an
    // ORDINARY type rather than a singleton (see QT_QML_SINGLETON_TYPE in
    // CMakeLists.txt), so `Theme.*` resolved to undefined and they isolated
    // themselves. With the registration fixed they read tokens like any
    // other surface.
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
    // Deliberately the SAME in both themes: these are fixed status hues, and
    // every one of them is drawn on its own tinted or neutral chip rather than
    // directly on the page ground. (The on-air output windows do not read Theme
    // at all, so a light app theme can never lighten the stage output.)
    readonly property color danger: "#ff4d3d"
    readonly property color dangerLight: "#ff6b61"
    // GO LIVE is red in BOTH themes (a live switch reads as red the way a
    // recording dot does), so these are fixed semantic hues, not neutrals.
    readonly property color liveBg: "#b03630"        // GO LIVE pill, at rest
    readonly property color liveBgOnAir: "#7a2a24"   // GO LIVE pill, on air
    readonly property color success: "#4ade80"
    readonly property color successLight: "#7ee2a8"
    readonly property color warning: "#f5c26b"
    readonly property color info: "#4da6ff"
    readonly property color infoLight: "#8ecbff"

    // ---- Neutrals ----
    // The dark values are the Figma-to-Qt export's own, pulled directly
    // (VGRPresenter_Main_Screen.qml) rather than approximated by hand. The light
    // counterparts keep the same relationships - ground darkest, cards above it,
    // insets recessed - inverted for a light ground.
    readonly property color windowBg: dark ? "#0f1015" : "#f4f5f8"   // app root background
    readonly property color panelBg: dark ? "#12131a" : "#ffffff"    // app-header / side columns
    readonly property color rowBg: dark ? "#16171e" : "#ffffff"      // list rows, cards, hero bg
    readonly property color surface: dark ? "#15161d" : "#ffffff"    // Settings modal surface
    readonly property color card: dark ? "#191b24" : "#ffffff"       // Settings card background
    readonly property color inset: dark ? "#1a1c26" : "#f1f2f6"      // input/inset fields
    readonly property color chip: dark ? "#232733" : "#e8eaf0"       // neutral tag chips
    readonly property color border: dark ? "#232530" : "#e2e5ec"     // borders, dividers, hover bg
    readonly property color borderSubtle: dark ? "#2c3140" : "#d7dbe4"
    // ---- App chrome (title bar, tab strip, menu bar, dropdowns) ----
    // These close the last gap Light mode had: the chrome files at one point
    // hardcoded their own dark shades, so picking Light recoloured the settings
    // screens and left the header/menus dark. Their families live here now.
    readonly property color hoverBg: dark ? "#1c1d26" : "#eceef3"     // pill / menu-row hover wash
    readonly property color activeBg: dark ? "#1e1f29" : "#e9ebf1"    // active tab pill
    readonly property color iconChrome: dark ? "#b4bccb" : "#5a6272"  // header icon at rest

    readonly property color textPrimary: dark ? "#e2e8f0" : "#16181f"
    readonly property color textSecondary: dark ? "#8a94a6" : "#5b6472"
    readonly property color textMuted: dark ? "#5c6475" : "#7b8496"

    // ---- Nav rail (Settings) ----
    readonly property color iconMuted: dark ? "#8b93a7" : "#6b7484"      // unselected nav/glyph icon
    readonly property color navLabelMuted: dark ? "#c7cdd8" : "#3a4150"  // unselected nav item label
    readonly property color navChipBg: dark ? "#1b1d27" : "#eceef3"      // unselected icon-chip background
    readonly property color railDivider: dark ? "#23252f" : "#e2e5ec"    // nav rail edge line
    readonly property color footerSupport: dark ? "#6f7788" : "#6b7484"  // nav rail "Support" text
    readonly property color footerVersion: dark ? "#4a5060" : "#8b93a7"  // nav rail version text
    readonly property color toggleOffTrack: dark ? "#2a2f3a" : "#d3d7e0" // ToggleSwitch off-state track

    // ---- Spacing scale ----
    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space5: 20
    readonly property int space6: 24
    readonly property int space8: 32

    // ---- Radius scale ----
    readonly property int radiusSm: 6
    readonly property int radiusMd: 8
    readonly property int radiusLg: 10
    readonly property int radiusXl: 16

    // ---- Typography ----
    // Windows' own system UI font - always installed, unlike a webfont ("Inter", this app's previous choice) that has to be
    // installed separately and silently falls back to something else when it isn't. FreeShow's own app chrome (not its output/
    // stage display, which does ship a font) takes the same approach: the OS's native font, not a bundled one.
    readonly property string fontFamily: "Segoe UI"
    readonly property int textXs: 10
    readonly property int textSm: 13
    readonly property int textMd: 15
    readonly property int textLg: 17
    readonly property int textXl: 23
    readonly property int textXxl: 25
}
