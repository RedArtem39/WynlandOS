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
            // graphite tiles; the logo's blue for the one that is blue by nature (files)
            GradientStop { position: 0; color: icon.kind === "files" ? "#4b8dff" : "#232831" }
            GradientStop { position: 1; color: icon.kind === "files" ? "#2c63d8" : "#14171c" }
        }
        border.color: "#2e343e"; border.width: 1 * icon.u
        Rectangle { anchors.fill: parent; radius: parent.radius; color: "white"; opacity: 0.06 }
    }

    // terminal: prompt
    Text {
        visible: icon.kind === "term"
        anchors { left: parent.left; leftMargin: 9 * icon.u; verticalCenter: parent.verticalCenter }
        text: ">_"
        color: "#6fb3ff"
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
    // web: a globe (outline, a meridian, the equator)
    Item {
        visible: icon.kind === "web"
        anchors.centerIn: parent
        width: 28 * icon.u; height: 28 * icon.u
        Rectangle { anchors.fill: parent; radius: width / 2; color: "transparent"; border.color: "white"; border.width: 2.2 * icon.u; antialiasing: true }
        Rectangle { anchors.centerIn: parent; width: parent.width * 0.44; height: parent.height; radius: width / 2; color: "transparent"; border.color: "white"; border.width: 2 * icon.u; antialiasing: true }
        Rectangle { anchors.centerIn: parent; width: parent.width; height: 2 * icon.u; color: "white" }
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
