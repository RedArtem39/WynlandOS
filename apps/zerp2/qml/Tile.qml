// One Zerp client as a tile: its pixels with rounded corners, a gradient
// border when focused, animated into place (and sideways on workspace
// switches).
import QtQuick
import QtQuick.Effects
import Zerp

Item {
    id: tile
    property var client
    property rect target
    property real wsOffset: 0
    property int borderWidth: 2
    property int cornerRadius: 12

    x: target.x + wsOffset
    y: target.y
    width: target.width
    height: target.height
    visible: target.width > 0 && Math.abs(wsOffset) < 4000

    Behavior on x { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
    Behavior on y { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
    Behavior on width { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }

    // open: pop in
    scale: 0.94
    opacity: 0
    Component.onCompleted: { scale = 1; opacity = 1; configureClient() }
    Behavior on scale { NumberAnimation { duration: 240; easing.type: Easing.OutBack } }
    Behavior on opacity { NumberAnimation { duration: 200 } }

    // the client renders at the layout's TARGET size, not the animated one
    function configureClient() {
        if (!client || target.width <= 0) return;
        client.configure(target.x + borderWidth, target.y + borderWidth,
                         target.width - 2 * borderWidth, target.height - 2 * borderWidth);
    }
    onTargetChanged: configureClient()
    onClientChanged: configureClient()   // target may have been set first

    // border: a gradient plate behind the content (active) or a dim one
    Rectangle {
        anchors.fill: parent
        radius: tile.cornerRadius + tile.borderWidth
        visible: tile.cornerRadius > 0
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: tile.client && tile.client.focused ? "#33ccff" : "#3a3f4b" }
            GradientStop { position: 1.0; color: tile.client && tile.client.focused ? "#a066ff" : "#3a3f4b" }
        }
    }

    Item {
        id: content
        anchors { fill: parent; margins: tile.cornerRadius > 0 ? tile.borderWidth : 0 }
        layer.enabled: tile.cornerRadius > 0
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        ZSurface {
            anchors.fill: parent
            client: tile.client
            onPressed: zerp.focus(tile.client)
        }
    }
    Item {
        id: mask
        anchors.fill: content
        layer.enabled: true
        visible: false
        Rectangle { anchors.fill: parent; radius: tile.cornerRadius; color: "black"; antialiasing: true }
    }
}
