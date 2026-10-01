// macOS-style menu bar: translucent blurred strip, logo menu + app name
// and menus on the left, status items and the clock on the right.
import QtQuick
import QtQuick.Effects

Item {
    id: bar
    height: 30
    property Item blurSource
    property string appName: ""

    // frosted glass: the wallpaper strip under the bar, blurred
    ShaderEffectSource {
        id: strip
        anchors.fill: parent
        sourceItem: bar.blurSource
        sourceRect: Qt.rect(0, 0, bar.width, bar.height)
        visible: false
    }
    MultiEffect {
        anchors.fill: parent
        source: strip
        blurEnabled: true
        blur: 1.0
        blurMax: 48
        saturation: 0.3
    }
    Rectangle { anchors.fill: parent; color: "#1c1e26"; opacity: 0.55 }
    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: "#ffffff"; opacity: 0.08 }

    // ---- left: logo menu, app name, menus
    Row {
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 18

        Item {
            width: 18; height: 18
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.fill: parent; radius: 5
                gradient: Gradient {
                    GradientStop { position: 0; color: "#7aa2f7" }
                    GradientStop { position: 1; color: "#bb9af7" }
                }
            }
            Text { anchors.centerIn: parent; text: "W"; color: "white"; font.pixelSize: 11; font.bold: true }
            MouseArea { anchors.fill: parent; onClicked: logoMenu.visible = !logoMenu.visible }
        }
        Text { text: bar.appName; color: "white"; font.pixelSize: 13; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
        Repeater {
            model: ["File", "Edit", "View", "Window", "Help"]
            delegate: Text {
                required property string modelData
                text: modelData; color: "#e8e8ee"; font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // ---- right: status + clock
    Row {
        anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 16

        // wifi: three arcs
        Canvas {
            width: 18; height: 14
            anchors.verticalCenter: parent.verticalCenter
            onPaint: {
                var c = getContext("2d");
                c.reset();
                c.strokeStyle = "#e8e8ee"; c.lineWidth = 1.8; c.lineCap = "round";
                for (var r = 4; r <= 12; r += 4) {
                    c.beginPath();
                    c.arc(9, 13, r, Math.PI * 1.25, Math.PI * 1.75);
                    c.stroke();
                }
                c.fillStyle = "#e8e8ee";
                c.beginPath(); c.arc(9, 13, 1.4, 0, Math.PI * 2); c.fill();
            }
        }
        // battery
        Item {
            width: 26; height: 12
            anchors.verticalCenter: parent.verticalCenter
            Rectangle { width: 23; height: 12; radius: 3; color: "transparent"; border.color: "#e8e8ee"; border.width: 1.2 }
            Rectangle { x: 2; y: 2; width: 15; height: 8; radius: 1.5; color: "#e8e8ee" }
            Rectangle { x: 24; y: 4; width: 2; height: 4; radius: 1; color: "#e8e8ee" }
        }
        Text {
            id: clock
            color: "white"; font.pixelSize: 13
            anchors.verticalCenter: parent.verticalCenter
            function refresh() { text = Qt.formatDateTime(new Date(), "ddd d MMM  HH:mm") }
            Component.onCompleted: refresh()
            Timer { interval: 1000; running: true; repeat: true; onTriggered: clock.refresh() }
        }
    }

    // ---- logo dropdown
    Rectangle {
        id: logoMenu
        visible: false
        x: 8; y: bar.height + 4
        width: 220; height: col.implicitHeight + 12
        radius: 10
        color: "#262833"
        border.color: "#3a3d4c"
        z: 100
        Column {
            id: col
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
            Repeater {
                model: ["About This Computer", "-", "System Settings…", "-", "Restart…", "Shut Down…"]
                delegate: Item {
                    required property string modelData
                    width: col.width
                    height: modelData === "-" ? 9 : 26
                    Rectangle {
                        visible: modelData === "-"
                        anchors.centerIn: parent; width: parent.width - 8; height: 1; color: "#3a3d4c"
                    }
                    Rectangle {
                        visible: modelData !== "-" && hover.containsMouse
                        anchors.fill: parent; radius: 5; color: "#4a6fd1"
                    }
                    Text {
                        visible: modelData !== "-"
                        text: modelData; color: "#ececf2"; font.pixelSize: 13
                        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    }
                    MouseArea {
                        id: hover; anchors.fill: parent; hoverEnabled: true
                        onClicked: logoMenu.visible = false
                    }
                }
            }
        }
    }
}
