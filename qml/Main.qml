import QtQuick
import QtQuick.Controls
import VGRPresenterUI
import "components"
import "screens"
import "screens/main"
import "screens/edit"

ApplicationWindow {
    id: window
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 640
    visible: true
    title: "VGRPresenter"
    color: Theme.windowBg

    property string settingsSection: "general"
    // "show" | "edit" | "stage" — Stage has no screen yet, so its tab click
    // is accepted but doesn't switch the view.
    property string currentView: "show"

    // Screens first (opaque, fill the window), then the shared header strip
    // and the menu layer on top — AppMenuBar must sit above AppHeader because
    // its logo/menu labels are drawn inside the same 48px strip.
    VGRPresenterMainScreen {
        anchors.fill: parent
        visible: window.currentView === "show"
    }

    EditScreen {
        anchors.fill: parent
        visible: window.currentView === "edit"
    }
    // Opens the Settings dialog — the one entry point both the header's
    // gear button and the menu bar's Settings/Preferences items funnel
    // into, so a future keyboard shortcut (Ctrl+,) only ever calls this.
    function openSettings(section) {
        if (section !== undefined)
            window.settingsSection = section
        settingsScrim.visible = true
    }

    AppHeader {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        activeTab: window.currentView
        onTabSelected: (tab) => {
            if (tab === "show" || tab === "edit") window.currentView = tab
        }
        onSettingsClicked: window.openSettings("general")
    }

    AppMenuBar {
        anchors.fill: parent
        onSettingsRequested: window.openSettings("general")
    }

    // ---- Settings overlay ----
    // Shared ModalScrim: click-dismisses, and consumes wheel events so a
    // modal's scroll never leaks into the Flickables on the page behind.
    ModalScrim {
        id: settingsScrim
        visible: false
        // ModalScrim's #99000000 is exactly the 0.6 black this overlay
        // always used — no visual change, just the shared behavior.
        onDismissed: settingsScrim.visible = false

        ModalShell {
            anchors.centerIn: parent
            currentKey: window.settingsSection
            onSectionSelected: (key) => window.settingsSection = key
            onCloseRequested: settingsScrim.visible = false
            onCancelRequested: settingsScrim.visible = false
            onSaveRequested: settingsScrim.visible = false

            Loader {
                    anchors.fill: parent
                    sourceComponent: {
                        switch (window.settingsSection) {
                        case "general": return generalScreenComponent
                        case "smart": return smartConfigScreenComponent
                        case "recording": return recordingScreenComponent
                        case "plugins": return pluginsScreenComponent
                        case "outputs": return outputsScreenComponent
                        case "styles": return stylesScreenComponent
                        case "av": return audioVideoScreenComponent
                        default: return placeholderComponent
                        }
                    }
                }
        }
    }

    Component {
        id: generalScreenComponent
        GeneralScreen {}
    }

    Component {
        id: smartConfigScreenComponent
        SmartConfigScreen {}
    }

    Component {
        id: recordingScreenComponent
        RecordingScreen {}
    }

    Component {
        id: pluginsScreenComponent
        PluginsScreen {}
    }

    Component {
        id: outputsScreenComponent
        OutputsScreen {}
    }

    Component {
        id: stylesScreenComponent
        StylesScreen {}
    }

    Component {
        id: audioVideoScreenComponent
        AudioVideoScreen {}
    }

    Component {
        id: placeholderComponent
        PlaceholderScreen {
            title: {
                switch (window.settingsSection) {
                case "outputs": return "Outputs"
                default: return window.settingsSection
                }
            }
        }
    }
}
