// The dock (after macOS): frosted shelf, icons magnify under the pointer
// and rise out of it, a dot under running apps. Click: focus the app if
// it runs, else start it. The shelf never changes size, so its frosted,
// rounded background is rendered once.
import QtQuick
import QtQuick.Effects

Item {
    id: dock
    property real screenW: 0
    property real screenH: 0
    readonly property int baseSize: 48
    readonly property int maxSize: 72
    readonly property int spacing: 10
    property real mouseX: -1000

    readonly property var apps: [
        { name: "Terminal", path: "/usr/bin/term", appId: "term", kind: "term" },
        { name: "Files", path: "/usr/bin/files", appId: "files", kind: "files" },
        { name: "Web", path: "/usr/bin/web", appId: "web", kind: "web" },
        { name: "Launcher", path: "/zerp_rofi.elf", appId: "zerp_rofi", kind: "launcher" },
        { name: "Qt Quick", path: "/usr/bin/qmldemo", appId: "qmldemo", kind: "qt" }
    ]

    width: apps.length * baseSize + (apps.length - 1) * spacing + 24
    height: baseSize + 22

    function running(appId) {
        return zerp.clients.some(function (c) { return c.appId === appId })
    }
    function activate(app) {
        var list = zerp.clients;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i].appId === app.appId) {
                zerp.workspace = list[i].workspace;
                zerp.focus(list[i]);
                return;
            }
        }
        zerp.spawn(app.path);
    }

    // ---- shelf: frosted slice + tint, rounded by a mask; all static
    Item {
        id: shelf
        anchors.fill: parent
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: shelfMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        Glass { anchors.fill: parent; gx: dock.x; gy: dock.y }
        Rectangle { anchors.fill: parent; color: "#10131a"; opacity: 0.5 }
    }
    Item {
        id: shelfMask
        anchors.fill: parent
        layer.enabled: true
        visible: false
        Rectangle { anchors.fill: parent; radius: 18; color: "black"; antialiasing: true }
    }
    Rectangle { anchors.fill: parent; radius: 18; color: "transparent"; border.color: "#ffffff"; border.width: 1; opacity: 0.12 }

    HoverHandler {
        id: hover
        onPointChanged: dock.mouseX = point.position.x
        onHoveredChanged: if (!hovered) dock.mouseX = -1000
    }

    // icons sit on fixed slots; magnified ones grow upwards out of the shelf
    Repeater {
        model: dock.apps
        delegate: Item {
            id: slot
            required property var modelData
            required property int index
            readonly property real centre: 12 + index * (dock.baseSize + dock.spacing) + dock.baseSize / 2
            readonly property real k: Math.max(0, 1 - Math.abs(dock.mouseX - centre) / 130)
            property real size: dock.baseSize + (dock.maxSize - dock.baseSize) * k
            x: centre - size / 2
            y: dock.height - 11 - size
            width: size
            height: size
            Behavior on size { NumberAnimation { duration: 80 } }

            DockIcon { anchors.fill: parent; kind: slot.modelData.kind }
            Rectangle {   // running dot
                width: 4; height: 4; radius: 2
                color: "#e6e8ef"
                x: (slot.width - width) / 2
                y: slot.height + 3
                visible: dock.running(slot.modelData.appId)
            }
            Rectangle {   // name tooltip
                visible: slot.k > 0.85
                x: (slot.width - width) / 2
                y: -height - 10
                width: tip.implicitWidth + 16; height: 24; radius: 7
                color: "#1d2130"; border.color: "#3b4261"
                Text { id: tip; anchors.centerIn: parent; text: slot.modelData.name; color: "#e6e8ef"; font.pixelSize: 12 }
            }
            MouseArea { anchors.fill: parent; onClicked: dock.activate(slot.modelData) }
        }
    }
}
