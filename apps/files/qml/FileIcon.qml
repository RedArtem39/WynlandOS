// A file's icon, drawn (no icon theme on WynlandOS yet): folders, pages
// with a coloured extension band, a terminal tile for programs, and a
// real thumbnail for images.
import QtQuick

Item {
    id: root
    property string kind: "other"
    property string name: ""
    property string path: ""
    property bool thumbnail: true
    readonly property string ext: {
        const i = name.lastIndexOf(".")
        return i > 0 ? name.substring(i + 1).toUpperCase().substring(0, 4) : ""
    }
    readonly property color band: kind === "image" ? "#e3b341"
                                : kind === "archive" ? "#ff7b72"
                                : kind === "text" ? "#7ee787"
                                : "#8a91a8"

    // ---- folder
    Item {
        visible: root.kind === "dir"
        anchors.fill: parent
        Rectangle {   // tab
            x: parent.width * 0.08; y: parent.height * 0.16
            width: parent.width * 0.38; height: parent.height * 0.2
            radius: width * 0.12
            color: "#4a6fc0"
        }
        Rectangle {   // body
            x: parent.width * 0.06; y: parent.height * 0.26
            width: parent.width * 0.88; height: parent.height * 0.6
            radius: width * 0.08
            gradient: Gradient {
                GradientStop { position: 0; color: "#8fb2ff" }
                GradientStop { position: 1; color: "#5b84e0" }
            }
            Rectangle {   // lip highlight
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                height: 1; color: "#c8d9ff"; opacity: 0.7
            }
        }
    }

    // ---- program
    Rectangle {
        visible: root.kind === "exec"
        x: parent.width * 0.12; y: parent.height * 0.14
        width: parent.width * 0.76; height: parent.height * 0.72
        radius: width * 0.16
        gradient: Gradient {
            GradientStop { position: 0; color: "#2b3045" }
            GradientStop { position: 1; color: "#171a26" }
        }
        border.color: "#3b4261"
        Text {
            anchors.centerIn: parent
            text: ">_"
            color: "#7ee787"
            font.family: "DejaVu Sans Mono"
            font.bold: true
            font.pixelSize: parent.height * 0.32
        }
    }

    // ---- page (text / archive / other, and images until the thumbnail loads)
    Item {
        visible: root.kind !== "dir" && root.kind !== "exec" && !(thumb.status === Image.Ready)
        anchors.fill: parent
        Rectangle {
            id: page
            x: parent.width * 0.2; y: parent.height * 0.08
            width: parent.width * 0.6; height: parent.height * 0.84
            radius: 3
            color: "#e6e8ef"
            Rectangle {   // folded corner
                width: page.width * 0.3; height: width
                anchors { top: parent.top; right: parent.right }
                color: "#b9bfd0"
                radius: 2
            }
            Column {   // text lines
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: page.width * 0.14; topMargin: page.height * 0.3 }
                spacing: page.height * 0.06
                visible: root.kind === "text"
                Repeater {
                    model: 4
                    Rectangle { width: parent.width * (index === 3 ? 0.6 : 1); height: Math.max(1, page.height * 0.035); color: "#b9bfd0" }
                }
            }
            Rectangle {   // extension band
                visible: root.ext !== ""
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: page.height * 0.12 }
                anchors.leftMargin: -page.width * 0.1
                anchors.rightMargin: page.width * 0.15
                height: Math.max(10, page.height * 0.22)
                radius: 2
                color: root.band
                Text {
                    anchors.centerIn: parent
                    text: root.ext
                    color: "#10131a"
                    font.bold: true
                    font.pixelSize: Math.max(7, parent.height * 0.62)
                }
            }
        }
    }

    // ---- image thumbnail
    Image {
        id: thumb
        anchors.fill: parent
        anchors.margins: parent.width * 0.06
        visible: status === Image.Ready
        source: root.kind === "image" && root.thumbnail && root.path !== "" ? "file://" + root.path : ""
        sourceSize.width: 128
        sourceSize.height: 128
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
    }
}
