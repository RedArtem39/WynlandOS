// One window as a tile: a Zerp client's pixels (ZSurface) or a Wayland
// client's surface (ShellSurfaceItem), as Hyprland draws its windows:
// - motion on Hyprland's own default curve, cubic-bezier(.05, .9, .1, 1.05)
//   -- quick, a touch past the mark and back: moves, resizes, workspaces
//   sliding in;
// - opening pops in from 80 %, closing goes back to 80 % and fades with
//   the window's last frame (Zerp keeps a closing client a moment);
// - rounded for real (a mask on the GPU), a soft shadow under it, and the
//   focused one's border a gradient that turns slowly around it.
// Without a GPU (simpledrm: everything drawn by the CPU) only the motion
// stays: the corners are capped in the border colour, no shadow, a still
// border.
import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import QtWayland.Compositor
import Zerp

Item {
    id: tile
    required property var client   // model role
    property rect target
    property real wsOffset: 0
    property int borderWidth: 2
    property int cornerRadius: 12
    // on the infinite canvas: target is in the plane's coordinates, drawn
    // through the camera -- moved by pan, scaled by zoom (the client keeps
    // its own size); animate off while the camera or the window is dragged
    property point pan: Qt.point(0, 0)
    property real zoom: 1
    property bool animate: true
    // held by the hand on the canvas: a little bigger about its middle
    property real lift: 1
    Behavior on lift { NumberAnimation { duration: 360; easing.type: Easing.OutBack; easing.overshoot: 2.2 } }
    property real lean: 0               // degrees: it leans with its speed when thrown or held

    readonly property bool fx: typeof gpuEffects === "undefined" || gpuEffects
    readonly property bool active: client && client.focused
    readonly property bool closing: client && client.closing
    readonly property bool flat: client && client.fullscreen

    // Hyprland's default bezier, and an ease-in for going away
    readonly property var curve: [0.05, 0.9, 0.1, 1.05, 1, 1]
    readonly property var curveOut: [0.3, 0, 0.8, 0.15, 1, 1]

    // the colours: the focused border two hues turning, the rest quiet
    readonly property color accentA: "#6fb3ff"
    readonly property color accentB: "#b38bff"
    readonly property color idle: "#2a2f3a"

    // where it is: the layout's target, held while the window closes (the
    // layout has forgotten it already)
    property rect held: target
    onTargetChanged: {
        if (!closing && target.width > 0) held = target;
        configureClient();
    }

    x: (held.x - pan.x) * zoom + wsOffset
    y: (held.y - pan.y) * zoom
    width: held.width
    height: held.height
    visible: held.width > 0 && Math.abs(wsOffset) < 4000

    Behavior on x { enabled: tile.animate && !tile.closing; NumberAnimation { duration: 420; easing.type: Easing.BezierSpline; easing.bezierCurve: tile.curve } }
    Behavior on y { enabled: tile.animate && !tile.closing; NumberAnimation { duration: 420; easing.type: Easing.BezierSpline; easing.bezierCurve: tile.curve } }
    Behavior on width { enabled: tile.animate && !tile.closing; NumberAnimation { duration: 420; easing.type: Easing.BezierSpline; easing.bezierCurve: tile.curve } }
    Behavior on height { enabled: tile.animate && !tile.closing; NumberAnimation { duration: 420; easing.type: Easing.BezierSpline; easing.bezierCurve: tile.curve } }

    // open and close: 0 gone, 1 there -- scaled from 80 % about the middle
    property real shown: 0
    Component.onCompleted: { shown = 1; configureClient(); }
    onClosingChanged: if (closing) shown = 0
    Behavior on shown {
        NumberAnimation {
            duration: tile.closing ? 240 : 380
            easing.type: Easing.BezierSpline
            easing.bezierCurve: tile.closing ? tile.curveOut : tile.curve
        }
    }
    opacity: Math.min(1, tile.shown * 1.25)

    transform: [
        Scale { origin.x: tile.width / 2; origin.y: tile.height / 2; xScale: .8 + .2 * tile.shown; yScale: .8 + .2 * tile.shown },
        Scale { origin.x: tile.width / 2; origin.y: tile.height / 2; xScale: tile.lift; yScale: tile.lift },
        Rotation { origin.x: tile.width / 2; origin.y: tile.height / 2; angle: tile.lean },
        Scale { xScale: tile.zoom; yScale: tile.zoom }
    ]

    // the client renders at the layout's TARGET size, not the animated one
    function configureClient() {
        if (!client || closing || target.width <= 0) return;
        client.configure(target.x + borderWidth, target.y + borderWidth,
                         target.width - 2 * borderWidth, target.height - 2 * borderWidth);
    }
    onClientChanged: configureClient()   // target may have been set first

    readonly property real outerRadius: cornerRadius > 0 ? cornerRadius + borderWidth : 0

    // ---- the shadow (an SDF shader: cheap, no pass of its own)
    RectangularShadow {
        anchors.fill: parent
        visible: tile.fx && !tile.flat
        radius: tile.outerRadius
        offset.y: tile.active ? 10 : 6
        blur: tile.active ? 36 : 22
        spread: 0
        color: Qt.rgba(0, 0, 0, tile.active ? .55 : .38)
        Behavior on blur { NumberAnimation { duration: 300 } }
    }

    // ---- the border: a conic gradient that turns once round the window
    // when it gets the focus, then rests (turning for ever, as Hyprland's
    // borderangle loop, redrew the whole screen every frame -- every one a
    // GPU submission under the kernel's big lock here)
    property real spin: 0
    NumberAnimation {
        id: spinOnce
        target: tile; property: "spin"
        from: 0; to: 360; duration: 1400
        easing.type: Easing.BezierSpline; easing.bezierCurve: tile.curve
    }
    onActiveChanged: if (active && fx && !flat) spinOnce.restart()
    Shape {
        anchors.fill: parent
        visible: !tile.flat
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            fillGradient: ConicalGradient {
                centerX: tile.width / 2; centerY: tile.height / 2
                angle: tile.spin
                GradientStop { position: 0.0; color: tile.active ? tile.accentA : tile.idle; Behavior on color { ColorAnimation { duration: 250 } } }
                GradientStop { position: 0.5; color: tile.active ? tile.accentB : tile.idle; Behavior on color { ColorAnimation { duration: 250 } } }
                GradientStop { position: 1.0; color: tile.active ? tile.accentA : tile.idle; Behavior on color { ColorAnimation { duration: 250 } } }
            }
            PathRectangle { x: 0; y: 0; width: tile.width; height: tile.height; radius: tile.outerRadius }
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

    // ---- the window, its corners cut by a mask on the GPU
    Loader {
        id: surface
        anchors { fill: parent; margins: tile.flat ? 0 : tile.borderWidth }
        sourceComponent: tile.client && tile.client.wayland ? waylandSurface : zerpSurface
        layer.enabled: tile.fx && !tile.flat && tile.cornerRadius > 0
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: cornerMask
            maskThresholdMin: .5
            maskSpreadAtMin: 1
        }
    }
    Rectangle {
        id: cornerMask
        anchors.fill: surface
        radius: tile.cornerRadius
        visible: false
        layer.enabled: true
    }

    Component {
        id: zerpSurface
        ZSurface {
            client: tile.client
            onPressed: zerp.focus(tile.client)
        }
    }

    Component {
        id: waylandSurface
        Item {
            clip: true
            Rectangle { anchors.fill: parent; color: "#0b0d11" }   // until the first frame
            ShellSurfaceItem {
                shellSurface: tile.client.shellSurface
                autoCreatePopupItems: true    // menus, tooltips
                moveItem: pinned              // an interactive move request moves nothing
                // closing: its last frame stays while the tile goes
                onSurfaceDestroyed: bufferLocked = true
                TapHandler {
                    acceptedButtons: Qt.AllButtons
                    onPressedChanged: if (pressed) zerp.focus(tile.client)
                }
            }
            Item { id: pinned }
        }
    }

    // without a GPU: corners capped in the border colour (no mask pass)
    Item {
        anchors.fill: surface
        visible: !tile.fx && !tile.flat && tile.cornerRadius > 0
        readonly property color edge: tile.active ? tile.accentA : tile.idle
        Corner { width: tile.cornerRadius; height: width; corner: 0; fill: parent.edge; x: 0; y: 0 }
        Corner { width: tile.cornerRadius; height: width; corner: 1; fill: parent.edge; x: parent.width - width; y: 0 }
        Corner { width: tile.cornerRadius; height: width; corner: 2; fill: parent.edge; x: parent.width - width; y: parent.height - height }
        Corner { width: tile.cornerRadius; height: width; corner: 3; fill: parent.edge; x: 0; y: parent.height - height }
    }
}
