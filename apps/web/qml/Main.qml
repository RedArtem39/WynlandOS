// WynlandOS - Web.
//
// A toolbar (back, forward, reload/stop, the address field) over one page.
// The address field takes a URL, a host name or words to search for.
//
// Keys: Ctrl+L the address, Enter go, Esc back to the page, F5 / Ctrl+R
// reload, Alt+Left / Alt+Right back and forward.
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import Wynland.Web

Window {
    id: win
    visible: true
    width: Screen.width > 0 ? Screen.width : 1280
    height: Screen.height > 0 ? Screen.height : 800
    color: pal.bg
    title: web.title !== "" ? web.title : "Web"

    QtObject {
        id: pal
        readonly property color bg: "#11141c"
        readonly property color panel: "#161a24"
        readonly property color field: "#1b2030"
        readonly property color fieldFocus: "#222838"
        readonly property color line: "#232838"
        readonly property color text: "#e6e8ef"
        readonly property color dim: "#8a91a8"
        readonly property color faint: "#5a6178"
        readonly property color accent: "#7aa2f7"
        readonly property color hover: "#1f2433"
        readonly property color danger: "#ff7b72"
        readonly property color ok: "#7ee787"
    }

    // ------------------------------------------------------------ toolbar
    Rectangle {
        id: bar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 44
        color: pal.panel

        Row {
            id: nav
            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
            spacing: 2
            ToolGlyph { glyph: "‹"; enabled: web.canGoBack; onClicked: web.goBack() }
            ToolGlyph { glyph: "›"; enabled: web.canGoForward; onClicked: web.goForward() }
            ToolGlyph { glyph: web.loading ? "×" : "↻"; onClicked: web.loading ? web.stop() : web.reload() }
        }

        Rectangle {
            id: addr
            anchors { left: nav.right; leftMargin: 8; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            height: 30
            radius: 8
            color: field.activeFocus ? pal.fieldFocus : pal.field
            border.color: field.activeFocus ? pal.accent : "transparent"
            border.width: 1

            Text {
                id: lock
                anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                visible: !field.activeFocus && web.url !== ""
                text: web.secure ? "●" : "○"
                color: web.secure ? pal.ok : pal.faint
                font.pixelSize: 9
            }

            TextInput {
                id: field
                anchors { left: lock.visible ? lock.right : parent.left; leftMargin: 8; right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                color: pal.text
                selectionColor: "#2d4a80"
                selectedTextColor: pal.text
                font.pixelSize: 14
                clip: true
                selectByMouse: true
                text: web.url
                onAccepted: { web.load(text); }
                onActiveFocusChanged: if (activeFocus) selectAll(); else text = Qt.binding(() => web.url)
                Keys.onEscapePressed: { text = web.url; web.forceActiveFocus() }

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: field.text === "" && !field.activeFocus
                    text: "Search or type an address"
                    color: pal.faint
                    font.pixelSize: 14
                }
            }
        }

        // load progress: a thin line along the bottom of the toolbar
        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom }
            height: 2
            width: parent.width * web.progress
            visible: web.loading
            color: pal.accent
        }
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: pal.line; z: -1 }
    }

    // ------------------------------------------------------------ page
    WebView {
        id: web
        anchors { left: parent.left; right: parent.right; top: bar.bottom; bottom: parent.bottom }
        focus: true
        Component.onCompleted: load(startUrl)
    }

    // a failed load, over the page
    Rectangle {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 24 }
        visible: web.error !== ""
        width: Math.min(errText.implicitWidth + 32, parent.width - 48)
        height: errText.implicitHeight + 20
        radius: 8
        color: "#2a1a1d"
        border.color: pal.danger
        Text {
            id: errText
            anchors.centerIn: parent
            width: parent.width - 32
            text: web.error
            color: pal.text
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }
    }

    // ------------------------------------------------------------ keys
    Shortcut { sequence: "Ctrl+L"; onActivated: field.forceActiveFocus() }
    Shortcut { sequences: ["F5", "Ctrl+R"]; onActivated: web.reload() }
    Shortcut { sequence: "Alt+Left"; onActivated: web.goBack() }
    Shortcut { sequence: "Alt+Right"; onActivated: web.goForward() }

    component ToolGlyph: Rectangle {
        id: tg
        property string glyph
        signal clicked()
        width: 32; height: 30; radius: 8
        color: ma.containsMouse && tg.enabled ? pal.hover : "transparent"
        opacity: tg.enabled ? 1 : 0.35
        Text {
            anchors.centerIn: parent
            text: tg.glyph
            color: pal.text
            font.pixelSize: tg.glyph === "‹" || tg.glyph === "›" ? 24 : 17
        }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; enabled: tg.enabled; onClicked: tg.clicked() }
    }
}
