// The bar: one frosted strip (macOS was the reference for the feel, not
// the layout). Left: workspaces. Centre: the focused window. Right:
// status and clock.
import QtQuick
import QtQuick.Effects

Item {
    id: bar
    property Item blurSource

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
        saturation: 0.25
    }
    Rectangle { anchors.fill: parent; color: "#10131a"; opacity: 0.55 }
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
                color: current ? "#7aa2f7" : occupied ? "#3b4261" : "#262a36"
                Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                Behavior on color { ColorAnimation { duration: 200 } }
                Text {
                    anchors.centerIn: parent
                    text: parent.ws
                    color: parent.current ? "#0f1117" : "#a9b1d6"
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
        color: "#e6e8ef"
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
        Canvas {
            width: 18; height: 14
            anchors.verticalCenter: parent.verticalCenter
            onPaint: {
                var c = getContext("2d");
                c.reset();
                c.strokeStyle = "#e6e8ef"; c.lineWidth = 1.8; c.lineCap = "round";
                for (var r = 4; r <= 12; r += 4) {
                    c.beginPath(); c.arc(9, 13, r, Math.PI * 1.25, Math.PI * 1.75); c.stroke();
                }
                c.fillStyle = "#e6e8ef";
                c.beginPath(); c.arc(9, 13, 1.4, 0, Math.PI * 2); c.fill();
            }
        }
        Text {
            id: clock
            color: "#e6e8ef"
            font.pixelSize: 13
            anchors.verticalCenter: parent.verticalCenter
            function refresh() { text = Qt.formatDateTime(new Date(), "ddd d MMM   HH:mm") }
            Component.onCompleted: refresh()
            Timer { interval: 1000; running: true; repeat: true; onTriggered: clock.refresh() }
        }
    }
}
