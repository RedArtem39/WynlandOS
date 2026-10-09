// Zerp 2.0's login screen (zerp2 --greeter, run by wynlogin): the
// wallpaper through a blur, the clock, and a frosted card with the user,
// a password and a go button. On the first boot -- no account yet -- the
// card makes the first one (an administrator). The `greeter` object is
// the link to wynlogin (apps/zerp2/greeter.h); a good password fades the
// screen out and the session starts.
import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: "#05070c"
    title: "Login"

    readonly property color accent: "#3d8bff"
    readonly property color accent2: "#7cc4ff"
    readonly property var users: greeter.users
    property int current: 0
    readonly property var user: users.length > 0 ? users[Math.min(current, users.length - 1)] : null
    property string message: ""

    // ---- backdrop: the wallpaper, blurred and dimmed
    Item {
        id: wall
        anchors.fill: parent
        visible: false
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#14204a" }
                GradientStop { position: 1.0; color: "#06101e" }
            }
        }
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
        blur: 1.0
        blurMax: 64
        saturation: 0.15
        brightness: -0.18
    }

    Item {
        id: content
        anchors.fill: parent
        opacity: 0
        scale: 0.985
        Component.onCompleted: { opacity = 1; scale = 1 }
        Behavior on opacity { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }

        // ---- the clock
        Column {
            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: root.height * 0.12 }
            spacing: 2
            Text {
                id: clock
                anchors.horizontalCenter: parent.horizontalCenter
                color: "white"
                font { pixelSize: Math.round(root.height * 0.11); weight: Font.Light }
                text: Qt.formatTime(new Date(), "HH:mm")
            }
            Text {
                id: date
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#d6def0"
                opacity: 0.85
                font.pixelSize: Math.round(root.height * 0.022)
                text: Qt.formatDate(new Date(), "dddd, d MMMM")
            }
            Timer {
                interval: 1000; repeat: true; running: true
                onTriggered: { clock.text = Qt.formatTime(new Date(), "HH:mm"); date.text = Qt.formatDate(new Date(), "dddd, d MMMM") }
            }
        }

        // ---- the card
        Item {
            id: card
            width: 360
            height: col.implicitHeight + 56
            anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter; verticalCenterOffset: root.height * 0.08 }

            Rectangle {
                anchors.fill: parent
                radius: 22
                color: "#121826"
                opacity: 0.55
                border.color: "#8099b4ff"
                border.width: 1
            }
            Rectangle {   // the border's glow is the faint part
                anchors.fill: parent
                radius: 22
                color: "transparent"
                border.color: "#20ffffff"
                border.width: 1
            }

            // a wrong password: a short shake
            SequentialAnimation {
                id: shake
                loops: 1
                NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: -12; duration: 50 }
                NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: 12; duration: 70 }
                NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: -7; duration: 70 }
                NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: 0; duration: 60 }
            }

            Column {
                id: col
                anchors { top: parent.top; topMargin: 28; horizontalCenter: parent.horizontalCenter }
                width: parent.width - 56
                spacing: 14

                // the avatar: the account's initial on the brand gradient
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 84; height: 84; radius: 42
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: root.accent2 }
                        GradientStop { position: 1.0; color: root.accent }
                    }
                    Text {
                        anchors.centerIn: parent
                        color: "white"
                        font { pixelSize: 36; bold: true }
                        text: greeter.firstBoot ? (nameField.text.length ? nameField.text[0].toUpperCase() : "+")
                                                : (root.user ? root.user.fullName[0].toUpperCase() : "?")
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: "white"
                    font { pixelSize: 20; bold: true }
                    text: greeter.firstBoot ? "Welcome to WynlandOS" : (root.user ? root.user.fullName : "No accounts")
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: "#aab6cf"
                    font.pixelSize: 13
                    visible: greeter.firstBoot
                    text: "Create your account. It will be the administrator: 'ary' runs commands as root with its password."
                }

                // first boot: a name and a full name
                Field { id: nameField; visible: greeter.firstBoot; placeholder: "user name (e.g. red39)"; focus: greeter.firstBoot
                        onAccepted: fullField.forceActiveFocus() }
                Field { id: fullField; visible: greeter.firstBoot; placeholder: "full name (optional)"
                        onAccepted: passField.forceActiveFocus() }

                Field {
                    id: passField
                    secret: true
                    placeholder: "password"
                    focus: !greeter.firstBoot
                    onAccepted: greeter.firstBoot ? againField.forceActiveFocus() : root.submit()
                }
                Field { id: againField; visible: greeter.firstBoot; secret: true; placeholder: "password again"
                        onAccepted: root.submit() }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: "#ff8a80"
                    font.pixelSize: 13
                    text: root.message
                    visible: text.length > 0
                }

                // go
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width; height: 42; radius: 12
                    color: goArea.containsMouse ? Qt.lighter(root.accent, 1.12) : root.accent
                    opacity: greeter.busy ? 0.6 : 1
                    Text {
                        anchors.centerIn: parent
                        color: "white"
                        font { pixelSize: 15; bold: true }
                        text: greeter.busy ? "…" : (greeter.firstBoot ? "Create account" : "Log in")
                    }
                    MouseArea { id: goArea; anchors.fill: parent; hoverEnabled: true; onClicked: root.submit() }
                }

                // other accounts
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 10
                    visible: !greeter.firstBoot && root.users.length > 1
                    Repeater {
                        model: root.users
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: 34; height: 34; radius: 17
                            color: index === root.current ? root.accent : "#2a3246"
                            border.color: "#40ffffff"
                            Text { anchors.centerIn: parent; color: "white"; font.bold: true; text: modelData.fullName[0].toUpperCase() }
                            MouseArea { anchors.fill: parent; onClicked: { root.current = index; root.message = ""; passField.clear(); passField.forceActiveFocus() } }
                        }
                    }
                }
            }
        }

        Text {
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 22 }
            color: "#8090b0"
            font.pixelSize: 12
            text: "WynlandOS"
        }
    }

    function submit() {
        if (greeter.busy) return;
        message = "";
        if (greeter.firstBoot) {
            if (!nameField.text.length) { message = "Choose a user name"; shake.start(); return; }
            if (passField.text.length < 1) { message = "Choose a password"; shake.start(); return; }
            if (passField.text !== againField.text) { message = "The passwords differ"; shake.start(); return; }
            greeter.createAccount(nameField.text, fullField.text, passField.text);
        } else if (root.user) {
            greeter.login(root.user.name, passField.text);
        }
    }

    Component.onCompleted: (greeter.firstBoot ? nameField : passField).forceActiveFocus()

    Connections {
        target: greeter
        function onAccepted() { content.opacity = 0; content.scale = 1.02; fadeOut.start() }
        function onRefused(reason) { root.message = reason; passField.clear(); againField.clear(); shake.start(); passField.forceActiveFocus() }
    }
    Timer { id: fadeOut; interval: 480; onTriggered: greeter.quit() }

    // a text field on the card
    component Field: Rectangle {
        id: f
        property alias text: input.text
        property string placeholder
        property bool secret: false
        signal accepted()
        function clear() { input.text = "" }
        function forceActiveFocus() { input.forceActiveFocus() }
        focus: false
        onFocusChanged: if (focus) input.forceActiveFocus()
        width: parent ? parent.width : 300
        height: 42
        radius: 12
        color: "#0c111d"
        opacity: 0.9
        border.color: input.activeFocus ? root.accent : "#33ffffff"
        border.width: input.activeFocus ? 2 : 1
        Behavior on border.color { ColorAnimation { duration: 150 } }
        TextInput {
            id: input
            anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
            verticalAlignment: TextInput.AlignVCenter
            color: "white"
            font.pixelSize: 15
            echoMode: f.secret ? TextInput.Password : TextInput.Normal
            passwordCharacter: "●"
            clip: true
            selectByMouse: true
            onAccepted: f.accepted()
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: f.placeholder
                color: "#6f7d99"
                font.pixelSize: 15
                visible: !input.text.length
            }
        }
        MouseArea { anchors.fill: parent; onClicked: input.forceActiveFocus(); cursorShape: Qt.IBeamCursor }
    }
}
