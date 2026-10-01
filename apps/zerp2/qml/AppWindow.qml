// A Zerp 2.0 window: rounded, shadowed, macOS traffic lights, draggable
// title bar, open animation. `default property` puts children in the body.
import QtQuick
import QtQuick.Effects

Item {
    id: win
    property string title: "Window"
    default property alias content: body.data
    signal activated()

    // open animation
    scale: 0.92
    opacity: 0
    Component.onCompleted: { scale = 1; opacity = 1 }
    Behavior on scale { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
    Behavior on opacity { NumberAnimation { duration: 220 } }

    // soft drop shadow following the rounded corners (one SDF shader,
    // no offscreen pass)
    RectangularShadow {
        anchors.fill: frame
        radius: frame.radius
        offset.y: 12
        blur: 36
        spread: 2
        color: Qt.rgba(0, 0, 0, 0.55)
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: 12
        color: "#1d2029"
        border.color: "#ffffff"
        border.width: 0
        clip: true

        // title bar
        Rectangle {
            id: titlebar
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 34
            radius: 12
            color: "#262a36"
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 12; color: parent.color }
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: "#000000"; opacity: 0.35 }

            Row {
                id: lights
                anchors { left: parent.left; leftMargin: 13; verticalCenter: parent.verticalCenter }
                spacing: 8
                Repeater {
                    model: ["#ff5f57", "#febc2e", "#28c840"]
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        width: 12; height: 12; radius: 6
                        color: modelData
                        border.color: Qt.darker(modelData, 1.3); border.width: 0.5
                        Text {
                            anchors.centerIn: parent
                            visible: lightsHover.hovered
                            text: index === 0 ? "×" : index === 1 ? "–" : "+"
                            color: "#5a2a20"; font.pixelSize: 10; font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: if (index === 0) { win.opacity = 0; win.scale = 0.9; closeTimer.start() }
                        }
                    }
                }
                HoverHandler { id: lightsHover }
            }
            Text {
                anchors.centerIn: parent
                text: win.title
                color: "#c9cddc"; font.pixelSize: 13; font.bold: true
            }
            DragHandler {
                target: win
                onActiveChanged: if (active) win.activated()
            }
        }

        Item {
            id: body
            anchors { left: parent.left; right: parent.right; top: titlebar.bottom; bottom: parent.bottom }
        }
        Rectangle { anchors.fill: parent; radius: 12; color: "transparent"; border.color: "#ffffff"; opacity: 0.09 }
    }

    TapHandler { onTapped: win.activated(); gesturePolicy: TapHandler.DragThreshold }
    Timer { id: closeTimer; interval: 260; onTriggered: win.visible = false }
}
