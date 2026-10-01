// WynlandOS - Qt Quick demo (Zerp 2.0 groundwork).
// Exercises what a shell needs: layouts, controls, text input, a list,
// animations, timers and JS.
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: root
    visible: true
    width: Screen.width > 0 ? Screen.width : 800
    height: Screen.height > 0 ? Screen.height : 600
    color: "#0f1117"
    title: "QML on WynlandOS"

    property int clicks: 0

    // clock bar -- the kind of thing a shell panel shows
    Rectangle {
        id: bar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 40
        color: "#181b24"
        Text {
            id: clock
            anchors.centerIn: parent
            color: "#e6e6e6"
            font.pixelSize: 18
            text: Qt.formatTime(new Date(), "hh:mm:ss")
        }
        Text {
            anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
            color: "#7aa2f7"
            font.pixelSize: 16
            font.bold: true
            text: "WynlandOS · Qt Quick"
        }
        Timer {
            interval: 1000; running: true; repeat: true
            onTriggered: clock.text = Qt.formatTime(new Date(), "hh:mm:ss")
        }
    }

    RowLayout {
        anchors { top: bar.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 16 }
        spacing: 16

        // left: controls
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: false   // layouts fill by default; leave room for the right column
            Layout.preferredWidth: root.width * 0.45
            spacing: 12

            Label { text: "Controls"; color: "#c0caf5"; font.pixelSize: 20 }

            Button {
                text: "Click me (" + root.clicks + ")"
                onClicked: { root.clicks++; spinner.rotation += 45 }
            }
            Slider {
                id: slider
                Layout.fillWidth: true
                from: 0; to: 100; value: 40
            }
            Label { text: "slider: " + Math.round(slider.value); color: "#a9b1d6" }
            Switch { id: sw; text: "Animate"; checked: true }
            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: "type here, Shift works too"
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#a9b1d6"
                text: input.text.length ? "you typed: " + input.text : ""
            }
            Item { Layout.fillHeight: true }
        }

        // right: animation + list
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                radius: 12
                color: "#1a1e2b"
                Rectangle {
                    id: spinner
                    width: 60 + slider.value; height: width
                    anchors.centerIn: parent
                    radius: 10
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#7aa2f7" }
                        GradientStop { position: 1.0; color: "#bb9af7" }
                    }
                    Behavior on rotation { NumberAnimation { duration: 300; easing.type: Easing.OutBack } }
                    RotationAnimation on rotation {
                        running: sw.checked
                        loops: Animation.Infinite
                        from: 0; to: 360; duration: 4000
                    }
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: 30
                delegate: Rectangle {
                    required property int index
                    width: ListView.view.width
                    height: 32
                    radius: 6
                    color: mouse.containsMouse ? "#2a3045" : "#1a1e2b"
                    Text {
                        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                        color: "#c0caf5"
                        text: "Item " + index
                    }
                    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true }
                }
            }
        }
    }
}
