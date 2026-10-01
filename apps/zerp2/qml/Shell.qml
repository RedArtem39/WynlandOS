// Zerp 2.0 shell: wallpaper, macOS-style top bar, windows.
// Windows are QML stand-ins for now; real Zerp clients become the same
// AppWindow items (their pixels as textures) in the next step.
import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: shell
    visible: true
    visibility: Window.FullScreen
    color: "#101218"
    title: "Zerp"

    property int nextZ: 10
    property string focusedTitle: "Zerp"

    function raise(w) { w.z = ++nextZ; focusedTitle = w.title }

    // ---- wallpaper (also the blur source for the top bar)
    Item {
        id: desktop
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            visible: wallpaperUrl === ""
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "#3a2f6b" }
                GradientStop { position: 0.55; color: "#1f4f7a" }
                GradientStop { position: 1.0; color: "#0e2a3d" }
            }
        }
        Image {
            anchors.fill: parent
            source: wallpaperUrl
            fillMode: Image.PreserveAspectCrop
            visible: wallpaperUrl !== ""
            asynchronous: true
        }
    }

    // ---- windows
    Item {
        id: windows
        anchors { fill: parent; topMargin: bar.height }

        AppWindow {
            title: "About WynlandOS"
            x: 120; y: 70; width: 520; height: 340
            onActivated: shell.raise(this)
            Column {
                anchors.centerIn: parent
                spacing: 10
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 84; height: 84; radius: 22
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#7aa2f7" }
                        GradientStop { position: 1; color: "#bb9af7" }
                    }
                    Text { anchors.centerIn: parent; text: "W"; color: "white"; font.pixelSize: 44; font.bold: true }
                }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "WynlandOS"; color: "#f0f0f5"; font.pixelSize: 26; font.bold: true }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Zerp 2.0 · Qt Quick on the GPU"; color: "#a9b1d6"; font.pixelSize: 14 }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "virgl · KMS · " + Screen.width + "×" + Screen.height; color: "#7f88a8"; font.pixelSize: 12 }
            }
        }

        AppWindow {
            title: "Notes"
            x: 700; y: 160; width: 460; height: 380
            onActivated: shell.raise(this)
            Rectangle {
                anchors { fill: parent; margins: 14 }
                radius: 8
                color: "#161922"
                border.color: input.activeFocus ? "#7aa2f7" : "#2a2f3d"
                TextEdit {
                    id: input
                    anchors { fill: parent; margins: 10 }
                    color: "#e6e6ee"
                    font.pixelSize: 15
                    wrapMode: TextEdit.Wrap
                    text: "Click here and type.\nDrag windows by the title bar."
                    selectByMouse: true
                }
            }
        }
    }

    // ---- top bar
    TopBar {
        id: bar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        blurSource: desktop
        appName: shell.focusedTitle
    }
}
