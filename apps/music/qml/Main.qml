// WynlandOS - Music: a small player in Material 3 Expressive.
//
// What plays: the cover (its corners rounder while it plays), the title,
// the artist, the wavy progress (press or drag it to seek), the controls
// -- shuffle, back, the play pill, next, repeat -- and the volume. The
// list: ~/Music and /usr/share/wynland/music (wide: beside; narrow: a
// sheet from the bottom). The colours: Material You's, from the cover
// (Google's material-color-utilities, mcu.js) -- no cover: from the
// title's own hue. The playing itself, the library and MPRIS: `player`
// (main.cpp).
//
// Keys: Space play/pause, Left/Right 5 s, Up/Down the volume, N next,
// P back, S shuffle, R repeat, L the list.
import QtQuick
import QtQuick.Window
import "mcu.js" as Mcu

Window {
    id: win
    visible: true
    width: Screen.width > 0 ? Math.min(Screen.width, 960) : 960
    height: Screen.height > 0 ? Math.min(Screen.height, 640) : 640
    color: m3.surface
    title: "Музыка"

    readonly property bool wide: width >= 760
    property bool listOpen: false

    FontLoader { id: roboto; source: "file:///usr/share/xvlet/assets/fonts/RobotoFlex.ttf" }
    FontLoader { id: symbols; source: "file:///usr/share/xvlet/assets/fonts/MaterialSymbolsRounded.ttf" }
    readonly property string font: roboto.status === FontLoader.Ready ? roboto.name : "Adwaita Sans"
    readonly property bool icons: symbols.status === FontLoader.Ready

    // ---- the colours: Material You's, from the cover ----
    Item {
        id: m3
        visible: false
        function hash(s) { let h = 0; for (const c of (s || "?")) h = (h * 31 + c.charCodeAt(0)) % 360; return h; }
        readonly property int seed: {
            const cs = player.coverColors || [];
            if (cs.length) {
                const pop = new Map();
                for (const e of cs) pop.set(e[0] >>> 0, e[1]);
                try {
                    const s = Mcu.MCU.Score.score(pop, { desired: 1, fallbackColorARGB: 0, filter: true });
                    if (s.length && s[0] !== 0) return s[0];
                } catch (e) {}
            }
            // no cover (or a colourless one): the title's own hue
            try { return Mcu.MCU.Hct.from(hash(player.title), 48, 55).toInt(); } catch (e) { return 0xff7c6aa8; }
        }
        readonly property var sc: { try { return new Mcu.MCU.SchemeTonalSpot(Mcu.MCU.Hct.fromInt(seed), true, 0, "2025"); } catch (e) { return null; } }
        function c(name, fb) { if (!sc) return fb; const a = sc[name]; return Qt.rgba(((a >> 16) & 255) / 255, ((a >> 8) & 255) / 255, (a & 255) / 255, 1); }
        property color surface: c("surface", "#141218")
        property color low: c("surfaceContainerLow", "#1d1b20")
        property color mid: c("surfaceContainer", "#211f26")
        property color high: c("surfaceContainerHigh", "#2b2930")
        property color highest: c("surfaceContainerHighest", "#36343b")
        property color primary: c("primary", "#d0bcff")
        property color onPrimary: c("onPrimary", "#381e72")
        property color primaryContainer: c("primaryContainer", "#4f378b")
        property color onPrimaryContainer: c("onPrimaryContainer", "#eaddff")
        property color secondaryContainer: c("secondaryContainer", "#4a4458")
        property color onSecondaryContainer: c("onSecondaryContainer", "#e8def8")
        property color tertiary: c("tertiary", "#efb8c8")
        property color fg: c("onSurface", "#e6e0e9")
        property color dim: c("onSurfaceVariant", "#cac4d0")
        property color outline: c("outlineVariant", "#49454f")
    }

    // the background, its colour easing into the next cover's (an animation
    // on the theme's own properties crashed Qt's QML engine here)
    Rectangle {
        anchors.fill: parent
        color: m3.surface
        Behavior on color { ColorAnimation { duration: 600 } }
    }

    function fmt(ms) { const s = Math.max(0, Math.floor(ms / 1000)); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); }

    // ---- small pieces ----
    component Txt: Text {
        property real size: 14
        property int w: 500
        font.family: win.font
        font.pixelSize: size
        font.weight: w
        color: m3.fg
        elide: Text.ElideRight
    }
    component Icon: Text {
        property string name
        property real size: 24
        property bool fill: true
        text: win.icons ? name : ({ play_arrow: "▶", pause: "❚❚", skip_next: "⏭", skip_previous: "⏮", shuffle: "⤮", repeat: "⟳", repeat_one: "⟳¹", volume_up: "🔊", volume_off: "🔇", queue_music: "☰", close: "✕", music_note: "♪" })[name] || "•"
        font.family: win.icons ? symbols.name : win.font
        font.pixelSize: size
        font.variableAxes: ({ FILL: fill ? 1 : 0, wght: 500, opsz: 24 })
        color: m3.fg
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    // a round button whose corners tighten under the pointer, smaller when pressed
    component Btn: Rectangle {
        id: b
        property string icon
        property real size: 48
        property bool on: false
        property color tint: on ? m3.onSecondaryContainer : m3.fg
        signal clicked
        width: size; height: size
        radius: ba.containsMouse ? size * .32 : size / 2
        color: on ? m3.secondaryContainer : ba.containsMouse ? Qt.rgba(m3.fg.r, m3.fg.g, m3.fg.b, .1) : "transparent"
        scale: ba.pressed ? .9 : 1
        Behavior on radius { NumberAnimation { duration: 300; easing.type: Easing.OutBack } }
        Behavior on scale { NumberAnimation { duration: 250; easing.type: Easing.OutBack } }
        Behavior on color { ColorAnimation { duration: 200 } }
        Icon { anchors.centerIn: parent; name: b.icon; size: b.size * .5; color: b.tint }
        MouseArea { id: ba; anchors.fill: parent; hoverEnabled: true; onClicked: b.clicked() }
    }
    // a picture with rounded corners (the software renderer has no masks:
    // drawn by a canvas, clipped)
    component Cover: Canvas {
        id: cv
        property string src
        property real r: 24
        property color a: m3.primaryContainer
        property color b2: m3.tertiary
        property string glyph: "music_note"
        onSrcChanged: { if (src) loadImage(src); requestPaint(); }
        onImageLoaded: requestPaint()
        onRChanged: requestPaint()
        onAChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const g = getContext("2d"), W = width, H = height, rr = Math.min(r, W / 2, H / 2);
            g.reset();
            g.beginPath();
            g.moveTo(rr, 0); g.lineTo(W - rr, 0); g.arcTo(W, 0, W, rr, rr); g.lineTo(W, H - rr); g.arcTo(W, H, W - rr, H, rr);
            g.lineTo(rr, H); g.arcTo(0, H, 0, H - rr, rr); g.lineTo(0, rr); g.arcTo(0, 0, rr, 0, rr); g.closePath();
            g.save(); g.clip();
            if (src && isImageLoaded(src)) g.drawImage(src, 0, 0, W, H);
            else {
                const gr = g.createLinearGradient(0, 0, W, H);
                gr.addColorStop(0, b2); gr.addColorStop(1, a);
                g.fillStyle = gr; g.fillRect(0, 0, W, H);
            }
            g.restore();
        }
        Icon { anchors.centerIn: parent; visible: !cv.src; name: cv.glyph; size: Math.min(cv.width, cv.height) * .38; color: m3.onPrimaryContainer }
    }

    // ---- what plays ----
    Item {
        id: now
        x: 0; y: 0
        width: win.wide ? Math.min(win.width * .55, 560) : win.width
        height: win.height

        Column {
            id: nc
            anchors.centerIn: parent
            width: Math.min(now.width - 64, 420)
            spacing: 0

            Cover {
                id: big
                width: Math.min(nc.width, win.height * .46); height: width
                anchors.horizontalCenter: parent.horizontalCenter
                src: player.cover
                // rounder while it plays (M3 Expressive's shape change)
                r: player.playing ? width * .22 : width * .1
                Behavior on r { NumberAnimation { duration: 600; easing.type: Easing.OutBack } }
                scale: player.playing ? 1 : .94
                Behavior on scale { NumberAnimation { duration: 600; easing.type: Easing.OutBack } }
            }
            Item { width: 1; height: 26 }
            Txt { width: parent.width; text: player.title || "Тишина"; size: 24; w: 700; horizontalAlignment: Text.AlignHCenter }
            Txt { width: parent.width; text: player.artist || (player.tracks.length ? "" : "Положите музыку в ~/Music"); size: 15; w: 500; color: m3.dim; horizontalAlignment: Text.AlignHCenter; topPadding: 4 }
            Item { width: 1; height: 18 }

            // the wavy progress: a wave up to the thumb, flat after it;
            // flat while paused or dragged
            Canvas {
                id: wave
                width: parent.width; height: 30
                property real v: player.duration > 0 ? player.position / player.duration : 0
                property real amp: player.playing && !wm.pressed ? 4 : 0
                property real phase: 0
                property color fgc: m3.primary
                property color trc: m3.highest
                Behavior on amp { NumberAnimation { duration: 400 } }
                Timer { interval: 50; repeat: true; running: player.playing && win.visible; onTriggered: wave.phase = (wave.phase + Math.PI * 2 * 50 / 1300) % (Math.PI * 2) }
                onPhaseChanged: requestPaint(); onVChanged: requestPaint(); onAmpChanged: requestPaint(); onFgcChanged: requestPaint()
                onPaint: {
                    const g = getContext("2d"), W = width, H = height, m = H / 2, P = 6;
                    const x1 = P + (W - P * 2) * Math.max(0, Math.min(1, wm.pressed ? wm.drag : v));
                    g.reset(); g.lineCap = "round"; g.lineWidth = 5;
                    g.strokeStyle = trc; g.beginPath(); g.moveTo(Math.min(W - P, x1 + 12), m); g.lineTo(W - P, m); g.stroke();
                    g.strokeStyle = fgc; g.beginPath();
                    for (let x = P; x <= x1 - 8; x += 1) { const y = m + Math.sin((x - P) / 40 * Math.PI * 2 - phase) * amp; x === P ? g.moveTo(x, y) : g.lineTo(x, y); }
                    g.stroke();
                    const th = wm.pressed ? H - 2 : H * .7, tw = wm.pressed ? 7 : 5;
                    g.fillStyle = fgc; g.beginPath(); g.roundedRect(x1 - tw / 2, m - th / 2, tw, th, tw / 2, tw / 2); g.fill();
                }
                MouseArea {
                    id: wm
                    anchors.fill: parent
                    property real drag: 0
                    function at(x) { return Math.max(0, Math.min(1, (x - 6) / (width - 12))); }
                    onPressed: e => { drag = at(e.x); wave.requestPaint(); }
                    onPositionChanged: e => { if (pressed) { drag = at(e.x); wave.requestPaint(); } }
                    onReleased: player.seek(drag * player.duration)
                }
            }
            Item {
                width: parent.width; height: 18
                Txt { text: win.fmt(wm.pressed ? wm.drag * player.duration : player.position); size: 12; w: 600; color: m3.dim; font.features: { "tnum": 1 } }
                Txt { anchors.right: parent.right; text: win.fmt(player.duration); size: 12; w: 600; color: m3.dim; font.features: { "tnum": 1 } }
            }
            Item { width: 1; height: 16 }

            // the controls
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                Btn { icon: "shuffle"; size: 44; on: player.shuffle; anchors.verticalCenter: parent.verticalCenter; onClicked: player.shuffle = !player.shuffle }
                Btn { icon: "skip_previous"; size: 56; color: m3.high; anchors.verticalCenter: parent.verticalCenter; onClicked: player.prev() }
                // the play pill: wider and rounder while paused, tighter while playing
                Rectangle {
                    id: pp
                    width: (player.playing ? 84 : 100) + (pa.containsMouse ? 6 : 0); height: 72
                    radius: player.playing ? 22 : 36
                    color: m3.primary
                    Behavior on color { ColorAnimation { duration: 600 } }
                    scale: pa.pressed ? .92 : 1
                    anchors.verticalCenter: parent.verticalCenter
                    Behavior on width { NumberAnimation { duration: 450; easing.type: Easing.OutBack } }
                    Behavior on radius { NumberAnimation { duration: 450; easing.type: Easing.OutBack } }
                    Behavior on scale { NumberAnimation { duration: 250; easing.type: Easing.OutBack } }
                    Icon { anchors.centerIn: parent; name: player.playing ? "pause" : "play_arrow"; size: 36; color: m3.onPrimary }
                    MouseArea { id: pa; anchors.fill: parent; hoverEnabled: true; onClicked: player.toggle() }
                }
                Btn { icon: "skip_next"; size: 56; color: m3.high; anchors.verticalCenter: parent.verticalCenter; onClicked: player.next() }
                Btn { icon: player.repeat === 2 ? "repeat_one" : "repeat"; size: 44; on: player.repeat > 0; anchors.verticalCenter: parent.verticalCenter; onClicked: player.repeat = (player.repeat + 1) % 3 }
            }
            Item { width: 1; height: 22 }

            // the volume
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Icon { name: player.volume === 0 ? "volume_off" : "volume_up"; size: 20; color: m3.dim; anchors.verticalCenter: parent.verticalCenter
                       MouseArea { anchors.fill: parent; onClicked: player.volume = player.volume > 0 ? 0 : .8 } }
                Rectangle {
                    id: vol
                    width: 220; height: 14; radius: 7
                    color: m3.highest
                    anchors.verticalCenter: parent.verticalCenter
                    Rectangle { width: Math.max(14, parent.width * player.volume); height: parent.height; radius: 7; color: m3.primary }
                    MouseArea {
                        anchors.fill: parent; anchors.margins: -8
                        function at(x) { player.volume = Math.max(0, Math.min(1, (x - 8) / vol.width)); }
                        onPressed: e => at(e.x)
                        onPositionChanged: e => { if (pressed) at(e.x); }
                        onWheel: w => player.volume = Math.max(0, Math.min(1, player.volume + (w.angleDelta.y > 0 ? .05 : -.05)))
                    }
                }
                Btn { icon: "queue_music"; size: 40; visible: !win.wide; on: win.listOpen; anchors.verticalCenter: parent.verticalCenter; onClicked: win.listOpen = !win.listOpen }
            }
        }
    }

    // ---- the list: beside (wide) or a sheet from the bottom (narrow) ----
    Rectangle {
        id: list
        readonly property real sheetH: win.height * .62
        x: win.wide ? now.width : 0
        y: win.wide ? 0 : (win.listOpen ? win.height - sheetH : win.height + 8)
        width: win.wide ? win.width - now.width : win.width
        height: win.wide ? win.height : sheetH
        color: m3.low
        topLeftRadius: win.wide ? 28 : 28; bottomLeftRadius: win.wide ? 28 : 0; topRightRadius: win.wide ? 0 : 28
        Behavior on y { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }

        Txt { x: 24; y: 22; text: "Очередь"; size: 20; w: 700 }
        Txt { x: 24; y: 50; text: player.tracks.length + " " + (player.tracks.length % 10 === 1 && player.tracks.length % 100 !== 11 ? "трек" : player.tracks.length % 10 >= 2 && player.tracks.length % 10 <= 4 && (player.tracks.length % 100 < 12 || player.tracks.length % 100 > 14) ? "трека" : "треков"); size: 12.5; color: m3.dim }
        Btn { visible: !win.wide; icon: "close"; size: 40; anchors.right: parent.right; anchors.rightMargin: 16; y: 18; onClicked: win.listOpen = false }

        ListView {
            id: lv
            x: 12; y: 84
            width: parent.width - 24; height: parent.height - 96
            clip: true
            spacing: 4
            model: player.tracks
            currentIndex: player.index
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool cur: index === player.index
                width: lv.width; height: 64
                radius: ra.containsMouse || cur ? 20 : 14
                color: cur ? m3.secondaryContainer : ra.containsMouse ? Qt.rgba(m3.fg.r, m3.fg.g, m3.fg.b, .06) : "transparent"
                Behavior on radius { NumberAnimation { duration: 250 } }
                Behavior on color { ColorAnimation { duration: 200 } }
                Cover { x: 10; anchors.verticalCenter: parent.verticalCenter; width: 44; height: 44; r: 12; src: row.modelData.cover }
                Column {
                    x: 66; anchors.verticalCenter: parent.verticalCenter
                    width: row.width - 66 - 70
                    Txt { width: parent.width; text: row.modelData.title; size: 14.5; w: 650; color: row.cur ? m3.onSecondaryContainer : m3.fg }
                    Txt { width: parent.width; text: row.modelData.artist; size: 12.5; color: row.cur ? m3.onSecondaryContainer : m3.dim; visible: text !== "" }
                }
                // playing: little bars; else the length
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter
                    spacing: 3; visible: row.cur && player.playing
                    Repeater {
                        model: 3
                        Rectangle {
                            id: eq
                            required property int index
                            width: 3.5; radius: 2; color: m3.primary
                            anchors.verticalCenter: parent.verticalCenter
                            height: 6
                            SequentialAnimation on height {
                                running: row.cur && player.playing; loops: Animation.Infinite
                                PauseAnimation { duration: eq.index * 140 }
                                NumberAnimation { to: 18; duration: 420; easing.type: Easing.InOutQuad }
                                NumberAnimation { to: 6; duration: 420; easing.type: Easing.InOutQuad }
                            }
                        }
                    }
                }
                Txt { anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; visible: !(row.cur && player.playing) && row.modelData.length > 0
                      text: win.fmt(row.modelData.length); size: 12.5; color: m3.dim; font.features: { "tnum": 1 } }
                MouseArea { id: ra; anchors.fill: parent; hoverEnabled: true; onClicked: player.playAt(row.index) }
            }
        }
    }

    // ---- keys ----
    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: e => {
            if (e.key === Qt.Key_Space) player.toggle();
            else if (e.key === Qt.Key_Right) player.seek(player.position + 5000);
            else if (e.key === Qt.Key_Left) player.seek(player.position - 5000);
            else if (e.key === Qt.Key_Up) player.volume = Math.min(1, player.volume + .05);
            else if (e.key === Qt.Key_Down) player.volume = Math.max(0, player.volume - .05);
            else if (e.key === Qt.Key_N) player.next();
            else if (e.key === Qt.Key_P) player.prev();
            else if (e.key === Qt.Key_S) player.shuffle = !player.shuffle;
            else if (e.key === Qt.Key_R) player.repeat = (player.repeat + 1) % 3;
            else if (e.key === Qt.Key_L) win.listOpen = !win.listOpen;
            else return;
            e.accepted = true;
        }
    }
}
