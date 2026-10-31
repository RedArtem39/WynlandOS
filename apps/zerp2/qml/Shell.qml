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
        // ext-session-lock: a lock screen the compositor enforces
        SessionLock {
            id: sessionLock
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
        for (var ws = 0; ws <= 9; ws++) {
            var tiled = [];
            for (var i = 0; i < list.length; i++) {
                var c = list[i];
                if (c.workspace !== ws) continue;
                if (c.fullscreen) out[c.id] = Qt.rect(0, 0, width, height);
                else tiled.push(c);
            }
            if (isCanvas(ws)) {
                // the infinite canvas: each window where it was put on the
                // plane; a new one in the middle of the view
                for (var j = 0; j < tiled.length; j++) {
                    var cl = tiled[j];
                    if (!world[cl.id]) world[cl.id] = restored(ws, cl) || placeBeside(ws, cl);
                    out[cl.id] = world[cl.id];
                }
                continue;
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
        if (Object.keys(canvasOn).length || canvasOpts.def) saveLater.restart();
    }

    Connections {
        target: zerp
        function onClientsChanged() { shell.relayout() }
        function onWorkspaceChanged() { shell.relayout() }
        function onLayoutChanged() { shell.relayout() }
        function onFocusDirectionRequested(dx, dy) { shell.focusTowards(dx, dy) }
        function onCanvasToggleRequested() { shell.toggleCanvas() }
        function onCanvasZoomRequested(how) {
            if (!shell.canvasHere) return;
            if (how === "fit") shell.fitAll();
            else shell.zoomAt(how === "-" ? 1 / 1.25 : 1.25, shell.area.x + shell.area.width / 2, shell.area.y + shell.area.height / 2);
        }
        function onFocusChanged() {
            shell.noteFocus();
            if (shell.canvasHere && zerp.focusedClient && !zerp.focusedClient.fullscreen && !shell.switching) shell.reveal(zerp.focusedClient);
        }
        function onCanvasActionRequested(what) { shell.canvasAction(what) }
        function onCycleRequested(dir) { shell.cycle(dir) }
        function onAltUp() { shell.commitSwitch() }
    }
    Connections { target: zerp; function onWorkspaceChanged() { shell.loadCam() } }
    onWidthChanged: relayout()
    onHeightChanged: relayout()

    // ---- the infinite canvas (Alt+G): per workspace, its windows on a plane
    // without edges, seen through a camera (pan, zoom) the mouse moves
    property var canvasOn: ({})          // workspace -> true
    property var world: ({})             // client id -> rect on the plane
    property var cams: ({})              // workspace -> { x, y, z }
    // the settings: new workspaces as canvases, where new windows go, the
    // motion's feel, inertia, snapping, the ground
    property var canvasOpts: ({ def: false, place: "beside", feel: "soft", inertia: true, snap: true, bg: "dots" })
    // a workspace never switched keeps the default
    function isCanvas(ws) { return canvasOn[ws] !== undefined ? !!canvasOn[ws] : !!canvasOpts.def; }
    readonly property bool canvasHere: isCanvas(zerp.workspace)
    // the camera of the workspace on screen (the others keep theirs in cams)
    property real camX: 0
    property real camY: 0
    property real camZ: 1
    property bool morphing: false        // tiles glide between tiling and the canvas
    Timer { id: morphEnd; interval: 320; onTriggered: shell.morphing = false }

    function camOf(ws) { return ws === zerp.workspace ? { x: camX, y: camY, z: camZ } : (cams[ws] || { x: 0, y: 0, z: 1 }); }
    function saveCam() { var c = Object.assign({}, cams); c[zerp.workspace] = { x: camX, y: camY, z: camZ }; cams = c; }
    function loadCam() { var c = cams[zerp.workspace] || { x: 0, y: 0, z: 1 }; camAnim.stop(); camX = c.x; camY = c.y; camZ = c.z; }
    onCamXChanged: if (canvasHere) saveCam()
    onCamYChanged: if (canvasHere) saveCam()
    onCamZChanged: if (canvasHere) saveCam()

    // the view's middle on the plane
    function viewCenter(ws) {
        var c = camOf(ws);
        return Qt.point(c.x + (area.x + area.width / 2) / c.z, c.y + (area.y + area.height / 2) / c.z);
    }
    // a window's place on the plane: where tiling had it (the camera
    // undone), or -- a new one -- 60% of the view in its middle, stepped
    // aside while it would sit right on another
    function placeNew(ws, tiledRect) {
        var c = camOf(ws);
        if (tiledRect && tiledRect.width > 0)
            return Qt.rect(c.x + tiledRect.x / c.z, c.y + tiledRect.y / c.z, tiledRect.width, tiledRect.height);
        var w = Math.round(area.width * .6), h = Math.round(area.height * .7), m = viewCenter(ws);
        var r = Qt.rect(Math.round(m.x - w / 2), Math.round(m.y - h / 2), w, h);
        for (var tries = 0; tries < 20; tries++) {
            var clash = false;
            for (var id in world) { var o = world[id]; if (Math.abs(o.x - r.x) < 24 && Math.abs(o.y - r.y) < 24) { clash = true; break; } }
            if (!clash) break;
            r = Qt.rect(r.x + 40, r.y + 40, w, h);
        }
        return r;
    }

    // a new window on the canvas: right of the one in focus (or below it,
    // or left of it) where that is free, as big as it; else the view's middle
    function placeBeside(ws, c) {
        var f = zerp.focusedClient, cm = camOf(ws);
        var fr = f && f !== c && f.workspace === ws ? world[f.id] : null;
        var W = Math.round(area.width * .5 / cm.z), H = Math.round(area.height * .62 / cm.z), where = canvasOpts.place;
        if (where === "cursor") return Qt.rect(cm.x + zerp.cursor.x / cm.z - W / 2, cm.y + zerp.cursor.y / cm.z - 22, W, H);
        if (fr && where !== "center") {
            var tries = [Qt.rect(fr.x + fr.width + gapIn, fr.y, W, H),
                         Qt.rect(fr.x, fr.y + fr.height + gapIn, W, H),
                         Qt.rect(fr.x - W - gapIn, fr.y, W, H),
                         Qt.rect(fr.x, fr.y - H - gapIn, W, H)];
            for (var k = 0; k < tries.length; k++) if (isFree(tries[k], ws, c.id)) return tries[k];
        }
        return placeNew(ws, null);
    }
    function overlaps(a, b) { return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height; }
    function isFree(r, ws, except) {
        var list = zerp.clients;
        for (var i = 0; i < list.length; i++) {
            var o = world[list[i].id];
            if (list[i].id !== except && list[i].workspace === ws && o && overlaps(r, o)) return false;
        }
        return true;
    }

    // ---- the canvas kept between sessions (~/.config/zerp2/canvas.json):
    // which workspaces are canvases, their cameras, the places of the
    // windows by app -- a window of that app opening there goes back to it
    readonly property string canvasFile: "~/.config/zerp2/canvas.json"
    property var pendingPlaces: []       // [{ ws, app, title, r }] not yet claimed
    function restored(ws, c) {
        for (var i = 0; i < pendingPlaces.length; i++) {
            var p = pendingPlaces[i];
            if (p.ws === ws && p.app === c.appId) {
                var l = pendingPlaces.slice(); l.splice(i, 1); pendingPlaces = l;
                return Qt.rect(p.r[0], p.r[1], p.r[2], p.r[3]);
            }
        }
        return null;
    }
    function saveCanvas() {
        var wins = [], list = zerp.clients;
        for (var i = 0; i < list.length; i++) {
            var r = world[list[i].id];
            if (r && isCanvas(list[i].workspace)) wins.push({ ws: list[i].workspace, app: list[i].appId, title: list[i].title, r: [r.x, r.y, r.width, r.height] });
        }
        zerp.writeFile(canvasFile, JSON.stringify({ on: canvasOn, opts: canvasOpts, cams: cams, marks: marks, wins: wins.concat(pendingPlaces) }));
    }
    Timer { id: saveLater; interval: 1200; onTriggered: shell.saveCanvas() }
    onWorldChanged: saveLater.restart()
    onCamsChanged: saveLater.restart()
    onCanvasOnChanged: saveLater.restart()
    Component.onCompleted: {
        try {
            var d = JSON.parse(zerp.readFile(canvasFile) || "{}");
            var on = {};
            if (Array.isArray(d.on)) for (var i = 0; i < d.on.length; i++) on[d.on[i]] = true;   // (the first form: a list)
            else on = d.on || {};
            canvasOn = on;
            if (d.opts) canvasOpts = Object.assign({}, canvasOpts, d.opts);
            cams = d.cams || {};
            marks = d.marks || {};
            pendingPlaces = d.wins || [];
            loadCam();
        } catch (e) { console.log("canvas.json: " + e); }
        relayout();
    }

    function toggleCanvas() {
        var ws = zerp.workspace, on = Object.assign({}, canvasOn);
        morphing = true; morphEnd.restart();
        if (isCanvas(ws)) {
            on[ws] = false;
        } else {
            on[ws] = true;
            // the first time: the plane starts where the tiles are
            if (!cams[ws]) { var cs = Object.assign({}, cams); cs[ws] = { x: 0, y: 0, z: 1 }; cams = cs; }
            loadCam();
            var list = zerp.clients;
            for (var i = 0; i < list.length; i++) {
                var c = list[i];
                if (c.workspace === ws && !world[c.id] && rects[c.id] && !c.fullscreen) world[c.id] = placeNew(ws, rects[c.id]);
            }
        }
        canvasOn = on;
        canvasNote.show(on[ws] ? "Бесконечный холст" : "Тайлинг",
                        on[ws] ? "Alt+G — обратно · колёсико и перетаскивание · Ctrl+колёсико — масштаб · Alt+мышь — окна" : "Alt+G — холст");
        relayout();
    }
    // zoom about a point of the screen (the pointer, or the view's middle)
    function zoomAt(f, sx, sy) {
        camAnim.stop();
        var z = Math.max(.15, Math.min(2.5, camZ * f));
        var px = camX + sx / camZ, py = camY + sy / camZ;
        camX = px - sx / z; camY = py - sy / z; camZ = z;
        canvasNote.show(Math.round(z * 100) + "%", "");
    }
    // the camera to a place, gliding
    function lookAt(x, y, z) {
        camAnim.stop();
        animX.to = x; animY.to = y; animZ.to = z;
        camAnim.start();
    }
    ParallelAnimation {
        id: camAnim
        NumberAnimation { id: animX; target: shell; property: "camX"; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { id: animY; target: shell; property: "camY"; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { id: animZ; target: shell; property: "camZ"; duration: 380; easing.type: Easing.OutCubic }
    }
    // everything on this workspace in view
    function fitAll() {
        var x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, n = 0, list = zerp.clients;
        for (var i = 0; i < list.length; i++) {
            var r = world[list[i].id];
            if (list[i].workspace !== zerp.workspace || !r || list[i].fullscreen) continue;
            x0 = Math.min(x0, r.x); y0 = Math.min(y0, r.y); x1 = Math.max(x1, r.x + r.width); y1 = Math.max(y1, r.y + r.height); n++;
        }
        if (!n) { lookAt(0, 0, 1); return; }
        var z = Math.max(.15, Math.min(1, area.width / (x1 - x0 + 80), area.height / (y1 - y0 + 80)));
        lookAt((x0 + x1) / 2 - (area.x + area.width / 2) / z, (y0 + y1) / 2 - (area.y + area.height / 2) / z, z);
        canvasNote.show(Math.round(z * 100) + "%", "все окна");
    }
    // a focused window out of view: the camera brings it in
    function reveal(c) {
        var r = world[c.id];
        if (!r) return;
        var sx = (r.x - camX) * camZ, sy = (r.y - camY) * camZ, sw = r.width * camZ, sh = r.height * camZ;
        if (sx >= area.x && sy >= area.y && sx + sw <= area.x + area.width && sy + sh <= area.y + area.height) return;
        var z = Math.min(camZ, area.width / (r.width + 80), area.height / (r.height + 80));
        lookAt(r.x + r.width / 2 - (area.x + area.width / 2) / z, r.y + r.height / 2 - (area.y + area.height / 2) / z, z);
    }
    function canvasWheel(w) {
        camAnim.stop();
        var d = w.angleDelta.y || w.angleDelta.x;
        if (w.modifiers & Qt.ControlModifier) { zoomAt(d > 0 ? 1.12 : 1 / 1.12, w.x, w.y); return; }
        var step = d / 120 * 90 / camZ;
        if ((w.modifiers & Qt.ShiftModifier) || w.angleDelta.x) camX -= step; else camY -= step;
    }

    // ---- the canvas's actions (keybinds.conf: "canvas, ...")
    property var marks: ({})             // "ws:n" -> { x, y, z }
    property var homeBack: null          // where Alt+A came from
    property var fitBack: ({})           // client id -> { r, cam } before Alt+M
    // window moves that should glide (fit, restore): the tiles' easing on for a moment
    function glide() { morphing = true; morphEnd.restart(); }
    function canvasAction(what) {
        var w = what.split(" ");
        if (w[0] === "opt" && w.length >= 3) {
            // a setting: "opt KEY VALUE" (true/false, a word)
            var o = Object.assign({}, canvasOpts), val = w.slice(2).join(" ");
            o[w[1]] = val === "true" ? true : val === "false" ? false : val;
            canvasOpts = o; saveLater.restart(); relayout();
            return;
        }
        if (!canvasHere) return;
        var f = zerp.focusedClient;
        if (w[0] === "fit" && f && world[f.id]) {
            // the window fills the view at 100%, the ones under it pushed
            // out to their side (again: all back as they were) -- gliding
            var fb = Object.assign({}, fitBack);
            if (fb[f.id]) {
                var st = fb[f.id];
                aim(f.id, st.r, 0, 0);
                for (var pi = 0; pi < st.pushed.length; pi++) if (world[st.pushed[pi].id]) aim(st.pushed[pi].id, st.pushed[pi].r, 0, 0);
                lookAt(st.cam.x, st.cam.y, st.cam.z); delete fb[f.id];
            } else {
                var r = world[f.id], cx = r.x + r.width / 2, cy = r.y + r.height / 2;
                var n = Qt.rect(cx - area.width / 2, cy - area.height / 2, area.width, area.height), pushed = [], list = zerp.clients;
                for (var li = 0; li < list.length; li++) {
                    var o = list[li], q = world[o.id];
                    if (o === f || o.workspace !== f.workspace || !q || !overlaps(n, q)) continue;
                    pushed.push({ id: o.id, r: q });
                    aim(o.id, Qt.rect(q.x + q.width / 2 < cx ? n.x - gapIn - q.width : n.x + n.width + gapIn, q.y, q.width, q.height), 0, 0);
                }
                fb[f.id] = { r: r, cam: { x: camX, y: camY, z: camZ }, pushed: pushed };
                aim(f.id, n, 0, 0);
                lookAt(n.x - area.x, n.y - area.y, 1);
            }
            fitBack = fb;
        } else if (w[0] === "center" && f && world[f.id]) {
            var rc = world[f.id];
            lookAt(rc.x + rc.width / 2 - (area.x + area.width / 2) / camZ, rc.y + rc.height / 2 - (area.y + area.height / 2) / camZ, camZ);
        } else if (w[0] === "home") {
            // the origin, and back
            if (homeBack) { lookAt(homeBack.x, homeBack.y, homeBack.z); homeBack = null; }
            else { homeBack = { x: camX, y: camY, z: camZ }; lookAt(0, 0, 1); }
            canvasNote.show(homeBack ? "Начало холста" : "Обратно", "Alt+A");
        } else if (w[0] === "mark" && w.length === 3) {
            var key = zerp.workspace + ":" + w[2], m = Object.assign({}, marks);
            if (w[1] === "set") { m[key] = { x: camX, y: camY, z: camZ }; marks = m; saveLater.restart(); canvasNote.show("Место " + w[2] + " запомнено", "Ctrl+Alt+" + w[2] + " — сюда"); }
            else if (m[key]) { lookAt(m[key].x, m[key].y, m[key].z); canvasNote.show("Место " + w[2], ""); }
            else canvasNote.show("Место " + w[2] + " пусто", "Ctrl+Alt+Shift+" + w[2] + " — запомнить");
        }
    }

    // ---- Alt-Tab: the windows by recent use; Alt held keeps the choice open
    property var mru: []                 // client ids, the latest first
    property var switchList: []
    property int switchSel: -1
    readonly property bool switching: switchSel >= 0
    function noteFocus() {
        var f = zerp.focusedClient;
        if (!f) return;
        var l = mru.filter(function (id) { return id !== f.id; });
        l.unshift(f.id); mru = l;
    }
    // a client's tile item (the Repeater's delegate)
    function tileOf(c) { for (var i = 0; i < tiles.children.length; i++) if (tiles.children[i].client === c) return tiles.children[i]; return null; }
    function clientById(id) { var l = zerp.clients; for (var i = 0; i < l.length; i++) if (l[i].id === id) return l[i]; return null; }
    function cycle(dir) {
        if (!switching) {
            var l = mru.map(clientById).filter(function (c) { return c; });
            var all = zerp.clients;
            for (var i = 0; i < all.length; i++) if (l.indexOf(all[i]) < 0) l.push(all[i]);
            if (l.length < 2) return;
            switchList = l;
            switchSel = dir > 0 ? 1 : l.length - 1;
        } else switchSel = (switchSel + dir + switchList.length) % switchList.length;
    }
    function commitSwitch() {
        if (!switching) return;
        var c = switchList[switchSel];
        switchSel = -1;
        if (!c) return;
        if (c.workspace !== zerp.workspace) zerp.workspace = c.workspace;
        zerp.focus(c);
    }

    // ---- momentum: a flick of the empty space keeps the camera going
    property real flingX: 0
    property real flingY: 0
    Timer {
        id: fling
        interval: 16; repeat: true
        onTriggered: {
            shell.camX -= shell.flingX * 16 / shell.camZ;
            shell.camY -= shell.flingY * 16 / shell.camZ;
            shell.flingX *= .92; shell.flingY *= .92;
            if (Math.abs(shell.flingX) + Math.abs(shell.flingY) < .02) stop();
        }
    }

    // ---- windows on the move: each eases to its target (a quarter of the
    // way a frame); a thrown one glides on, slowing, then settles -- onto a
    // neighbour's edge if near one. The held one is lifted (Tile.lift).
    property var tgt: ({})               // client id -> where it is going
    property var vel: ({})               // client id -> [vx, vy] world px per ms
    property int liftId: -1
    function aim(id, r, vx, vy) {
        var t = Object.assign({}, tgt); t[id] = r; tgt = t;
        var v = Object.assign({}, vel); v[id] = [vx || 0, vy || 0]; vel = v;
        physics.start();
    }
    property var springV: ({})           // client id -> the spring's speed [x, y, w, h]
    property var tilt: ({})              // client id -> its lean, degrees
    readonly property var feels: ({ soft: [.075, .80], normal: [.12, .74], crisp: [.22, .62] })
    Timer {
        id: physics
        interval: 16; repeat: true
        property real last: 0
        onRunningChanged: last = Date.now()
        onTriggered: {
            var now = Date.now(), dt = Math.min(40, Math.max(1, now - last)), n = dt / 16; last = now;
            var fk = shell.feels[shell.canvasOpts.feel] || shell.feels.soft, K = fk[0], damp = Math.pow(fk[1], n);
            var w = Object.assign({}, shell.world), t = Object.assign({}, shell.tgt), v = Object.assign({}, shell.vel);
            var sv = Object.assign({}, shell.springV), tl = Object.assign({}, shell.tilt), busy = false;
            for (var id in t) {
                var g = t[id], r = w[id];
                if (!r || !g) { delete t[id]; continue; }
                var vv = v[id] || [0, 0];
                if (vv[0] || vv[1]) {
                    // thrown: the target glides on, slowing; at rest, onto a neighbour
                    g = Qt.rect(g.x + vv[0] * dt, g.y + vv[1] * dt, g.width, g.height);
                    var f = Math.pow(.9, n);
                    vv = [vv[0] * f, vv[1] * f];
                    if (Math.sqrt(vv[0] * vv[0] + vv[1] * vv[1]) * shell.camZ < .03) { vv = [0, 0]; g = shell.snapRect(Number(id), g); }
                    t[id] = g; v[id] = vv; busy = true;
                }
                var s_ = sv[id] || [0, 0, 0, 0];
                s_ = [(s_[0] + (g.x - r.x) * K * n) * damp, (s_[1] + (g.y - r.y) * K * n) * damp,
                      (s_[2] + (g.width - r.width) * K * n) * damp, (s_[3] + (g.height - r.height) * K * n) * damp];
                var nr = Qt.rect(r.x + s_[0] * n, r.y + s_[1] * n, r.width + s_[2] * n, r.height + s_[3] * n);
                var held = mover.client && mover.group.indexOf(Number(id)) >= 0;
                // the lean: with the sideways speed while held or flying
                var lean = held && !mover.resize || vv[0] ? Math.max(-7, Math.min(7, s_[0] * shell.camZ * .5)) : 0;
                tl[id] = (tl[id] || 0) + (lean - (tl[id] || 0)) * Math.min(1, .25 * n);
                var far = Math.abs(g.x - nr.x) + Math.abs(g.y - nr.y) + Math.abs(g.width - nr.width) + Math.abs(g.height - nr.height)
                        + Math.abs(s_[0]) + Math.abs(s_[1]) > .4 || Math.abs(tl[id]) > .05;
                if (held || far || vv[0] || vv[1]) { w[id] = nr; sv[id] = s_; busy = true; }
                else { w[id] = g; delete t[id]; delete v[id]; delete sv[id]; delete tl[id]; }
            }
            shell.world = w; shell.tgt = t; shell.vel = v; shell.springV = sv; shell.tilt = tl;
            shell.relayout();
            if (!busy) stop();
        }
    }

    // ---- a window dragged to a screen edge takes the view along (the
    // pointer only resting there -- on the bar -- pans nothing)
    property int edgeHold: 0
    Timer {
        interval: 16; repeat: true
        running: shell.canvasHere && !shell.switching && !!mover.client
        onTriggered: {
            var p = zerp.cursor, ex = p.x <= 1 ? -1 : p.x >= shell.width - 2 ? 1 : 0, ey = p.y <= 1 ? -1 : p.y >= shell.height - 2 ? 1 : 0;
            if (!ex && !ey) { shell.edgeHold = 0; return; }
            if (++shell.edgeHold < 4) return;
            var step = 16 / shell.camZ;
            camAnim.stop();
            shell.camX += ex * step; shell.camY += ey * step;
            if (mover.client && !mover.resize) mover.shift(ex * step, ey * step);
        }
    }

    // ---- snapping: a moved window's edges catch its neighbours' (a gap
    // apart, or in line), within a few pixels on screen
    function snapRect(id, n) {
        if (canvasOpts.snap === false) return n;
        var d = 18 / camZ, g = gapIn, list = zerp.clients, bx = null, by = null;
        for (var i = 0; i < list.length; i++) {
            var o = world[list[i].id];
            if (list[i].id === id || !o || list[i].workspace !== zerp.workspace) continue;
            var vOver = n.y < o.y + o.height + d && o.y < n.y + n.height + d;
            var hOver = n.x < o.x + o.width + d && o.x < n.x + n.width + d;
            if (vOver) {
                var cx = [[o.x + o.width + g - n.x], [o.x - g - n.width - n.x], [o.x - n.x], [o.x + o.width - n.width - n.x]];
                for (var a = 0; a < cx.length; a++) if (Math.abs(cx[a][0]) < d && (bx === null || Math.abs(cx[a][0]) < Math.abs(bx))) bx = cx[a][0];
            }
            if (hOver) {
                var cy = [[o.y + o.height + g - n.y], [o.y - g - n.height - n.y], [o.y - n.y], [o.y + o.height - n.height - n.y]];
                for (var b = 0; b < cy.length; b++) if (Math.abs(cy[b][0]) < d && (by === null || Math.abs(cy[b][0]) < Math.abs(by))) by = cy[b][0];
            }
        }
        return Qt.rect(n.x + (bx || 0), n.y + (by || 0), n.width, n.height);
    }
    // the windows stuck to one (edges a gap apart, side by side): its cluster
    function clusterOf(id) {
        var list = zerp.clients, seen = {}, out = [id], g = gapIn + 3;
        seen[id] = true;
        for (var k = 0; k < out.length; k++) {
            var a = world[out[k]];
            for (var i = 0; i < list.length; i++) {
                var c = list[i], b = world[c.id];
                if (seen[c.id] || !b || c.workspace !== zerp.workspace) continue;
                var touchX = (Math.abs(a.x + a.width - b.x) <= g || Math.abs(b.x + b.width - a.x) <= g) && a.y < b.y + b.height && b.y < a.y + a.height;
                var touchY = (Math.abs(a.y + a.height - b.y) <= g || Math.abs(b.y + b.height - a.y) <= g) && a.x < b.x + b.width && b.x < a.x + a.width;
                if (touchX || touchY) { seen[c.id] = true; out.push(c.id); }
            }
        }
        return out;
    }

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

    // ---- the canvas's ground: a faint grid that moves with the camera;
    // drag it or scroll to move about, Ctrl+scroll to zoom (under the windows)
    Item {
        id: ground
        anchors.fill: parent
        z: 0.9
        visible: shell.canvasHere
        readonly property real step: { var st = 48 * shell.camZ; while (st < 22) st *= 2; return st; }
        readonly property real ox: ((-shell.camX * shell.camZ) % step + step) % step
        readonly property real oy: ((-shell.camY * shell.camZ) % step + step) % step
        // a dot grid, painted once per step and slid along (a step over
        // the screen's size: any offset is covered)
        Canvas {
            id: dots
            x: ground.ox - ground.step; y: ground.oy - ground.step
            width: shell.width + 2 * ground.step; height: shell.height + 2 * ground.step
            property real st: ground.step
            onStChanged: requestPaint()
            onWidthChanged: requestPaint()
            property string kind: shell.canvasOpts.bg
            onKindChanged: requestPaint()
            onPaint: {
                var g = getContext("2d"); g.reset();
                if (kind === "none") return;
                if (kind === "grid") {
                    g.fillStyle = Qt.rgba(1, 1, 1, .06);
                    for (var gx = 0; gx < width; gx += st) g.fillRect(gx, 0, 1, height);
                    for (var gy = 0; gy < height; gy += st) g.fillRect(0, gy, width, 1);
                    return;
                }
                g.fillStyle = Qt.rgba(1, 1, 1, .13);
                var r = Math.max(1.2, Math.min(2.2, st / 26));
                for (var x = 0; x < width; x += st) for (var y = 0; y < height; y += st) { g.beginPath(); g.arc(x, y, r, 0, 6.3); g.fill(); }
            }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            property point last
            property real lastT
            onPressed: m => { camAnim.stop(); fling.stop(); shell.flingX = 0; shell.flingY = 0; last = Qt.point(m.x, m.y); lastT = Date.now(); }
            onPositionChanged: m => {
                var now = Date.now(), dt = Math.max(1, now - lastT), dx = m.x - last.x, dy = m.y - last.y;
                shell.camX -= dx / shell.camZ; shell.camY -= dy / shell.camZ;
                // the speed, smoothed (px per ms)
                shell.flingX = shell.flingX * .6 + dx / dt * .4; shell.flingY = shell.flingY * .6 + dy / dt * .4;
                last = Qt.point(m.x, m.y); lastT = now;
            }
            onReleased: { if (shell.canvasOpts.inertia !== false && Date.now() - lastT < 60 && Math.abs(shell.flingX) + Math.abs(shell.flingY) > .25) fling.start(); }
            onWheel: w => shell.canvasWheel(w)
        }
    }

    // Alt + the mouse over a window on the canvas: left moves it, right
    // resizes it (from its bottom-right corner); without Alt the clicks go
    // through to the window
    MouseArea {
        id: mover
        anchors.fill: parent
        z: 1.5
        enabled: shell.canvasHere
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property var client: null
        property point last
        property real lastT: 0
        property real vx: 0
        property real vy: 0
        property bool resize: false
        property var group: []          // Alt+Shift: the cluster moves together
        property var free: null         // the window where the pointer would put it (before snapping)
        onPressed: m => {
            if (!(m.modifiers & Qt.AltModifier)) { m.accepted = false; return; }
            var t = tiles.childAt(m.x, m.y);
            if (!t || !t.client || !shell.world[t.client.id] || t.client.fullscreen) { m.accepted = false; return; }
            client = t.client; resize = m.button === Qt.RightButton; last = Qt.point(m.x, m.y); lastT = Date.now(); vx = vy = 0;
            group = (m.modifiers & Qt.ShiftModifier) && !resize ? shell.clusterOf(client.id) : [client.id];
            for (var i = 0; i < group.length; i++) shell.aim(group[i], shell.tgt[group[i]] || shell.world[group[i]], 0, 0);
            free = shell.tgt[client.id];
            if (!resize) shell.liftId = client.id;
            zerp.focus(client);
        }
        // where the held window (and its group) should be: the pointer's
        // place, snapped when single -- the window follows it on a spring
        function shift(dx, dy) {
            var r = shell.tgt[client.id];
            if (!r) return;
            if (resize) {
                free = Qt.rect(free.x, free.y, Math.max(240, free.width + dx), Math.max(140, free.height + dy));
                shell.aim(client.id, free, 0, 0);
            } else {
                free = Qt.rect(free.x + dx, free.y + dy, free.width, free.height);
                // snapping only for one window: a cluster keeps its own shape
                var n = group.length === 1 ? shell.snapRect(client.id, free) : free;
                var mx = n.x - r.x, my = n.y - r.y;
                for (var i = 0; i < group.length; i++) {
                    var g = shell.tgt[group[i]];
                    if (g) shell.aim(group[i], Qt.rect(g.x + mx, g.y + my, g.width, g.height), 0, 0);
                }
            }
        }
        onPositionChanged: m => {
            if (!client) return;
            var now = Date.now(), dt = Math.max(1, now - lastT);
            var dx = (m.x - last.x) / shell.camZ, dy = (m.y - last.y) / shell.camZ;
            vx = vx * .6 + dx / dt * .4; vy = vy * .6 + dy / dt * .4;
            last = Qt.point(m.x, m.y); lastT = now;
            shift(dx, dy);
        }
        onReleased: {
            // thrown: the group flies on with the hand's speed, slowing
            if (client && !resize && shell.canvasOpts.inertia !== false && Date.now() - lastT < 70 && Math.sqrt(vx * vx + vy * vy) * shell.camZ > .45)
                for (var i = 0; i < group.length; i++) shell.aim(group[i], shell.tgt[group[i]], vx, vy);
            shell.liftId = -1;
            client = null;
        }
        onWheel: w => { if (w.modifiers & Qt.ControlModifier) shell.canvasWheel(w); else w.accepted = false; }
    }

    // ---- Alt-Tab's row: the windows by recent use, the chosen one lit
    Rectangle {
        z: 130
        visible: shell.switching
        anchors.centerIn: parent
        width: Math.min(shell.width - 80, switchRow.implicitWidth + 32); height: switchRow.implicitHeight + 32
        radius: 28
        color: "#ee111318"
        Row {
            id: switchRow
            anchors.centerIn: parent
            spacing: 10
            Repeater {
                model: shell.switching ? shell.switchList : []
                Rectangle {
                    id: card
                    required property var modelData
                    required property int index
                    readonly property bool on: index === shell.switchSel
                    // the window's own picture, live (its tile, wherever it is)
                    readonly property Item src: shell.tileOf(modelData)
                    width: 260; height: 196
                    radius: on ? 30 : 22
                    color: on ? "#2b4a8a" : "#1d2129"
                    scale: on ? 1.05 : 1
                    Behavior on scale { NumberAnimation { duration: 300; easing.type: Easing.OutBack } }
                    Behavior on radius { NumberAnimation { duration: 300; easing.type: Easing.OutBack } }
                    Rectangle {
                        x: 12; y: 12; width: 236; height: 140; radius: 14
                        color: "#10131a"; clip: true
                        ShaderEffectSource {
                            readonly property real k: card.src ? Math.min(236 / Math.max(1, card.src.width), 140 / Math.max(1, card.src.height)) : 1
                            anchors.centerIn: parent
                            width: card.src ? card.src.width * k : 0; height: card.src ? card.src.height * k : 0
                            sourceItem: card.src && card.src.visible ? card.src : null
                            live: true; hideSource: false
                        }
                        Text { anchors.centerIn: parent; visible: !card.src || !card.src.visible; text: "стол " + card.modelData.workspace; color: "#6f7889"; font.pixelSize: 13 }
                    }
                    Text { x: 14; y: 158; width: 232; text: (modelData.appId || "?").split(".").pop() + (modelData.title ? " — " + modelData.title : ""); color: on ? "#ffffff" : "#eef2f8"; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight }
                    Text { x: 14; y: 176; text: "стол " + modelData.workspace + (shell.isCanvas(modelData.workspace) ? " · холст" : ""); color: on ? "#dbe6ff" : "#6f7889"; font.pixelSize: 11; font.bold: true }
                }
            }
        }
    }

    // what changed: the mode, the zoom -- a pill low in the middle, a moment
    Rectangle {
        id: canvasNote
        z: 120
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .8
        width: noteCol.implicitWidth + 44; height: noteCol.implicitHeight + 24
        radius: height / 2
        color: "#e6111318"
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220 } }
        function show(t, sub) { noteT.text = t; noteS.text = sub; opacity = 1; noteHide.restart(); }
        Timer { id: noteHide; interval: 1600; onTriggered: canvasNote.opacity = 0 }
        Column {
            id: noteCol
            anchors.centerIn: parent
            spacing: 2
            Text { id: noteT; anchors.horizontalCenter: parent.horizontalCenter; color: "#eef2f8"; font.pixelSize: 17; font.bold: true }
            Text { id: noteS; anchors.horizontalCenter: parent.horizontalCenter; color: "#9aa3b2"; font.pixelSize: 12; visible: text !== "" }
        }
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
                readonly property bool onCanvas: shell.isCanvas(client.workspace) && !client.fullscreen
                readonly property var cam: onCanvas ? shell.camOf(client.workspace) : null
                pan: onCanvas ? Qt.point(cam.x, cam.y) : Qt.point(0, 0)
                zoom: onCanvas ? cam.z : 1
                animate: !onCanvas || shell.morphing
                lift: shell.liftId === client.id ? 1.045 : 1
                lean: shell.tilt[client.id] || 0
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

    // ---- the session locked (zsessionlock.h): black over everything, the
    // lock screen alone on it, every click and key its. Its client gone
    // with the session locked: still black, and our own password field.
    Rectangle {
        id: lockCover
        anchors.fill: parent
        z: 1000
        color: "black"
        visible: opacity > 0
        opacity: sessionLock.locked ? 1 : 0
        // (on: at once -- nothing may show; off: a short fade)
        Behavior on opacity { enabled: !sessionLock.locked; NumberAnimation { duration: 250 } }
        MouseArea {
            anchors.fill: parent
            enabled: sessionLock.locked
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: w => w.accepted = true
            onPressed: if (sessionLock.abandoned) lockPass.forceActiveFocus()
        }
        WaylandQuickItem {
            id: lockItem
            anchors.fill: parent
            surface: sessionLock.surface ? sessionLock.surface.surface : null
            visible: !!sessionLock.surface && sessionLock.surface.mapped
            onVisibleChanged: if (visible) takeFocus()
        }
        Connections {
            target: sessionLock
            function onLockedChanged() { if (sessionLock.abandoned) { lockPass.text = ""; lockPass.forceActiveFocus(); } }
            function onPasswordChecked(ok) { lockPass.text = ""; lockFail.visible = !ok; lockBusy.visible = false; }
        }
        Column {
            anchors.centerIn: parent
            spacing: 16
            visible: sessionLock.abandoned
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Сеанс заблокирован"; color: "#eef2f8"; font.pixelSize: 28; font.bold: true }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Экран блокировки закрылся. Пароль " + userName + ":"; color: "#8b95a6"; font.pixelSize: 15 }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 320; height: 44; radius: 22
                color: "#1d2129"
                border.width: 2; border.color: lockFail.visible ? "#e0707a" : "#3d7eff"
                TextInput {
                    id: lockPass
                    anchors.fill: parent; anchors.margins: 12
                    verticalAlignment: TextInput.AlignVCenter
                    echoMode: TextInput.Password
                    color: "#eef2f8"; font.pixelSize: 16
                    onAccepted: { if (text) { lockBusy.visible = true; lockFail.visible = false; sessionLock.tryPassword(text); } }
                }
            }
            Text { id: lockBusy; visible: false; anchors.horizontalCenter: parent.horizontalCenter; text: "Проверка…"; color: "#8b95a6"; font.pixelSize: 13 }
            Text { id: lockFail; visible: false; anchors.horizontalCenter: parent.horizontalCenter; text: "Неверный пароль"; color: "#e0707a"; font.pixelSize: 13 }
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

}
