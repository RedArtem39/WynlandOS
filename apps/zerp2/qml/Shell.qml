// Zerp 2.0: a tiling WM. Wallpaper, a bar on top, a dock at the bottom,
// and Zerp clients tiled dwindle-style per workspace in between.
// Keys (mod = Alt or Super): Enter terminal, Q close, D launcher,
// F fullscreen, 1..9 workspace, Shift+1..9 move window, arrows/HJKL focus.
import QtQuick
import QtQuick.Window
import Zerp

Window {
    id: shell
    visible: true
    visibility: Window.FullScreen
    color: "#0b0d12"
    title: "Zerp"

    // ---- look
    readonly property int gapOut: 14
    readonly property int gapIn: 10
    readonly property int border: 2
    readonly property int radius: 12
    readonly property int barHeight: 34
    readonly property int dockReserve: 84

    // usable area for tiles
    readonly property rect area: Qt.rect(gapOut, barHeight + gapOut,
                                         width - 2 * gapOut,
                                         height - barHeight - dockReserve - 2 * gapOut)

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
                GradientStop { position: 0.0; color: "#1b1f3a" }
                GradientStop { position: 1.0; color: "#0b1a2a" }
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

    // ---- tiles
    Item {
        id: tiles
        anchors.fill: parent
        Repeater {
            model: zerp.clients
            delegate: Tile {
                required property var modelData
                client: modelData
                target: shell.rects[modelData.id] !== undefined ? shell.rects[modelData.id] : Qt.rect(0, 0, 0, 0)
                wsOffset: (modelData.workspace - zerp.workspace) * shell.width
                borderWidth: shell.border
                cornerRadius: modelData.fullscreen ? 0 : shell.radius
                z: modelData.fullscreen ? 50 : (modelData.focused ? 2 : 1)
            }
        }
    }

    Bar {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: shell.barHeight
        blurSource: desktop
        z: 40
    }

    Dock {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 10 }
        blurSource: desktop
        z: 40
    }

    Component.onCompleted: relayout()
}
