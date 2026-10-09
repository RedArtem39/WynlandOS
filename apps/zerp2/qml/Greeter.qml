// Zerp 2.0's login screen (zerp2 --greeter, run by wynlogin): the
// wallpaper as it is, the time and date at the top, and at the bottom one
// thin field -- the account's name, then its password. Nothing else. On
// the first boot the same line asks for a name and a password twice: the
// first account, an administrator. The `greeter` object is the link to
// wynlogin (apps/zerp2/greeter.h).
import QtQuick
import QtQuick.Window

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: "#07090c"
    title: "Login"

    readonly property color text: "#d6dbe4"
    readonly property color dim: "#7b8494"
    readonly property color line: "#3a414d"
    readonly property color accent: "#3d7eff"
    readonly property color error: "#e0707a"
    readonly property string mono: "JetBrains Mono"

    readonly property var users: greeter.users
    property int current: 0
    readonly property var user: users.length > 0 ? users[Math.min(current, users.length - 1)] : null
    property string message: ""
    property int step: 0          // first boot: 0 name, 1 password, 2 again

    Image {
        anchors.fill: parent
        source: wallpaperUrl
        visible: wallpaperUrl !== ""
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: root.width
    }

    Item {
        id: content
        anchors.fill: parent
        opacity: 0
        Component.onCompleted: opacity = 1
        Behavior on opacity { NumberAnimation { duration: 300 } }

        // ---- the time
        Column {
            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: root.height * 0.13 }
            spacing: 6
            Text {
                id: clock
                anchors.horizontalCenter: parent.horizontalCenter
                color: root.text
                font { family: root.mono; pixelSize: 96; weight: Font.Light }
                text: Qt.formatTime(new Date(), "HH:mm")
            }
            Text {
                id: date
                anchors.horizontalCenter: parent.horizontalCenter
                color: root.dim
                font { family: root.mono; pixelSize: 16 }
                text: Qt.formatDate(new Date(), "dddd d MMMM").toLowerCase()
            }
            Timer {
                interval: 1000; repeat: true; running: true
                onTriggered: {
                    clock.text = Qt.formatTime(new Date(), "HH:mm");
                    date.text = Qt.formatDate(new Date(), "dddd d MMMM").toLowerCase();
                }
            }
        }

        // ---- the line: name, then the field
        Item {
            id: line
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: root.height * 0.14 }
            width: 440
            height: 40

            SequentialAnimation {
                id: shake
                NumberAnimation { target: line; property: "anchors.horizontalCenterOffset"; to: -8; duration: 45 }
                NumberAnimation { target: line; property: "anchors.horizontalCenterOffset"; to: 8; duration: 60 }
                NumberAnimation { target: line; property: "anchors.horizontalCenterOffset"; to: 0; duration: 50 }
            }

            Text {
                id: who
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                color: root.text
                font { family: root.mono; pixelSize: 17; bold: true }
                text: greeter.firstBoot ? ["new user", "password", "again"][root.step]
                                        : (root.user ? root.user.name : "")
            }
            TextInput {
                id: input
                anchors { left: who.right; leftMargin: 18; right: parent.right; verticalCenter: parent.verticalCenter }
                color: root.text
                font { family: root.mono; pixelSize: 17 }
                echoMode: (greeter.firstBoot && root.step === 0) ? TextInput.Normal : TextInput.Password
                passwordCharacter: "•"
                selectByMouse: true
                clip: true
                focus: true
                onAccepted: root.submit()
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.dim
                    font: input.font
                    visible: !input.text.length
                    text: greeter.busy ? "..." : (greeter.firstBoot && root.step === 0 ? "name" : "password")
                }
            }
            Rectangle {   // the underline
                anchors { left: input.left; right: input.right; top: parent.bottom }
                height: 1
                color: input.activeFocus ? root.accent : root.line
            }
        }

        Text {
            anchors { horizontalCenter: line.horizontalCenter; top: line.bottom; topMargin: 16 }
            color: root.message.length ? root.error : root.dim
            font { family: root.mono; pixelSize: 13 }
            text: root.message.length ? root.message
                : greeter.firstBoot ? "first start: make your account (it will be an administrator)"
                : root.users.length > 1 ? "tab: another account" : ""
        }
    }

    // first boot: what was typed so far
    property string newName: ""
    property string newPass: ""

    function submit() {
        if (greeter.busy) return;
        message = "";
        if (!greeter.firstBoot) {
            if (root.user) greeter.login(root.user.name, input.text);
            return;
        }
        if (step === 0) {
            if (!input.text.length) return;
            newName = input.text; step = 1; input.text = "";
        } else if (step === 1) {
            if (!input.text.length) return;
            newPass = input.text; step = 2; input.text = "";
        } else {
            if (input.text !== newPass) { refuse("the passwords differ"); step = 1; newPass = ""; return; }
            greeter.createAccount(newName, "", newPass);
        }
    }
    function refuse(why) { message = why; input.text = ""; shake.start(); input.forceActiveFocus() }

    // Tab: the next account
    Shortcut {
        sequence: "Tab"
        enabled: !greeter.firstBoot && root.users.length > 1
        onActivated: { root.current = (root.current + 1) % root.users.length; root.message = ""; input.text = "" }
    }

    Component.onCompleted: input.forceActiveFocus()

    Connections {
        target: greeter
        function onAccepted() { content.opacity = 0; fadeOut.start() }
        function onRefused(reason) {
            if (greeter.firstBoot) { root.step = 0; root.newName = ""; root.newPass = "" }
            root.refuse(reason.toLowerCase());
        }
    }
    Timer { id: fadeOut; interval: 320; onTriggered: greeter.quit() }
}
