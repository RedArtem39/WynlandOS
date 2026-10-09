// Zerp 2.0's login screen (zerp2 --greeter, run by wynlogin), laid out
// after Serpantinum's lock screen (github.com/ilyamiro/serpantinum -- the
// look only, this is our own code): at rest, the blurred wallpaper and a
// large clock; the first key or click wakes it -- the clock steps away,
// the blur deepens, and the panel rises: the avatar, the account and
// status pills, the password field, the keyboard layout. On the first
// boot the panel makes the first account (an administrator) instead.
// The `greeter` object is the link to wynlogin (apps/zerp2/greeter.h).
import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: root
    visible: true
    visibility: Window.FullScreen
    color: crust
    title: "Login"

    // ---- theme: WynlandOS black and the logo's blue in Serpantinum's roles
    readonly property color crust: "#08090c"
    readonly property color surface0: "#14171d"
    readonly property color surface1: "#232832"
    readonly property color text: "#dfe5ee"
    readonly property color subtext: "#8b95a6"
    readonly property color accent: "#3d7eff"
    readonly property color busy: "#6fb3ff"
    readonly property color red: "#e0707a"
    readonly property string font: "Adwaita Mono"
    readonly property int radius: 8
    function s(v) { return v * root.height / 1080 }

    readonly property var users: greeter.users
    property int current: 0
    readonly property var user: users.length > 0 ? users[Math.min(current, users.length - 1)] : null

    property bool active: greeter.firstBoot          // the panel is up
    property bool failed: false
    property string status: greeter.firstBoot ? "new system" : "locked"

    // ---- the wallpaper: blurred, more when the panel is up
    Item {
        id: wall
        anchors.fill: parent
        visible: false
        Rectangle { anchors.fill: parent; color: root.crust }
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
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: root.s(64)
        saturation: 0.25
        contrast: 0.06
        brightness: 0.02
        blur: root.active ? 1.0 : 0.72
        Behavior on blur { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }
    }
    Rectangle {
        anchors.fill: parent
        color: root.crust
        opacity: root.active ? 0.14 : 0.38
        Behavior on opacity { NumberAnimation { duration: 600; easing.type: Easing.OutCubic } }
    }

    // a click wakes it (keys: below)
    MouseArea { anchors.fill: parent; onClicked: root.wake() }

    // ---- the clock, at rest
    Column {
        id: clock
        anchors { centerIn: parent; verticalCenterOffset: root.active ? root.s(-280) : root.s(-130) }
        spacing: root.s(8)
        opacity: root.active ? 0 : 1
        scale: root.active ? 0.92 : 1
        visible: opacity > 0.01
        Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        property date now: new Date()
        Timer { interval: 1000; repeat: true; running: true; onTriggered: clock.now = new Date() }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: root.s(4)
            Shadowed { text: Qt.formatTime(clock.now, "HH"); size: root.s(120) }
            Shadowed { text: ":"; size: root.s(80); weight: Font.Light; opacity: 0.75; anchors.verticalCenter: parent.verticalCenter }
            Shadowed { text: Qt.formatTime(clock.now, "mm"); size: root.s(120) }
        }
        Shadowed {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatDate(clock.now, "dddd, d MMMM").toUpperCase()
            size: root.s(14)
            weight: Font.Bold
            spacing: 1.4
            opacity: 0.85
        }
    }

    // ---- the panel
    Rectangle {   // its shadow
        anchors.fill: panel
        anchors.topMargin: root.s(1.5)
        anchors.bottomMargin: -root.s(1.5)
        radius: panel.radius
        color: Qt.rgba(0, 0, 0, 0.14)
        opacity: panel.opacity
        scale: panel.scale
    }
    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: root.s(440)
        height: root.s(580)
        radius: root.radius * 1.5
        color: root.surface0
        border.width: 1.5
        border.color: root.surface1
        opacity: root.active ? 1 : 0
        scale: root.active ? 1 : 0.92
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack; easing.overshoot: 1.2 } }

        Column {
            // centred in the panel, as Serpantinum's (spacers above and below)
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: root.s(24) }
            spacing: root.s(14)

            // the avatar: the account's initial on a circle
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.s(214); height: width; radius: width / 2
                color: root.surface1
                Text {
                    anchors.centerIn: parent
                    color: root.text
                    font { family: root.font; pixelSize: root.s(96) }
                    text: greeter.firstBoot ? (nameField.text.length ? nameField.text[0].toUpperCase() : "+")
                                            : (root.user ? root.user.name[0].toUpperCase() : "?")
                }
            }

            Item { width: 1; height: root.s(10) }

            // the account and the status
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.s(10)
                Pill {
                    icon: "user"
                    label: greeter.firstBoot ? (nameField.text.length ? nameField.text : "new account")
                                             : (root.user ? root.user.name : "nobody")
                    onClicked: root.nextUser()
                }
                Pill {
                    icon: "lock"
                    label: greeter.busy ? "checking" : root.status
                    fg: root.failed ? root.red : (greeter.busy ? root.busy : root.text)
                }
            }

            // the fields: the password (with the lock); on the first boot
            // the name (a person) above two password fields
            Item {
                id: fields
                width: parent.width
                height: col.implicitHeight
                SequentialAnimation {
                    id: shake
                    NumberAnimation { target: col; property: "x"; to: -root.s(10); duration: 45 }
                    NumberAnimation { target: col; property: "x"; to: root.s(10); duration: 65 }
                    NumberAnimation { target: col; property: "x"; to: -root.s(5); duration: 60 }
                    NumberAnimation { target: col; property: "x"; to: 0; duration: 50 }
                }
                Column {
                    id: col
                    width: parent.width
                    spacing: root.s(8)
                    Field {
                        id: nameField
                        visible: greeter.firstBoot
                        icon: "user"
                        secret: false
                        placeholder: "your name"
                        onAccepted: input.forceActiveFocus()
                    }
                    Field {
                        id: input
                        icon: "lock"
                        placeholder: greeter.firstBoot ? "a password" : "enter password"
                        onAccepted: greeter.firstBoot ? againField.forceActiveFocus() : root.submit()
                    }
                    Field {
                        id: againField
                        visible: greeter.firstBoot
                        icon: "lock"
                        placeholder: "the password again"
                        onAccepted: root.submit()
                    }
                }
            }

            // the keyboard layout
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.s(10)
                Pill { icon: "keyboard"; label: "US"; small: true }
            }
        }
    }

    // ---- keys: the first one wakes the screen and is typed in
    Item {
        id: keys
        focus: true
        Keys.onPressed: (e) => {
            if (root.active) return;
            root.wake();
            if (e.text.length && e.key !== Qt.Key_Return && e.key !== Qt.Key_Enter && e.key !== Qt.Key_Escape)
                input.append(e.text);
            e.accepted = true;
        }
    }
    // back to rest after a while without typing
    Timer {
        interval: 15000
        running: root.active && !greeter.firstBoot && !input.text.length
        onTriggered: { root.active = false; keys.forceActiveFocus() }
    }

    function wake() {
        root.active = true;
        input.forceActiveFocus();
    }
    function nextUser() {
        if (greeter.firstBoot || root.users.length < 2) return;
        root.current = (root.current + 1) % root.users.length;
        input.clear();
        root.failed = false;
        root.status = "locked";
    }
    function submit() {
        if (greeter.busy) return;
        root.failed = false;
        if (!greeter.firstBoot) {
            if (root.user && input.text.length) greeter.login(root.user.name, input.text);
            return;
        }
        if (!nameField.text.length) { root.refuse("choose a name"); nameField.forceActiveFocus(); return }
        if (!input.text.length) { root.refuse("choose a password"); return }
        if (input.text !== againField.text) { againField.clear(); root.refuse("passwords differ"); return }
        root.status = "creating";
        greeter.createAccount(nameField.text, "", input.text);
    }
    function refuse(why) {
        root.failed = true;
        root.status = why;
        input.clear();
        againField.clear();
        shake.start();
        input.forceActiveFocus();
    }

    Component.onCompleted: greeter.firstBoot ? nameField.forceActiveFocus() : keys.forceActiveFocus()

    Connections {
        target: greeter
        function onAccepted() { root.status = "welcome"; leave.start() }
        function onRefused(reason) {
            root.refuse(greeter.firstBoot ? reason.toLowerCase() : "access denied");
        }
    }
    // unlocked: the panel folds away, then the session takes over
    ParallelAnimation {
        id: leave
        NumberAnimation { target: panel; property: "scale"; to: 0; duration: 220; easing.type: Easing.InBack; easing.overshoot: 1.3 }
        NumberAnimation { target: panel; property: "opacity"; to: 0; duration: 250; easing.type: Easing.InQuad }
        onFinished: greeter.quit()
    }

    // ---- pieces
    component Shadowed: Text {
        property real size: 14
        property int weight: Font.Normal
        property real spacing: 0
        color: "#ffffff"
        font { family: root.font; pixelSize: size; weight: weight; letterSpacing: spacing }
        Text {   // a soft shadow 1.5 px below
            anchors.fill: parent
            anchors.topMargin: root.s(1.5)
            anchors.bottomMargin: -root.s(1.5)
            z: -1
            color: Qt.rgba(0, 0, 0, 0.16)
            font: parent.font
            text: parent.text
        }
    }

    // a field: an icon box at the left end, the text centred
    component Field: Rectangle {
        id: fld
        property string icon: "lock"
        property bool secret: true
        property string placeholder
        property alias text: ti.text
        signal accepted()
        function clear() { ti.text = "" }
        function append(t) { ti.insert(ti.text.length, t) }
        function forceActiveFocus() { ti.forceActiveFocus() }
        width: parent ? parent.width : root.s(392)
        height: root.s(44)
        radius: root.radius
        color: Qt.lighter(root.surface0, 1.28)
        border.width: ti.activeFocus ? 1 : 0
        border.color: root.failed ? root.red : root.accent
        Rectangle {
            id: box
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: root.s(4) }
            width: height
            radius: root.radius - 2
            color: Qt.lighter(root.surface0, 1.55)
            Icon { anchors.centerIn: parent; kind: fld.icon; size: root.s(16); color: root.failed ? root.red : root.text }
        }
        TextInput {
            id: ti
            anchors { left: box.right; right: parent.right; leftMargin: root.s(12); rightMargin: root.s(12) + box.width; verticalCenter: parent.verticalCenter }
            horizontalAlignment: TextInput.AlignHCenter
            color: root.text
            font { family: root.font; pixelSize: root.s(14) }
            echoMode: fld.secret ? TextInput.Password : TextInput.Normal
            passwordCharacter: "●"
            clip: true
            onAccepted: fld.accepted()
            onTextEdited: { if (root.failed) { root.failed = false; root.status = greeter.firstBoot ? "new system" : "locked" } }
            Keys.onEscapePressed: { ti.text = ""; if (!greeter.firstBoot) root.active = false }
            Text {
                anchors.centerIn: parent
                color: root.subtext
                font: ti.font
                visible: !ti.text.length
                text: fld.placeholder
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.IBeamCursor; onClicked: ti.forceActiveFocus() }
    }

    component Pill: Rectangle {
        property string icon: ""
        property string label
        property color fg: root.text
        property bool small: false
        signal clicked()
        height: root.s(small ? 32 : 38)
        width: row.implicitWidth + root.s(small ? 24 : 32)
        radius: root.radius
        color: Qt.lighter(root.surface0, 1.28)
        Row {
            id: row
            anchors.centerIn: parent
            spacing: root.s(8)
            Icon { kind: parent.parent.icon; size: root.s(parent.parent.small ? 14 : 15); color: parent.parent.fg; anchors.verticalCenter: parent.verticalCenter }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: parent.parent.fg
                font { family: root.font; pixelSize: root.s(parent.parent.small ? 12 : 13) }
                text: parent.parent.label
            }
        }
        MouseArea { anchors.fill: parent; onClicked: parent.clicked() }
    }

    // small line icons (no icon font in the image): user, lock, keyboard
    component Icon: Canvas {
        property string kind
        property real size: 16
        property color color: root.text
        width: size; height: size
        onColorChanged: requestPaint()
        onKindChanged: requestPaint()
        onPaint: {
            var c = getContext("2d");
            c.reset();
            var w = width, h = height;
            c.strokeStyle = color; c.fillStyle = color;
            c.lineWidth = Math.max(1.4, w / 10); c.lineCap = "round"; c.lineJoin = "round";
            if (kind === "user") {
                c.beginPath(); c.arc(w / 2, h * 0.32, w * 0.2, 0, Math.PI * 2); c.stroke();
                c.beginPath(); c.arc(w / 2, h * 1.02, w * 0.4, Math.PI * 1.15, Math.PI * 1.85); c.stroke();
            } else if (kind === "lock") {
                c.beginPath(); c.arc(w / 2, h * 0.38, w * 0.22, Math.PI, 0); c.stroke();
                c.beginPath(); c.moveTo(w * 0.28, h * 0.38); c.lineTo(w * 0.28, h * 0.46);
                c.moveTo(w * 0.72, h * 0.38); c.lineTo(w * 0.72, h * 0.46); c.stroke();
                c.fillRect(w * 0.18, h * 0.46, w * 0.64, h * 0.44);
            } else if (kind === "keyboard") {
                c.strokeRect(w * 0.08, h * 0.25, w * 0.84, h * 0.5);
                for (var i = 0; i < 4; i++) c.fillRect(w * (0.2 + i * 0.17), h * 0.36, w * 0.08, h * 0.08);
                c.fillRect(w * 0.3, h * 0.56, w * 0.4, h * 0.07);
            }
        }
    }
}
