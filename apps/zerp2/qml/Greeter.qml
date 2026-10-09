// Zerp 2.0's login screen (zerp2 --greeter, run by wynlogin). Flat and
// quiet: the wallpaper, a large monospaced clock, one input bar -- the
// account and its password -- and a status line; pills for the accounts
// and the keyboard layout at the bottom. On the first boot the bar grows
// into a short form that makes the first account (an administrator).
// The `greeter` object is the link to wynlogin (apps/zerp2/greeter.h).
import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: "#11111b"
    title: "Login"

    // palette (Catppuccin Mocha) and type
    readonly property color base: "#1e1e2e"
    readonly property color mantle: "#181825"
    readonly property color surface: "#313244"
    readonly property color overlay: "#6c7086"
    readonly property color text: "#cdd6f4"
    readonly property color subtext: "#a6adc8"
    readonly property color accent: "#89b4fa"
    readonly property color red: "#f38ba8"
    readonly property string mono: "JetBrains Mono"

    readonly property var users: greeter.users
    property int current: 0
    readonly property var user: users.length > 0 ? users[Math.min(current, users.length - 1)] : null
    property string status: greeter.firstBoot ? "new system: create the first account" : "locked"
    property bool failed: false

    // ---- the wallpaper, a little blurred and dimmed
    Item {
        id: wall
        anchors.fill: parent
        visible: false
        Rectangle { anchors.fill: parent; color: root.mantle }
        Image {
            anchors.fill: parent
            source: wallpaperUrl
            visible: wallpaperUrl !== ""
            fillMode: Image.PreserveAspectCrop
            sourceSize.width: root.width
        }
    }
    MultiEffect {
        anchors.fill: parent
        source: wall
        blurEnabled: true
        blur: 0.35
        blurMax: 32
        brightness: -0.3
        saturation: -0.1
    }

    Item {
        id: content
        anchors.fill: parent
        opacity: 0
        Component.onCompleted: opacity = 1
        Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }

        // ---- the clock
        Column {
            anchors { horizontalCenter: parent.horizontalCenter; bottom: bar.top; bottomMargin: 46 }
            spacing: 4
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Text {
                    id: hm
                    color: root.text
                    font { family: root.mono; pixelSize: 112; weight: Font.Light; letterSpacing: -4 }
                    text: Qt.formatTime(new Date(), "HH:mm")
                }
                Text {
                    id: sec
                    anchors.baseline: hm.baseline
                    color: root.accent
                    font { family: root.mono; pixelSize: 30 }
                    text: Qt.formatTime(new Date(), "ss")
                }
            }
            Text {
                id: day
                anchors.horizontalCenter: parent.horizontalCenter
                color: root.subtext
                font { family: root.mono; pixelSize: 15 }
                text: Qt.formatDate(new Date(), "dddd, d MMMM").toLowerCase()
            }
            Timer {
                interval: 1000; repeat: true; running: true
                onTriggered: {
                    const d = new Date();
                    hm.text = Qt.formatTime(d, "HH:mm");
                    sec.text = Qt.formatTime(d, "ss");
                    day.text = Qt.formatDate(d, "dddd, d MMMM").toLowerCase();
                }
            }
        }

        // ---- the input bar: [initial] [name] [password ...] [go]
        Rectangle {
            id: bar
            anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter; verticalCenterOffset: 70 }
            width: 520
            height: form.implicitHeight + 16
            radius: 12
            color: root.base
            opacity: 0.94

            SequentialAnimation {
                id: shake
                NumberAnimation { target: bar; property: "anchors.horizontalCenterOffset"; to: -10; duration: 45 }
                NumberAnimation { target: bar; property: "anchors.horizontalCenterOffset"; to: 10; duration: 65 }
                NumberAnimation { target: bar; property: "anchors.horizontalCenterOffset"; to: -5; duration: 65 }
                NumberAnimation { target: bar; property: "anchors.horizontalCenterOffset"; to: 0; duration: 55 }
            }

            Column {
                id: form
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                spacing: 6

                // login: one row
                Row {
                    visible: !greeter.firstBoot
                    spacing: 8
                    Rectangle {
                        width: 44; height: 44; radius: 8
                        color: root.failed ? root.red : root.accent
                        Behavior on color { ColorAnimation { duration: 200 } }
                        Text {
                            anchors.centerIn: parent
                            color: root.mantle
                            font { family: root.mono; pixelSize: 20; bold: true }
                            text: root.user ? root.user.name[0].toUpperCase() : "?"
                        }
                    }
                    Rectangle {
                        width: nameLabel.implicitWidth + 24; height: 44; radius: 8
                        color: root.surface
                        Text {
                            id: nameLabel
                            anchors.centerIn: parent
                            color: root.text
                            font { family: root.mono; pixelSize: 15; bold: true }
                            text: root.user ? root.user.name : "no accounts"
                        }
                    }
                    Field {
                        id: passField
                        width: 520 - 16 - 44 - 8 - (nameLabel.implicitWidth + 24) - 8 - 44 - 8
                        secret: true
                        placeholder: "password"
                        onAccepted: root.submit()
                    }
                    GoButton { }
                }

                // first boot: a short form
                Repeater {
                    model: greeter.firstBoot ? 4 : 0
                    delegate: Row {
                        required property int index
                        spacing: 8
                        Rectangle {
                            width: 88; height: 40; radius: 8
                            color: root.surface
                            Text {
                                anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                color: root.subtext
                                font { family: root.mono; pixelSize: 14 }
                                text: ["user", "name", "pass", "again"][index]
                            }
                        }
                        Field {
                            id: f
                            width: 520 - 16 - 88 - 8
                            height: 40
                            secret: index >= 2
                            placeholder: ["e.g. red39", "full name (optional)", "password", "once more"][index]
                            Component.onCompleted: root.fields[index] = f
                            onAccepted: index < 3 ? root.fields[index + 1].forceActiveFocus() : root.submit()
                        }
                    }
                }
                Row {
                    visible: greeter.firstBoot
                    anchors.right: parent.right
                    spacing: 8
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.overlay
                        font { family: root.mono; pixelSize: 12 }
                        text: "it will be an administrator (ary)"
                    }
                    GoButton { label: "create" }
                }
            }
        }

        // ---- the status line
        Text {
            anchors { horizontalCenter: bar.horizontalCenter; top: bar.bottom; topMargin: 14 }
            color: root.failed ? root.red : root.subtext
            font { family: root.mono; pixelSize: 13 }
            text: greeter.busy ? "checking..." : root.status
        }

        // ---- bottom left: the machine; bottom right: accounts and layout
        Row {
            anchors { left: parent.left; bottom: parent.bottom; margins: 18 }
            spacing: 8
            Pill { label: "wynland" }
            Pill { label: "wynlandos"; dim: true }
        }
        Row {
            anchors { right: parent.right; bottom: parent.bottom; margins: 18 }
            spacing: 8
            Repeater {
                model: greeter.firstBoot ? [] : root.users
                delegate: Pill {
                    required property var modelData
                    required property int index
                    label: modelData.name
                    active: index === root.current
                    onClicked: { root.current = index; root.failed = false; root.status = "locked"; passField.clear(); passField.forceActiveFocus() }
                }
            }
            Pill { label: "EN" }
        }
    }

    property var fields: [null, null, null, null]

    function submit() {
        if (greeter.busy) return;
        failed = false;
        if (greeter.firstBoot) {
            const name = fields[0] ? fields[0].text : "";
            const full = fields[1] ? fields[1].text : "";
            const pw = fields[2] ? fields[2].text : "";
            const again = fields[3] ? fields[3].text : "";
            if (!name.length) { refuse("choose a user name"); return; }
            if (!pw.length) { refuse("choose a password"); return; }
            if (pw !== again) { refuse("the passwords differ"); return; }
            status = "creating " + name + "...";
            greeter.createAccount(name, full, pw);
        } else if (root.user) {
            greeter.login(root.user.name, passField.text);
        }
    }
    function refuse(why) { failed = true; status = why; shake.start() }

    Component.onCompleted: {
        if (greeter.firstBoot) Qt.callLater(function () { if (fields[0]) fields[0].forceActiveFocus() });
        else passField.forceActiveFocus();
    }

    Connections {
        target: greeter
        function onAccepted() { root.status = "welcome"; content.opacity = 0; fadeOut.start() }
        function onRefused(reason) {
            root.refuse(reason.toLowerCase());
            passField.clear();
            for (let i = 2; i < 4; i++) if (root.fields[i]) root.fields[i].clear();
            (greeter.firstBoot ? root.fields[2] : passField).forceActiveFocus();
        }
    }
    Timer { id: fadeOut; interval: 380; onTriggered: greeter.quit() }

    // ---- pieces
    component Field: Rectangle {
        id: f
        property alias text: input.text
        property string placeholder
        property bool secret: false
        signal accepted()
        function clear() { input.text = "" }
        function forceActiveFocus() { input.forceActiveFocus() }
        height: 44
        radius: 8
        color: root.mantle
        border.color: input.activeFocus ? root.accent : "transparent"
        border.width: 1
        TextInput {
            id: input
            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
            verticalAlignment: TextInput.AlignVCenter
            color: root.text
            font { family: root.mono; pixelSize: 15 }
            echoMode: f.secret ? TextInput.Password : TextInput.Normal
            passwordCharacter: "•"
            clip: true
            onAccepted: f.accepted()
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: f.placeholder
                color: root.overlay
                font: input.font
                visible: !input.text.length
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.IBeamCursor; onClicked: input.forceActiveFocus() }
    }

    component GoButton: Rectangle {
        property string label: "->"
        width: label === "->" ? 44 : goText.implicitWidth + 28
        height: 44
        radius: 8
        color: goArea.containsMouse ? Qt.lighter(root.accent, 1.1) : root.accent
        opacity: greeter.busy ? 0.5 : 1
        Text {
            id: goText
            anchors.centerIn: parent
            color: root.mantle
            font { family: root.mono; pixelSize: 16; bold: true }
            text: parent.label
        }
        MouseArea { id: goArea; anchors.fill: parent; hoverEnabled: true; onClicked: root.submit() }
    }

    component Pill: Rectangle {
        property string label
        property bool active: false
        property bool dim: false
        signal clicked()
        height: 30
        width: pillText.implicitWidth + 24
        radius: 8
        color: active ? root.accent : root.base
        opacity: 0.94
        Text {
            id: pillText
            anchors.centerIn: parent
            color: parent.active ? root.mantle : (parent.dim ? root.overlay : root.text)
            font { family: root.mono; pixelSize: 13; bold: parent.active }
            text: parent.label
        }
        MouseArea { anchors.fill: parent; onClicked: parent.clicked() }
    }
}
