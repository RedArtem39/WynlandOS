// The bar: one frosted strip (macOS was the reference for the feel, not
// the layout). Left: workspaces. Centre: the focused window. Right:
// status and clock (and the FPS meter, mod+P).
import QtQuick

Item {
    id: bar
    property int fps: -1

    // frosted: our slice of the live blurred wallpaper
    Glass { anchors.fill: parent; gx: bar.x; gy: bar.y }
    Rectangle { anchors.fill: parent; color: "#0a0c10"; opacity: 0.7 }
    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: "white"; opacity: 0.07 }

    // ---- workspaces
    Row {
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 6
        Repeater {
            model: 9
            delegate: Rectangle {
                required property int index
                readonly property int ws: index + 1
                readonly property bool current: zerp.workspace === ws
                readonly property bool occupied: zerp.clients.some(function (c) { return c.workspace === ws })
                visible: current || occupied || ws <= 4
                height: 18
                width: current ? 34 : 18
                radius: 9
                color: current ? "#3d7eff" : occupied ? "#2a303b" : "#171a20"
                Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                Behavior on color { ColorAnimation { duration: 200 } }
                Text {
                    anchors.centerIn: parent
                    text: parent.ws
                    color: parent.current ? "#0a0c10" : "#8a93a3"
                    font.pixelSize: 11
                    font.bold: true
                }
                MouseArea { anchors.fill: parent; onClicked: zerp.workspace = parent.ws }
            }
        }
    }

    // ---- focused window
    Text {
        anchors.centerIn: parent
        text: zerp.focusedClient ? zerp.focusedClient.title : "WynlandOS"
        color: "#d6dbe4"
        font.pixelSize: 13
        font.bold: true
        elide: Text.ElideRight
        width: Math.min(implicitWidth, bar.width * 0.4)
        horizontalAlignment: Text.AlignHCenter
    }

    // ---- status + clock
    Row {
        anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
        spacing: 16
        Text {
            visible: bar.fps >= 0
            text: bar.fps + " fps"
            color: bar.fps >= 50 ? "#7ee787" : bar.fps >= 30 ? "#e3b341" : "#ff7b72"
            font { family: "DejaVu Sans Mono"; pixelSize: 12; bold: true }
            anchors.verticalCenter: parent.verticalCenter
        }
        Canvas {
            width: 18; height: 14
            anchors.verticalCenter: parent.verticalCenter
            onPaint: {
                var c = getContext("2d");
                c.reset();
                c.strokeStyle = "#d6dbe4"; c.lineWidth = 1.8; c.lineCap = "round";
                for (var r = 4; r <= 12; r += 4) {
                    c.beginPath(); c.arc(9, 13, r, Math.PI * 1.25, Math.PI * 1.75); c.stroke();
                }
                c.fillStyle = "#d6dbe4";
                c.beginPath(); c.arc(9, 13, 1.4, 0, Math.PI * 2); c.fill();
            }
        }
        // the user; a click logs out (wynlogin shows the login screen)
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            height: 22
            width: who.implicitWidth + 30
            radius: 11
            color: logoutArea.containsMouse ? "#1b1f27" : "transparent"
            Behavior on color { ColorAnimation { duration: 150 } }
            Rectangle {
                anchors { left: parent.left; leftMargin: 4; verticalCenter: parent.verticalCenter }
                width: 16; height: 16; radius: 8
                color: "#3d7eff"
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 9
                    font.bold: true
                    text: userName.length ? userName[0].toUpperCase() : "?"
                }
            }
            Text {
                id: who
                anchors { left: parent.left; leftMargin: 25; verticalCenter: parent.verticalCenter }
                color: "#d6dbe4"
                font.pixelSize: 13
                text: logoutArea.containsMouse ? "Log out " + userName : userName
            }
            MouseArea { id: logoutArea; anchors.fill: parent; hoverEnabled: true; onClicked: zerp.logout() }
        }
        Text {
            id: clock
            color: "#d6dbe4"
            font.pixelSize: 13
            anchors.verticalCenter: parent.verticalCenter
            function refresh() { text = Qt.formatDateTime(new Date(), "ddd d MMM   HH:mm") }
            Component.onCompleted: refresh()
            Timer { interval: 1000; running: true; repeat: true; onTriggered: clock.refresh() }
        }
    }
}
