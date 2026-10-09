// One Zerp client as a tile: its pixels, rounded by corner caps in the
// border colour, a gradient border when focused, animated into place (and
// sideways on workspace switches). No layers: animating stays cheap.
import QtQuick
import Zerp

Item {
    id: tile
    required property var client   // model role
    property rect target
    property real wsOffset: 0
    property int borderWidth: 2
    property int cornerRadius: 12

    readonly property bool active: client && client.focused
    readonly property color edgeLeft: active ? "#6fb3ff" : "#22262e"
    readonly property color edgeRight: active ? "#3d7eff" : "#22262e"

    x: target.x + wsOffset
    y: target.y
    width: target.width
    height: target.height
    visible: target.width > 0 && Math.abs(wsOffset) < 4000

    Behavior on x { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
    Behavior on y { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
    Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }

    // open: pop in
    scale: 0.95
    opacity: 0
    Component.onCompleted: { scale = 1; opacity = 1; configureClient() }
    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    Behavior on opacity { NumberAnimation { duration: 180 } }

    // the client renders at the layout's TARGET size, not the animated one
    function configureClient() {
        if (!client || target.width <= 0) return;
        client.configure(target.x + borderWidth, target.y + borderWidth,
                         target.width - 2 * borderWidth, target.height - 2 * borderWidth);
    }
    onTargetChanged: configureClient()
    onClientChanged: configureClient()   // target may have been set first

    // border plate (the gradient shows around the content)
    Rectangle {
        anchors.fill: parent
        radius: tile.cornerRadius > 0 ? tile.cornerRadius + tile.borderWidth : 0
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: tile.edgeLeft; Behavior on color { ColorAnimation { duration: 180 } } }
            GradientStop { position: 1.0; color: tile.edgeRight; Behavior on color { ColorAnimation { duration: 180 } } }
        }
    }

    // a see-through client (ZERP_MSG_ALPHA): live frosted glass under it,
    // over the border plate
    Glass {
        anchors.fill: surface
        visible: tile.client && tile.client.alpha
        gx: tile.x + surface.x
        gy: tile.y + surface.y
    }

    ZSurface {
        id: surface
        anchors { fill: parent; margins: tile.cornerRadius > 0 ? tile.borderWidth : 0 }
        client: tile.client
        onPressed: zerp.focus(tile.client)

        // rounded corners: caps in the border colour over the content
        Corner { visible: tile.cornerRadius > 0; width: tile.cornerRadius; height: width; corner: 0; fill: tile.edgeLeft; x: 0; y: 0 }
        Corner { visible: tile.cornerRadius > 0; width: tile.cornerRadius; height: width; corner: 1; fill: tile.edgeRight; x: parent.width - width; y: 0 }
        Corner { visible: tile.cornerRadius > 0; width: tile.cornerRadius; height: width; corner: 2; fill: tile.edgeRight; x: parent.width - width; y: parent.height - height }
        Corner { visible: tile.cornerRadius > 0; width: tile.cornerRadius; height: width; corner: 3; fill: tile.edgeLeft; x: 0; y: parent.height - height }
    }
}
