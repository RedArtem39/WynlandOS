// Dock icons, drawn (no icon theme in the image yet).
import QtQuick

Item {
    id: icon
    property string kind: "term"
    readonly property real u: width / 48

    Rectangle {
        anchors.fill: parent
        radius: 11 * icon.u
        antialiasing: true
        gradient: Gradient {
            GradientStop { position: 0; color: icon.kind === "term" ? "#2b2f3a" : icon.kind === "files" ? "#4ea8ff" : icon.kind === "launcher" ? "#a066ff" : "#41cd52" }
            GradientStop { position: 1; color: icon.kind === "term" ? "#14161c" : icon.kind === "files" ? "#2a6fd6" : icon.kind === "launcher" ? "#6a3fd1" : "#2a9a3a" }
        }
        border.color: "#ffffff"; border.width: 1 * icon.u
        Rectangle { anchors.fill: parent; radius: parent.radius; color: "white"; opacity: 0.06 }
    }

    // terminal: prompt
    Text {
        visible: icon.kind === "term"
        anchors { left: parent.left; leftMargin: 9 * icon.u; verticalCenter: parent.verticalCenter }
        text: ">_"
        color: "#7ee787"
        font { family: "DejaVu Sans Mono"; pixelSize: 18 * icon.u; bold: true }
    }
    // files: folder
    Item {
        visible: icon.kind === "files"
        anchors.centerIn: parent
        width: 28 * icon.u; height: 21 * icon.u
        Rectangle { width: 12 * icon.u; height: 6 * icon.u; radius: 2 * icon.u; color: "#dff1ff" }
        Rectangle { y: 3 * icon.u; width: parent.width; height: parent.height - 3 * icon.u; radius: 3 * icon.u; color: "#eaf6ff" }
    }
    // launcher: 3x3 grid
    Grid {
        visible: icon.kind === "launcher"
        anchors.centerIn: parent
        columns: 3; spacing: 4 * icon.u
        Repeater {
            model: 9
            Rectangle { width: 6 * icon.u; height: 6 * icon.u; radius: 3 * icon.u; color: "white" }
        }
    }
    // Qt
    Text {
        visible: icon.kind === "qt"
        anchors.centerIn: parent
        text: "Qt"
        color: "white"
        font { pixelSize: 20 * icon.u; bold: true }
    }
}
