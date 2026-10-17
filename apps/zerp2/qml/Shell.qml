// Zerp 2.0: a tiling WM and Wayland compositor. Wallpaper, a bar on top,
// a dock at the bottom, and windows -- Zerp clients and Wayland clients --
// tiled dwindle-style per workspace in between.
// Keys (mod = Alt or Super): Enter terminal, Q close, D launcher,
// F fullscreen, 1..9 workspace, Shift+1..9 move window, arrows/HJKL focus.
import QtQuick
import QtQuick.Window
import QtQuick.Effects
import QtWayland.Compositor
import QtWayland.Compositor.XdgShell
import Zerp

Window {
    id: shell
    visible: true
    visibility: Window.FullScreen
    color: "#07090c"
    title: "Zerp"

    // ---- look
    readonly property int gapOut: 14
    readonly property int gapIn: 10
    readonly property int border: 2
    readonly property int radius: 12
    readonly property int barHeight: 34
    readonly property int dockReserve: 84

    // a desktop shell of its own (Caelestia, any layer-shell panel with an
    // exclusive zone): it has the edges, our bar and dock step aside
    readonly property var reserved: layerShell.reserved
    readonly property bool externalShell: reserved.top > 0 || reserved.bottom > 0 || reserved.left > 0 || reserved.right > 0

    // usable area for tiles
    readonly property rect area: externalShell
        ? Qt.rect(reserved.left + gapOut, reserved.top + gapOut,
                  width - reserved.left - reserved.right - 2 * gapOut,
                  height - reserved.top - reserved.bottom - 2 * gapOut)
        : Qt.rect(gapOut, barHeight + gapOut, width - 2 * gapOut, height - barHeight - dockReserve - 2 * gapOut)
    onAreaChanged: relayout()

    // a full-screen window in front: the bar and the dock step aside (they
    // sit above the windows and covered its top -- a browser's toolbar)
    readonly property bool fullscreenOn: {
        const f = zerp.focusedClient
        return !!f && f.fullscreen && f.workspace === zerp.workspace
    }

    // ---- Wayland: the socket is $XDG_RUNTIME_DIR/wayland-0; every
    // xdg-shell toplevel becomes a window (ZServer::addWayland), drawn by
    // Tile.qml. Clients are asked to leave the decorations to us.
    WaylandCompositor {
        id: wayland
        socketName: "wayland-0"
        WaylandOutput {
            sizeFollowsWindow: true
            window: shell
            // the output's name for clients (xdg-output), the one the
            // Hyprland IPC reports: shells pair screens and monitors by it.
            // Its logical geometry is the screen's size for Qt's clients
            // (empty: a 0x0 screen -- Caelestia's launcher had no room)
            XdgOutputV1 {
                name: outputName
                description: "WynlandOS " + outputName
                logicalPosition: Qt.point(0, 0)
                logicalSize: Qt.size(shell.width, shell.height)
            }
        }
        XdgOutputManagerV1 { id: xdgOutputManager }
        // wlr-layer-shell: panels, wallpapers, overlays of desktop shells
        LayerShell {
            id: layerShell
            outputSize: Qt.size(shell.width, shell.height)
        }
        // Hyprland's global shortcuts: Caelestia's launcher and the rest,
        // fired by the key bindings (keybinds.conf, `global` dispatcher)
        GlobalShortcuts {}
        XdgShell {
            onToplevelCreated: (toplevel, xdgSurface) => zerp.addWayland(xdgSurface)
        }
        XdgDecorationManagerV1 {
            preferredMode: XdgToplevel.ServerSideDecoration
        }
        Component.onCompleted: console.log("wayland compositor up on " + socketName)
    }

    // id -> target rect, recomputed whenever clients/workspace change
    property var rects: ({})

    function relayout() {
        var out = {};
        var list = zerp.clients;
        for (var ws = 1; ws <= 9; ws++) {
            var tiled = [];
            for (var i = 0; i < list.length; i++) {
                var c = list[i];
                if (c.workspace !== ws) continue;
                if (c.fullscreen) out[c.id] = Qt.rect(0, 0, width, height);
                else tiled.push(c);
            }
            // dwindle: each new window splits the previous one's space,
            // alternating along the longer side
            var r = Qt.rect(area.x, area.y, area.width, area.height);
            for (var k = 0; k < tiled.length; k++) {
                if (k === tiled.length - 1) { out[tiled[k].id] = r; break; }
                var a, b;
                if (r.width >= r.height) {
                    var w1 = Math.floor((r.width - gapIn) / 2);
                    a = Qt.rect(r.x, r.y, w1, r.height);
                    b = Qt.rect(r.x + w1 + gapIn, r.y, r.width - w1 - gapIn, r.height);
                } else {
                    var h1 = Math.floor((r.height - gapIn) / 2);
                    a = Qt.rect(r.x, r.y, r.width, h1);
                    b = Qt.rect(r.x, r.y + h1 + gapIn, r.width, r.height - h1 - gapIn);
                }
                out[tiled[k].id] = a;
                r = b;
            }
        }
        rects = out;
    }

    Connections {
        target: zerp
        function onClientsChanged() { shell.relayout() }
        function onWorkspaceChanged() { shell.relayout() }
        function onLayoutChanged() { shell.relayout() }
        function onFocusDirectionRequested(dx, dy) { shell.focusTowards(dx, dy) }
    }
    onWidthChanged: relayout()
    onHeightChanged: relayout()

    // nearest tile in a direction from the focused one
    function focusTowards(dx, dy) {
        var f = zerp.focusedClient;
        if (!f || !rects[f.id]) return;
        var fr = rects[f.id];
        var fx = fr.x + fr.width / 2, fy = fr.y + fr.height / 2;
        var best = null, bestD = 1e9;
        var list = zerp.clients;
        for (var i = 0; i < list.length; i++) {
            var c = list[i];
            if (c === f || c.workspace !== zerp.workspace || !rects[c.id]) continue;
            var r = rects[c.id];
            var vx = r.x + r.width / 2 - fx, vy = r.y + r.height / 2 - fy;
            if (dx !== 0 && Math.sign(vx) !== dx) continue;
            if (dy !== 0 && Math.sign(vy) !== dy) continue;
            var d = Math.abs(vx) + Math.abs(vy) + (dx !== 0 ? 2 * Math.abs(vy) : 2 * Math.abs(vx));
            if (d < bestD) { bestD = d; best = c; }
        }
        if (best) zerp.focus(best);
    }

    // ---- wallpaper (blur source for the bar and the dock)
    Item {
        id: desktop
        anchors.fill: parent
        Rectangle {
            anchors.fill: parent
            visible: wallpaperUrl === ""
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#0d1015" }
                GradientStop { position: 1.0; color: "#07090c" }
            }
        }
        Image {
            anchors.fill: parent
            source: wallpaperUrl

            fillMode: Image.PreserveAspectCrop
            visible: wallpaperUrl !== ""
            asynchronous: true
            sourceSize.width: shell.width
        }
    }

    // ---- frosted glass: the wallpaper through ONE blur for the whole
    // screen, live (a wallpaper that moves -- a video -- shows through as it
    // changes), re-rendered only when the wallpaper does. The bar, the dock
    // and see-through windows (ZERP_MSG_ALPHA) each show their slice of it
    // (Glass.qml); blurring in each of them redid the blur per item.
    MultiEffect {
        id: blurredWall
        anchors.fill: parent
        source: desktop
        blurEnabled: true
        blur: 1.0
        blurMax: 64
        saturation: 0.25
        z: -1   // under the wallpaper: rendered for the slices, never seen itself
    }

    // FPS meter (mod+P)
    property int fps: 0
    property int frames: 0
    onFrameSwapped: frames++
    Timer {
        interval: 1000; repeat: true; running: zerp.showFps
        onTriggered: { shell.fps = shell.frames; shell.frames = 0 }
    }

    // ---- tiles
    Item {
        id: tiles
        anchors.fill: parent
        z: 1   // over the background and bottom layers
        Repeater {
            model: zerp.model
            delegate: Tile {
                target: shell.rects[client.id] !== undefined ? shell.rects[client.id] : Qt.rect(0, 0, 0, 0)
                wsOffset: (client.workspace - zerp.workspace) * shell.width
                borderWidth: shell.border
                cornerRadius: client.fullscreen ? 0 : shell.radius
                z: client.fullscreen ? 50 : (client.focused ? 2 : 1)
            }
        }
    }

    // ---- layer surfaces: background and bottom under the windows, top
    // over them, overlay over everything; placed by their anchors and margins
    Repeater {
        model: layerShell.surfaces
        delegate: WaylandQuickItem {
            required property var modelData
            readonly property var ls: modelData
            readonly property int a: ls.anchors
            surface: ls.surface
            visible: ls.mapped && (!shell.fullscreenOn || ls.layer === 3)
            width: ls.width
            height: ls.height
            x: (a & 4) && !(a & 8) ? ls.margins.left
             : (a & 8) && !(a & 4) ? shell.width - width - ls.margins.right
             : (a & 4) && (a & 8) ? ls.margins.left
             : (shell.width - width) / 2
            y: (a & 1) && !(a & 2) ? ls.margins.top
             : (a & 2) && !(a & 1) ? shell.height - height - ls.margins.bottom
             : (a & 1) && (a & 2) ? ls.margins.top
             : (shell.height - height) / 2
            z: [0.5, 0.6, 45, 100][ls.layer]
            focusOnClick: ls.keyboard > 0
            // exclusive keyboard (a launcher, a lock): it takes the keys while there
            onVisibleChanged: if (visible && ls.keyboard === 1) takeFocus()
        }
    }

    Bar {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: shell.barHeight
        fps: zerp.showFps ? shell.fps : -1
        visible: !shell.fullscreenOn && !shell.externalShell
        z: 40
    }

    Dock {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 10 }
        screenW: shell.width
        screenH: shell.height
        visible: !shell.fullscreenOn && !shell.externalShell
        z: 40
    }

    Component.onCompleted: relayout()
}
