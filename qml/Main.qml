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

    VGRPresenterMainScreen {
        anchors.fill: parent
        visible: window.currentView === "show"
        onTabSelected: (tab) => {
            if (tab === "show" || tab === "edit") window.currentView = tab
        }
    }

    EditScreen {
        anchors.fill: parent
        visible: window.currentView === "edit"
        onTabSelected: (tab) => {
            if (tab === "show" || tab === "edit") window.currentView = tab
        }
    }

    // ---- Settings overlay ----
    Rectangle {
        id: settingsScrim
        anchors.fill: parent
        visible: false
        color: Qt.rgba(0, 0, 0, 0.6)

        MouseArea {
            anchors.fill: parent
            onClicked: settingsScrim.visible = false
        }

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
        id: placeholderComponent
        PlaceholderScreen {
            title: {
                switch (window.settingsSection) {
                case "smart": return "Smart Config"
                case "outputs": return "Outputs"
                case "screens": return "Screens"
                case "av": return "Audio & Video"
                case "recording": return "Recording"
                case "plugins": return "Plugins"
                default: return window.settingsSection
                }
            }
        }
    }
}
