// The dock (after macOS): frosted shelf, icons magnify under the pointer,
// a dot under running apps. Click: focus the app if it runs, else start it.
import QtQuick
import QtQuick.Effects

Item {
    id: dock
    property Item blurSource
    readonly property int baseSize: 48
    readonly property int maxSize: 74
    property real mouseX: -1000

    readonly property var apps: [
        { name: "Terminal", path: "/zerp_term.elf", appId: "zerp_term", kind: "term" },
        { name: "Files", path: "/zerp_files.elf", appId: "zerp_files", kind: "files" },
        { name: "Launcher", path: "/zerp_rofi.elf", appId: "zerp_rofi", kind: "launcher" },
        { name: "Qt Quick", path: "/usr/bin/qmldemo", appId: "qmldemo", kind: "qt" }
    ]

    width: row.width + 24
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

    // shelf
    ShaderEffectSource {
        id: under
        anchors.fill: shelf
        sourceItem: dock.blurSource
        sourceRect: Qt.rect(dock.x + shelf.x, dock.y + shelf.y, shelf.width, shelf.height)
        visible: false
    }
    Item {
        id: shelfMask
        anchors.fill: shelf
        layer.enabled: true
        visible: false
        Rectangle { anchors.fill: parent; radius: 18; color: "black" }
    }
    MultiEffect {
        anchors.fill: shelf
        source: under
        blurEnabled: true
        blur: 1.0
        blurMax: 48
        saturation: 0.3
        maskEnabled: true
        maskSource: shelfMask
    }
    Rectangle {
        id: shelf
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: dock.baseSize + 22
        radius: 18
        color: "#10131a"
        opacity: 0.5
        border.color: "#ffffff"
        border.width: 1
    }

    HoverHandler {
        id: hover
        onPointChanged: dock.mouseX = point.position.x
        onHoveredChanged: if (!hovered) dock.mouseX = -1000
    }

    Row {
        id: row
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 11 }
        spacing: 10
        Repeater {
            model: dock.apps
            delegate: Item {
                id: slot
                required property var modelData
                // magnification: grows with closeness to the pointer
                readonly property real centre: x + width / 2 + row.x
                readonly property real d: Math.abs(dock.mouseX - centre)
                readonly property real k: Math.max(0, 1 - d / 140)
                width: dock.baseSize + (dock.maxSize - dock.baseSize) * k
                height: width
                anchors.bottom: parent.bottom
                Behavior on width { NumberAnimation { duration: 90 } }

                DockIcon {
                    anchors.fill: parent
                    kind: slot.modelData.kind
                }
                Rectangle {   // running dot
                    width: 4; height: 4; radius: 2
                    color: "#e6e8ef"
                    anchors { horizontalCenter: parent.horizontalCenter; top: parent.bottom; topMargin: 3 }
                    visible: dock.running(slot.modelData.appId)
                }
                // name tooltip
                Rectangle {
                    visible: slot.k > 0.85
                    anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.top; bottomMargin: 10 }
                    width: tip.implicitWidth + 16; height: 24; radius: 7
                    color: "#1d2130"; border.color: "#3b4261"
                    Text { id: tip; anchors.centerIn: parent; text: slot.modelData.name; color: "#e6e8ef"; font.pixelSize: 12 }
                }
                MouseArea { anchors.fill: parent; onClicked: dock.activate(slot.modelData) }
            }
        }
    }
}
