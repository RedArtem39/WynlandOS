// WynlandOS - Web.
//
// Tabs down a sidebar on the left on Zerp's live frosted glass (the window
// is see-through there, tinted), the page as a rounded card on the right. The address
// field takes a URL, a host name or words to search Google for.
//
// Keys: Ctrl+T new tab, Ctrl+W close it, Ctrl+Tab / Ctrl+Shift+Tab next /
// previous, Ctrl+1..9 a tab, Ctrl+L the address, Enter go, Esc back to the
// page, F5 / Ctrl+R reload, Alt+Left / Alt+Right back and forward.
import QtQuick
import QtQuick.Window
import Wynland.Web

Window {
    id: win
    visible: true
    width: Screen.width > 0 ? Screen.width : 1280
    height: Screen.height > 0 ? Screen.height : 800
    color: "transparent"
    title: current && current.title !== "" ? current.title : "Web"

    QtObject {
        id: pal
        readonly property color bg: "#0f121a"
        readonly property color text: "#e8eaf1"
        readonly property color dim: "#9aa1b6"
        readonly property color faint: "#646b82"
        readonly property color accent: "#7aa2f7"
        readonly property color glass: "#ffffff"      // hover/selection washes, at low opacity
        readonly property color field: "#000000"
        readonly property color danger: "#ff7b72"
        readonly property color ok: "#7ee787"
    }

    readonly property int sideWidth: 252
    readonly property string homeUrl: "https://www.google.com/"

    // ------------------------------------------------------------ tabs
    ListModel { id: tabs }
    property int currentIndex: 0
    readonly property var current: pages.count > currentIndex ? pages.itemAt(currentIndex) : null

    function newTab(url, select) {
        tabs.append({ startUrl: url && url !== "" ? url : homeUrl })
        if (select !== false) currentIndex = tabs.count - 1
        if (!url) Qt.callLater(() => field.forceActiveFocus())
    }
    function closeTab(i) {
        if (tabs.count <= 1) { pages.itemAt(0).load(homeUrl); return }
        tabs.remove(i)
        if (currentIndex >= tabs.count) currentIndex = tabs.count - 1
        else if (i < currentIndex) currentIndex--
    }
    function selectTab(i) { if (i >= 0 && i < tabs.count) currentIndex = i }

    Component.onCompleted: {
        if (startUrls.length === 0) newTab(homeUrl)
        for (let i = 0; i < startUrls.length; i++) newTab(startUrls[i], i === 0)
        currentIndex = 0
    }

    // ------------------------------------------------------------ the glass
    // Zerp's frosted glass shows through; a tint keeps the text readable
    Rectangle { anchors.fill: parent; color: "#0b0e15"; opacity: 0.5 }

    // ------------------------------------------------------------ sidebar
    Item {
        id: side
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: sideWidth

        // navigation
        Row {
            id: nav
            anchors { left: parent.left; leftMargin: 10; top: parent.top; topMargin: 10 }
            spacing: 2
            Glyph { glyph: "‹"; size: 22; enabled: current && current.canGoBack; onClicked: current.goBack() }
            Glyph { glyph: "›"; size: 22; enabled: current && current.canGoForward; onClicked: current.goForward() }
            Glyph { glyph: current && current.loading ? "×" : "↻"; size: 16
                    onClicked: current.loading ? current.stop() : current.reload() }
        }

        // the address
        Rectangle {
            id: addr
            anchors { left: parent.left; right: parent.right; top: nav.bottom; margins: 10; topMargin: 8 }
            height: 34
            radius: 9
            color: pal.field
            opacity: field.activeFocus ? 0.55 : 0.32
            border.color: field.activeFocus ? pal.accent : "transparent"
        }
        Text {
            id: lock
            anchors { left: addr.left; leftMargin: 11; verticalCenter: addr.verticalCenter }
            visible: !field.activeFocus && current && current.url !== ""
            text: current && current.secure ? "●" : "○"
            color: current && current.secure ? pal.ok : pal.faint
            font.pixelSize: 8
        }
        TextInput {
            id: field
            anchors { left: lock.visible ? lock.right : addr.left; leftMargin: lock.visible ? 8 : 12
                      right: addr.right; rightMargin: 10; verticalCenter: addr.verticalCenter }
            color: pal.text
            selectionColor: "#2d4a80"
            selectedTextColor: pal.text
            font.pixelSize: 13
            clip: true
            selectByMouse: true
            // the host when idle (as Zen shows it), the whole URL to edit
            text: activeFocus ? (current ? current.url : "") : hostOf(current ? current.url : "")
            onAccepted: { if (current) current.load(text); current.forceActiveFocus() }
            onActiveFocusChanged: if (activeFocus) { text = current ? current.url : ""; selectAll() }
            Keys.onEscapePressed: current.forceActiveFocus()
            Text {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: field.text === "" && !field.activeFocus
                text: "Search Google or type a URL"
                color: pal.faint
                font.pixelSize: 13
            }
        }

        // the tabs
        ListView {
            id: list
            anchors { left: parent.left; right: parent.right; top: addr.bottom; bottom: newRow.top
                      leftMargin: 8; rightMargin: 8; topMargin: 14 }
            model: tabs
            spacing: 2
            clip: true
            delegate: Item {
                id: row
                required property int index
                readonly property var page: pages.count > index ? pages.itemAt(index) : null
                readonly property bool selected: index === win.currentIndex
                width: ListView.view.width
                height: 34
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: pal.glass
                    opacity: row.selected ? 0.14 : (hover.containsMouse ? 0.07 : 0)
                }
                // a letter badge for the site
                Rectangle {
                    id: badge
                    anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                    width: 18; height: 18; radius: 5
                    color: Qt.hsla((hostOf(row.page ? row.page.url : "").length * 0.13) % 1, 0.45, 0.45, 1)
                    Text {
                        anchors.centerIn: parent
                        text: { const h = hostOf(row.page ? row.page.url : ""); return h ? h.replace(/^www\./, "")[0].toUpperCase() : "·" }
                        color: "white"
                        font { pixelSize: 11; bold: true }
                    }
                }
                Text {
                    anchors { left: badge.right; leftMargin: 9; right: closeBtn.left; rightMargin: 4; verticalCenter: parent.verticalCenter }
                    text: row.page ? (row.page.title !== "" ? row.page.title : (row.page.url !== "" ? hostOf(row.page.url) : "New tab")) : ""
                    color: row.selected ? pal.text : pal.dim
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                // loading: a small dot instead of the close button's place
                Rectangle {
                    anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                    width: 5; height: 5; radius: 3
                    color: pal.accent
                    visible: row.page && row.page.loading && !hover.containsMouse
                }
                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onClicked: (m) => { if (m.button === Qt.MiddleButton) closeTab(row.index); else selectTab(row.index) }
                }
                Glyph {
                    id: closeBtn
                    anchors { right: parent.right; rightMargin: 3; verticalCenter: parent.verticalCenter }
                    width: 26; height: 26
                    glyph: "×"; size: 15
                    visible: hover.containsMouse || hovered
                    onClicked: closeTab(row.index)
                }
            }
        }

        Item {
            id: newRow
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8 }
            height: 34
            Rectangle { anchors.fill: parent; radius: 8; color: pal.glass; opacity: newHover.containsMouse ? 0.07 : 0 }
            Text {
                anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                text: "+   New tab"
                color: pal.dim
                font.pixelSize: 13
            }
            MouseArea { id: newHover; anchors.fill: parent; hoverEnabled: true; onClicked: newTab() }
        }
    }

    // ------------------------------------------------------------ pages
    Item {
        id: card
        anchors { left: side.right; right: parent.right; top: parent.top; bottom: parent.bottom
                  topMargin: 8; rightMargin: 8; bottomMargin: 8 }

        // a soft edge under the card
        Rectangle { anchors.fill: parent; anchors.margins: -1; radius: 11; color: "#000000"; opacity: 0.35 }

        Repeater {
            id: pages
            model: tabs
            delegate: WebView {
                required property int index
                required property string startUrl
                anchors.fill: parent
                radius: 10
                visible: index === win.currentIndex
                active: index === win.currentIndex
                focus: index === win.currentIndex
                Component.onCompleted: load(startUrl)
                onNewTabRequested: (url) => win.newTab(url)
            }
        }

        // load progress along the top of the card
        Rectangle {
            anchors { left: parent.left; top: parent.top; leftMargin: 10 }
            height: 2
            width: (parent.width - 20) * (current ? current.progress : 0)
            visible: current && current.loading
            color: pal.accent
        }

        // a failed load
        Rectangle {
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 24 }
            visible: current && current.error !== ""
            width: Math.min(errText.implicitWidth + 32, parent.width - 48)
            height: errText.implicitHeight + 20
            radius: 8
            color: "#2a1a1d"
            border.color: pal.danger
            Text {
                id: errText
                anchors.centerIn: parent
                width: parent.width - 32
                text: current ? current.error : ""
                color: pal.text
                wrapMode: Text.Wrap
                font.pixelSize: 13
            }
        }
    }

    // ------------------------------------------------------------ keys
    Shortcut { sequence: "Ctrl+T"; onActivated: newTab() }
    Shortcut { sequence: "Ctrl+W"; onActivated: closeTab(currentIndex) }
    Shortcut { sequence: "Ctrl+Tab"; onActivated: selectTab((currentIndex + 1) % tabs.count) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: selectTab((currentIndex + tabs.count - 1) % tabs.count) }
    Shortcut { sequence: "Ctrl+L"; onActivated: field.forceActiveFocus() }
    Shortcut { sequences: ["F5", "Ctrl+R"]; onActivated: if (current) current.reload() }
    Shortcut { sequence: "Alt+Left"; onActivated: if (current) current.goBack() }
    Shortcut { sequence: "Alt+Right"; onActivated: if (current) current.goForward() }
    Repeater {
        model: 9
        delegate: Item {
            required property int index
            Shortcut { sequence: "Ctrl+" + (index + 1); onActivated: selectTab(index) }
        }
    }

    function hostOf(url) {
        const m = /^[a-z]+:\/\/([^\/?#]+)/i.exec(url || "")
        return m ? m[1] : (url || "")
    }

    component Glyph: Rectangle {
        id: g
        property string glyph
        property int size: 16
        readonly property bool hovered: ma.containsMouse
        signal clicked()
        width: 32; height: 30; radius: 8
        color: ma.containsMouse && g.enabled ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
        opacity: g.enabled ? 1 : 0.35
        Text { anchors.centerIn: parent; text: g.glyph; color: pal.text; font.pixelSize: g.size }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; enabled: g.enabled; onClicked: g.clicked() }
    }
}
