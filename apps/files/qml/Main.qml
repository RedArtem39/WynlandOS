// WynlandOS - Files.
//
// Places on the left, a breadcrumb path, grid or list view, a preview
// pane; copy/cut/paste, rename, delete, new folder/file, hidden files,
// type-to-filter. The file system work is the `fs` object (main.cpp).
//
// Keys: Enter open, Backspace up (or edit the filter), arrows move,
// Ctrl+A/C/X/V, Delete, F2 rename, F5 reload, Ctrl+N new folder,
// Ctrl+H hidden files, Ctrl+L type a path, Ctrl+G grid/list,
// Ctrl+P preview, Esc clear.
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: win
    visible: true
    width: Screen.width > 0 ? Screen.width : 1000
    height: Screen.height > 0 ? Screen.height : 640
    color: pal.bg
    title: "Files"

    QtObject {
        id: pal
        readonly property color bg: "#11141c"
        readonly property color side: "#0c0f15"
        readonly property color panel: "#161a24"
        readonly property color field: "#1b2030"
        readonly property color line: "#232838"
        readonly property color text: "#e6e8ef"
        readonly property color dim: "#8a91a8"
        readonly property color faint: "#5a6178"
        readonly property color accent: "#7aa2f7"
        readonly property color sel: "#25355a"
        readonly property color selLine: "#4e6fb8"
        readonly property color hover: "#1a1f2c"
        readonly property color danger: "#ff7b72"
        readonly property color ok: "#7ee787"
    }

    // ------------------------------------------------------------ state
    property string cwd: startDir
    property var entries: []
    property var shown: []
    property var history: []
    property int histPos: -1
    property bool showHidden: false
    property bool gridMode: true
    property bool previewWanted: true
    readonly property bool showPreview: previewWanted && body.width >= 900
    property var selected: ({})
    property int selCount: 0
    property int current: -1
    property string filter: ""
    property var clip: []
    property bool clipCut: false
    property string message: ""
    property bool messageBad: false
    property bool editingPath: false

    readonly property var places: [
        { label: "Home", glyph: "⌂", path: fs.home },
        { label: "Root", glyph: "/", path: "/" },
        { label: "Temp", glyph: "◷", path: "/tmp" },
        { label: "Config", glyph: "⚙", path: "/etc" },
        { label: "Programs", glyph: "▶", path: "/usr/bin" },
        { label: "Apps", glyph: "▦", path: "/apps" },
        { label: "Docs", glyph: "≡", path: "/docs" }
    ]

    readonly property var focusEntry: current >= 0 && current < shown.length ? shown[current] : null

    function say(text, bad) { message = text; messageBad = !!bad; messageTimer.restart() }

    function load(dir, push) {
        if (!fs.isDir(dir)) { say(dir + " is not a folder", true); return false }
        const list = fs.list(dir, showHidden)
        if (list.length === 0 && fs.lastError !== "" && fs.countEntries(dir) > 0) { say(fs.lastError, true); return false }
        const prev = cwd
        cwd = dir
        entries = list
        filter = ""
        selected = ({}); selCount = 0
        applyFilter()
        // coming back up: land on the folder we came from
        current = shown.length ? 0 : -1
        for (let i = 0; i < shown.length; i++)
            if (shown[i].path === prev) { current = i; break }
        if (push !== false) {
            history = history.slice(0, histPos + 1).concat([dir])
            histPos = history.length - 1
        }
        view().positionViewAtIndex(Math.max(0, current), ItemView.Contain)
        return true
    }
    function reload() {
        const keep = focusEntry ? focusEntry.path : ""
        entries = fs.list(cwd, showHidden)
        applyFilter()
        for (let i = 0; i < shown.length; i++) if (shown[i].path === keep) { current = i; return }
        current = Math.min(current, shown.length - 1)
    }
    function applyFilter() {
        const f = filter.toLowerCase()
        shown = f === "" ? entries : entries.filter(e => e.name.toLowerCase().indexOf(f) >= 0)
        if (current >= shown.length) current = shown.length - 1
        if (current < 0 && shown.length) current = 0
    }
    onFilterChanged: applyFilter()
    onShowHiddenChanged: reload()

    function back() { if (histPos > 0) { histPos--; load(history[histPos], false) } }
    function forward() { if (histPos < history.length - 1) { histPos++; load(history[histPos], false) } }
    function up() { if (cwd !== "/") load(fs.parentOf(cwd)) }

    function view() { return gridMode ? grid : list }

    // selection
    function isSel(p) { return selected[p] === true }
    function select(i, toggle) {
        if (i < 0 || i >= shown.length) return
        const s = toggle ? Object.assign({}, selected) : ({})
        const p = shown[i].path
        if (toggle && s[p]) delete s[p]; else s[p] = true
        selected = s
        selCount = Object.keys(s).length
        current = i
    }
    function selectAll() {
        const s = ({})
        for (const e of shown) s[e.path] = true
        selected = s; selCount = shown.length
    }
    function clearSel() { selected = ({}); selCount = 0 }
    function targets() {   // what an action applies to: the selection, else the focused item
        const keys = Object.keys(selected)
        if (keys.length) return keys
        return focusEntry ? [focusEntry.path] : []
    }

    function open(e) {
        if (!e) return
        if (e.dir) { load(e.path); return }
        if (e.kind === "exec") {
            if (fs.spawn(e.path)) say("Started " + e.name)
            else say(fs.lastError, true)
            return
        }
        // text and images: the preview pane when there's room, else a viewer
        if (showPreview) return
        if (e.kind === "text" || e.kind === "image" || e.kind === "other") { viewer.entry = e; viewer.open() }
    }

    // clipboard
    function copySel(cut) {
        clip = targets(); clipCut = cut
        if (clip.length) say((cut ? "Cut " : "Copied ") + clip.length + (clip.length === 1 ? " item" : " items"))
    }
    function paste() {
        if (!clip.length) return
        let ok = 0
        for (const p of clip) {
            if (!fs.exists(p)) continue
            if (clipCut ? fs.move(p, cwd) : fs.copy(p, cwd)) ok++
            else { say(fs.lastError, true); break }
        }
        if (clipCut) clip = []
        reload()
        if (ok) say((clipCut ? "Moved " : "Pasted ") + ok + (ok === 1 ? " item" : " items"))
    }
    function removeSel() {
        const t = targets()
        let ok = 0
        for (const p of t) {
            if (fs.remove(p)) ok++
            else { say(fs.lastError, true); break }
        }
        clearSel(); reload()
        if (ok) say("Deleted " + ok + (ok === 1 ? " item" : " items"))
    }

    function ask(kind, initial) {
        dialog.kind = kind
        dialog.initial = initial
        dialog.open()
    }

    function fmtDate(ms) { return ms > 0 ? Qt.formatDateTime(new Date(ms), "dd MMM yyyy  hh:mm") : "—" }
    function ownerName(uid) { return uid === 0 ? "root" : uid === 1000 ? "user" : "uid " + uid }
    function kindLabel(e) {
        return e.dir ? "Folder" : e.kind === "exec" ? "Program" : e.kind === "image" ? "Image"
             : e.kind === "text" ? "Text" : e.kind === "archive" ? "Archive" : "File"
    }

    Timer { id: messageTimer; interval: 4000; onTriggered: win.message = "" }

    Component.onCompleted: { if (!load(cwd)) load(fs.home) ; keys.forceActiveFocus() }

    // ------------------------------------------------------------ keys
    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Keys.onPressed: (ev) => {
            const ctrl = ev.modifiers & Qt.ControlModifier
            const n = win.shown.length
            const cols = win.gridMode ? Math.max(1, Math.floor(grid.width / grid.cellWidth)) : 1
            let handled = true
            if (ctrl) {
                switch (ev.key) {
                case Qt.Key_A: win.selectAll(); break
                case Qt.Key_C: win.copySel(false); break
                case Qt.Key_X: win.copySel(true); break
                case Qt.Key_V: win.paste(); break
                case Qt.Key_H: win.showHidden = !win.showHidden; break
                case Qt.Key_L: win.editingPath = true; break
                case Qt.Key_N: win.ask("folder", "New folder"); break
                case Qt.Key_G: win.gridMode = !win.gridMode; break
                case Qt.Key_P: win.previewWanted = !win.previewWanted; break
                default: handled = false
                }
            } else switch (ev.key) {
            case Qt.Key_Right: if (win.current < n - 1) win.select(win.current + 1); break
            case Qt.Key_Left: if (win.current > 0) win.select(win.current - 1); break
            case Qt.Key_Down: win.select(Math.min(n - 1, win.current + cols)); break
            case Qt.Key_Up: win.select(Math.max(0, win.current - cols)); break
            case Qt.Key_Home: win.select(0); break
            case Qt.Key_End: win.select(n - 1); break
            case Qt.Key_PageDown: win.select(Math.min(n - 1, win.current + cols * 6)); break
            case Qt.Key_PageUp: win.select(Math.max(0, win.current - cols * 6)); break
            case Qt.Key_Return:
            case Qt.Key_Enter: win.open(win.focusEntry); break
            case Qt.Key_Backspace:
                if (win.filter !== "") win.filter = win.filter.slice(0, -1)
                else win.up()
                break
            case Qt.Key_Delete: if (win.targets().length) confirm.open(); break
            case Qt.Key_F2: if (win.focusEntry) win.ask("rename", win.focusEntry.name); break
            case Qt.Key_F5: win.reload(); break
            case Qt.Key_Escape:
                if (win.filter !== "") win.filter = ""
                else win.clearSel()
                break
            default:
                // type to filter
                if (ev.text.length === 1 && ev.text >= " " && ev.text !== "\x7f") win.filter += ev.text
                else handled = false
            }
            if (handled) {
                ev.accepted = true
                win.view().positionViewAtIndex(Math.max(0, win.current), ItemView.Contain)
            }
        }
    }

    // ------------------------------------------------------------ layout
    RowLayout {
        id: body
        anchors.fill: parent
        spacing: 0

        // ---- places
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: body.width >= 720 ? 186 : 52
            color: pal.side
            id: sideBar
            readonly property bool compact: width < 100

            Column {
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10; topMargin: 14 }
                spacing: 2
                Text {
                    visible: !sideBar.compact
                    text: "PLACES"
                    color: pal.faint
                    font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2
                    leftPadding: 8; bottomPadding: 6
                }
                Repeater {
                    model: win.places
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool here: win.cwd === modelData.path ||
                                                     (modelData.path !== "/" && win.cwd.startsWith(modelData.path + "/"))
                        width: parent.width; height: 32; radius: 8
                        color: here ? pal.sel : placeMouse.containsMouse ? pal.hover : "transparent"
                        visible: fs.isDir(modelData.path)
                        Text {
                            id: glyph
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            width: 18; horizontalAlignment: Text.AlignHCenter
                            text: modelData.glyph
                            color: here ? pal.accent : pal.dim
                            font.pixelSize: 15
                        }
                        Text {
                            visible: !sideBar.compact
                            anchors { left: glyph.right; leftMargin: 10; verticalCenter: parent.verticalCenter }
                            text: modelData.label
                            color: here ? pal.text : "#c4c9d8"
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: placeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { win.load(modelData.path); keys.forceActiveFocus() }
                        }
                    }
                }
            }

            // who you are here
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 12 }
                height: 34; radius: 8
                color: pal.panel
                border.color: pal.line
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: fs.uid === 0 ? pal.danger : pal.ok
                    }
                    Text {
                        visible: !sideBar.compact
                        text: fs.uid === 0 ? "root" : "user"
                        color: pal.dim; font.pixelSize: 12
                    }
                }
            }
            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: pal.line }
        }

        // ---- main column
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 220
            Layout.fillHeight: true
            spacing: 0

            // toolbar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 50
                color: pal.bg
                RowLayout {
                    anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                    spacing: 6
                    ToolBtn { glyph: "‹"; size: 22; enabled: win.histPos > 0; onClicked: win.back() }
                    ToolBtn { glyph: "›"; size: 22; enabled: win.histPos < win.history.length - 1; onClicked: win.forward() }
                    ToolBtn { glyph: "↑"; enabled: win.cwd !== "/"; onClicked: win.up() }

                    // breadcrumbs / typed path
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: 8
                        color: pal.field
                        border.color: win.editingPath ? pal.selLine : pal.line
                        clip: true

                        Flickable {
                            id: crumbsFlick
                            visible: !win.editingPath
                            anchors { fill: parent; leftMargin: 6; rightMargin: 6 }
                            contentWidth: crumbs.width
                            contentX: Math.max(0, contentWidth - width)
                            interactive: false
                            Row {
                                id: crumbs
                                height: parent.height
                                Repeater {
                                    model: {
                                        const parts = win.cwd.split("/").filter(s => s !== "")
                                        const out = [{ label: "/", path: "/" }]
                                        let acc = ""
                                        for (const p of parts) { acc += "/" + p; out.push({ label: p, path: acc }) }
                                        return out
                                    }
                                    delegate: Row {
                                        required property var modelData
                                        required property int index
                                        height: crumbs.height
                                        Text {
                                            visible: index > 1
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: "›"; color: pal.faint; font.pixelSize: 14
                                            leftPadding: 2; rightPadding: 2
                                        }
                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: crumbText.width + 14; height: 24; radius: 6
                                            color: crumbMouse.containsMouse ? pal.hover : "transparent"
                                            Text {
                                                id: crumbText
                                                anchors.centerIn: parent
                                                text: modelData.label
                                                color: modelData.path === win.cwd ? pal.text : pal.dim
                                                font.pixelSize: 13
                                                font.bold: modelData.path === win.cwd
                                            }
                                            MouseArea {
                                                id: crumbMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onClicked: { win.load(modelData.path); keys.forceActiveFocus() }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        MouseArea {   // click the empty part: type a path
                            anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                            width: Math.max(0, parent.width - crumbs.width - 12)
                            visible: !win.editingPath
                            onClicked: win.editingPath = true
                        }
                        TextInput {
                            id: pathInput
                            visible: win.editingPath
                            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                            verticalAlignment: TextInput.AlignVCenter
                            color: pal.text
                            selectionColor: pal.selLine
                            font.pixelSize: 13
                            font.family: "DejaVu Sans Mono"
                            clip: true
                            onVisibleChanged: if (visible) { text = win.cwd; forceActiveFocus(); selectAll() }
                            Keys.onReturnPressed: { const t = text.trim(); win.editingPath = false; if (t !== "") win.load(t); keys.forceActiveFocus() }
                            Keys.onEnterPressed: Keys.onReturnPressed(event)
                            Keys.onEscapePressed: { win.editingPath = false; keys.forceActiveFocus() }
                            onActiveFocusChanged: if (!activeFocus) win.editingPath = false
                        }
                    }

                    // filter indicator (type anywhere to filter)
                    Rectangle {
                        visible: win.filter !== ""
                        Layout.preferredHeight: 32
                        Layout.preferredWidth: filterText.width + 40
                        radius: 8
                        color: pal.field
                        border.color: pal.selLine
                        Text {
                            id: filterText
                            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                            text: "filter: " + win.filter
                            color: pal.text; font.pixelSize: 13
                        }
                        Text {
                            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                            text: "✕"; color: pal.dim; font.pixelSize: 12
                            MouseArea { anchors.fill: parent; anchors.margins: -6; onClicked: win.filter = "" }
                        }
                    }

                    ToolBtn { glyph: "+"; size: 20; tip: "New folder"; visible: body.width >= 560; onClicked: win.ask("folder", "New folder") }
                    ToolBtn { glyph: win.gridMode ? "☰" : "▦"; onClicked: win.gridMode = !win.gridMode }
                    ToolBtn { glyph: "◐"; visible: body.width >= 560; checked: win.showHidden; onClicked: win.showHidden = !win.showHidden }
                    ToolBtn { glyph: "◨"; checked: win.showPreview; onClicked: win.previewWanted = !win.previewWanted }
                }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: pal.line }
            }

            // list header
            Rectangle {
                visible: !win.gridMode
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: pal.bg
                Row {
                    anchors { fill: parent; leftMargin: 18; rightMargin: 18 }
                    Text { width: parent.width - 330; text: "Name"; color: pal.faint; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                    Text { width: 80; text: "Size"; color: pal.faint; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignRight; anchors.verticalCenter: parent.verticalCenter }
                    Text { width: 150; text: "Modified"; color: pal.faint; font.pixelSize: 11; font.bold: true; leftPadding: 20; anchors.verticalCenter: parent.verticalCenter }
                    Text { width: 100; text: "Permissions"; color: pal.faint; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: pal.line }
            }

            // the files
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                MouseArea {   // empty space: clear selection / background menu
                    id: bgMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (m) => {
                        win.clearSel(); keys.forceActiveFocus()
                        if (m.button === Qt.RightButton) ctx.popupAt(null, m.x, m.y, bgMouse)
                    }
                }

                GridView {
                    id: grid
                    visible: win.gridMode
                    anchors { fill: parent; margins: 10 }
                    clip: true
                    model: win.shown
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 112)))
                    cellHeight: 112
                    currentIndex: win.current
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Item {
                        id: cell
                        required property var modelData
                        required property int index
                        width: grid.cellWidth; height: grid.cellHeight
                        readonly property bool sel: win.isSel(modelData.path)
                        readonly property bool cur: index === win.current
                        Rectangle {
                            anchors { fill: parent; margins: 4 }
                            radius: 10
                            color: cell.sel ? pal.sel : cellMouse.containsMouse ? pal.hover : "transparent"
                            border.color: cell.cur && keys.activeFocus ? pal.selLine : "transparent"
                        }
                        FileIcon {
                            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 10 }
                            width: 54; height: 54
                            kind: modelData.kind; name: modelData.name; path: modelData.path
                            opacity: win.clipCut && win.clip.indexOf(modelData.path) >= 0 ? 0.45 : 1
                        }
                        Text {
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 68; leftMargin: 8; rightMargin: 8 }
                            text: modelData.name
                            color: modelData.name.startsWith(".") ? pal.dim : pal.text
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WrapAnywhere
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: cellMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: (m) => {
                                keys.forceActiveFocus()
                                if (m.button === Qt.RightButton) { if (!cell.sel) win.select(cell.index) }
                                else win.select(cell.index, m.modifiers & Qt.ControlModifier)
                            }
                            onClicked: (m) => { if (m.button === Qt.RightButton) ctx.popupAt(cell.modelData, m.x, m.y, cellMouse) }
                            onDoubleClicked: (m) => { if (m.button === Qt.LeftButton) win.open(cell.modelData) }
                        }
                    }
                }

                ListView {
                    id: list
                    visible: !win.gridMode
                    anchors { fill: parent; margins: 6 }
                    clip: true
                    model: win.shown
                    currentIndex: win.current
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        width: list.width; height: 30; radius: 6
                        readonly property bool sel: win.isSel(modelData.path)
                        color: sel ? pal.sel : rowMouse.containsMouse ? pal.hover : (index % 2 ? "#13161f" : "transparent")
                        border.color: index === win.current && keys.activeFocus ? pal.selLine : "transparent"
                        Row {
                            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                            Item {
                                width: parent.width - 330; height: parent.height
                                FileIcon {
                                    id: rowIcon
                                    width: 22; height: 22; anchors.verticalCenter: parent.verticalCenter
                                    kind: row.modelData.kind; name: row.modelData.name; path: row.modelData.path
                                    thumbnail: false
                                }
                                Text {
                                    anchors { left: rowIcon.right; leftMargin: 10; right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: row.modelData.name + (row.modelData.link ? "  →" : "")
                                    color: row.modelData.name.startsWith(".") ? pal.dim : pal.text
                                    font.pixelSize: 13
                                    elide: Text.ElideMiddle
                                }
                            }
                            Text { width: 80; anchors.verticalCenter: parent.verticalCenter; horizontalAlignment: Text.AlignRight
                                   text: row.modelData.dir ? "" : fs.sizeString(row.modelData.size); color: pal.dim; font.pixelSize: 12 }
                            Text { width: 150; anchors.verticalCenter: parent.verticalCenter; leftPadding: 20
                                   text: win.fmtDate(row.modelData.mtime); color: pal.dim; font.pixelSize: 12 }
                            Text { width: 100; anchors.verticalCenter: parent.verticalCenter
                                   text: fs.permString(row.modelData.mode, row.modelData.dir); color: pal.faint
                                   font.pixelSize: 12; font.family: "DejaVu Sans Mono" }
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: (m) => {
                                keys.forceActiveFocus()
                                if (m.button === Qt.RightButton) { if (!row.sel) win.select(row.index) }
                                else win.select(row.index, m.modifiers & Qt.ControlModifier)
                            }
                            onClicked: (m) => { if (m.button === Qt.RightButton) ctx.popupAt(row.modelData, m.x, m.y, rowMouse) }
                            onDoubleClicked: (m) => { if (m.button === Qt.LeftButton) win.open(row.modelData) }
                        }
                    }
                }

                // empty folder
                Column {
                    anchors.centerIn: parent
                    visible: win.shown.length === 0
                    spacing: 8
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: win.filter !== "" ? "⌕" : "∅"; color: pal.faint; font.pixelSize: 40 }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: win.filter !== "" ? "Nothing matches “" + win.filter + "”" : "This folder is empty"
                        color: pal.dim; font.pixelSize: 14
                    }
                }
            }

            // status bar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: pal.side
                Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: pal.line }
                Text {
                    anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
                    text: {
                        let s = win.shown.length + (win.shown.length === 1 ? " item" : " items")
                        if (win.selCount) {
                            let bytes = 0
                            for (const e of win.shown) if (win.selected[e.path] && !e.dir) bytes += e.size
                            s += "  ·  " + win.selCount + " selected" + (bytes ? "  (" + fs.sizeString(bytes) + ")" : "")
                        }
                        if (win.clip.length) s += "  ·  " + win.clip.length + (win.clipCut ? " to move" : " to paste")
                        return s
                    }
                    color: pal.dim; font.pixelSize: 12
                }
                Text {
                    anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
                    text: win.message
                    color: win.messageBad ? pal.danger : pal.ok
                    font.pixelSize: 12
                }
            }
        }

        // ---- preview
        Rectangle {
            id: pv
            visible: win.showPreview
            Layout.fillHeight: true
            Layout.preferredWidth: 280
            color: pal.panel
            Rectangle { anchors { left: parent.left; top: parent.top; bottom: parent.bottom } width: 1; color: pal.line }

            readonly property var e: win.focusEntry
            ColumnLayout {
                anchors { fill: parent; margins: 16 }
                spacing: 10
                visible: pv.e !== null

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: pv.e && pv.e.kind === "image" ? 180 : 110
                    FileIcon {
                        visible: !(pv.e && pv.e.kind === "image")
                        anchors.centerIn: parent
                        width: 96; height: 96
                        kind: pv.e ? pv.e.kind : "other"
                        name: pv.e ? pv.e.name : ""
                        thumbnail: false
                    }
                    Image {
                        anchors.fill: parent
                        visible: !!(pv.e && pv.e.kind === "image")
                        source: visible ? "file://" + pv.e.path : ""
                        sourceSize.width: 560
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: pv.e ? pv.e.name : ""
                    color: pal.text; font.pixelSize: 15; font.bold: true
                    wrapMode: Text.WrapAnywhere
                    horizontalAlignment: Text.AlignHCenter
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    color: pal.dim; font.pixelSize: 12
                    text: {
                        const e = pv.e
                        if (!e) return ""
                        if (e.dir) { const n = fs.countEntries(e.path); return "Folder  ·  " + n + (n === 1 ? " item" : " items") }
                        return win.kindLabel(e) + "  ·  " + fs.sizeString(e.size)
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: pal.line }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2; columnSpacing: 12; rowSpacing: 6
                    readonly property var e: pv.e
                    Text { text: "Modified"; color: pal.faint; font.pixelSize: 12 }
                    Text { Layout.fillWidth: true; text: pv.e ? win.fmtDate(pv.e.mtime) : ""; color: pal.text; font.pixelSize: 12 }
                    Text { text: "Owner"; color: pal.faint; font.pixelSize: 12 }
                    Text { text: pv.e ? win.ownerName(pv.e.owner) : ""; color: pal.text; font.pixelSize: 12 }
                    Text { text: "Access"; color: pal.faint; font.pixelSize: 12 }
                    Text { text: pv.e ? fs.permString(pv.e.mode, pv.e.dir) : ""; color: pal.text; font.pixelSize: 12; font.family: "DejaVu Sans Mono" }
                    Text { text: "Path"; color: pal.faint; font.pixelSize: 12 }
                    Text { Layout.fillWidth: true; text: pv.e ? pv.e.path : ""; color: pal.dim; font.pixelSize: 11; wrapMode: Text.WrapAnywhere }
                }
                // text preview
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: "#0e1118"
                    border.color: pal.line
                    visible: pv.e !== null && pv.e.kind === "text"
                    clip: true
                    Flickable {
                        id: textFlick
                        anchors { fill: parent; margins: 10 }
                        contentHeight: previewText.height
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        Text {
                            id: previewText
                            width: textFlick.width
                            text: pv.e && pv.e.kind === "text" ? fs.readText(pv.e.path, 8192) : ""
                            color: "#c4c9d8"
                            font.family: "DejaVu Sans Mono"
                            font.pixelSize: 11
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                        }
                    }
                }
                Item { Layout.fillHeight: true; visible: !(pv.e !== null && pv.e.kind === "text") }
                Button {
                    Layout.fillWidth: true
                    visible: pv.e !== null && (pv.e.dir || pv.e.kind === "exec")
                    text: pv.e && pv.e.dir ? "Open" : "Run"
                    onClicked: { win.open(pv.e); keys.forceActiveFocus() }
                    background: Rectangle { radius: 8; color: parent.down ? "#3d5fa8" : parent.hovered ? "#4b70c4" : "#41619f" }
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: pv.e === null
                text: "Nothing selected"
                color: pal.faint; font.pixelSize: 13
            }
        }
    }

    // ------------------------------------------------------------ context menu
    Popup {
        id: ctx
        property var target: null
        padding: 6
        modal: false
        focus: true
        background: Rectangle { color: "#1b2030"; radius: 10; border.color: "#2e3550" }
        onClosed: keys.forceActiveFocus()

        function popupAt(entry, x, y, item) {
            target = entry
            const p = item.mapToItem(win.contentItem, x, y)
            ctx.x = Math.min(p.x, body.width - 210)
            ctx.y = Math.min(p.y, body.height - implicitHeight - 8)
            open()
        }

        readonly property var items: target ? [
            { label: target.dir ? "Open" : target.kind === "exec" ? "Run" : "Preview", key: "Enter", act: () => win.open(target) },
            { sep: true },
            { label: "Copy", key: "Ctrl+C", act: () => win.copySel(false) },
            { label: "Cut", key: "Ctrl+X", act: () => win.copySel(true) },
            { label: "Paste here", key: "Ctrl+V", act: () => win.paste(), off: win.clip.length === 0 },
            { sep: true },
            { label: "Rename…", key: "F2", act: () => win.ask("rename", target.name) },
            { label: "Duplicate", key: "", act: () => { if (fs.copy(target.path, win.cwd)) win.reload(); else win.say(fs.lastError, true) } },
            { label: "Delete", key: "Del", danger: true, act: () => confirm.open() }
        ] : [
            { label: "New folder…", key: "Ctrl+N", act: () => win.ask("folder", "New folder") },
            { label: "New file…", key: "", act: () => win.ask("file", "untitled.txt") },
            { label: "Paste", key: "Ctrl+V", act: () => win.paste(), off: win.clip.length === 0 },
            { sep: true },
            { label: win.showHidden ? "Hide hidden files" : "Show hidden files", key: "Ctrl+H", act: () => win.showHidden = !win.showHidden },
            { label: win.gridMode ? "List view" : "Grid view", key: "Ctrl+G", act: () => win.gridMode = !win.gridMode },
            { label: "Reload", key: "F5", act: () => win.reload() },
            { sep: true },
            { label: "Open terminal", key: "", act: () => { if (!fs.spawn("/zerp_term.elf")) win.say(fs.lastError, true) } }
        ]

        contentItem: Column {
            width: 200
            Repeater {
                model: ctx.items
                delegate: Item {
                    required property var modelData
                    width: 200
                    height: modelData.sep ? 9 : 30
                    Rectangle { visible: !!modelData.sep; anchors.centerIn: parent; width: parent.width - 12; height: 1; color: "#2e3550" }
                    Rectangle {
                        visible: !modelData.sep
                        anchors.fill: parent
                        radius: 6
                        color: itemMouse.containsMouse && !modelData.off ? (modelData.danger ? "#4a2328" : "#2a3a5e") : "transparent"
                        Text {
                            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                            text: modelData.label || ""
                            color: modelData.off ? pal.faint : modelData.danger ? pal.danger : pal.text
                            font.pixelSize: 13
                        }
                        Text {
                            anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                            text: modelData.key || ""
                            color: pal.faint; font.pixelSize: 11
                        }
                        MouseArea {
                            id: itemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { if (modelData.off) return; const a = modelData.act; ctx.close(); a() }
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ name dialog
    Popup {
        id: dialog
        property string kind: "folder"     // folder | file | rename
        property string initial: ""
        anchors.centerIn: parent
        width: 360
        modal: true
        focus: true
        padding: 18
        background: Rectangle { color: "#1b2030"; radius: 12; border.color: "#2e3550" }
        Overlay.modal: Rectangle { color: "#80000000" }
        onOpened: { nameField.text = initial; nameField.forceActiveFocus(); nameField.select(0, initial.lastIndexOf(".") > 0 && kind !== "folder" ? initial.lastIndexOf(".") : initial.length) }
        onClosed: keys.forceActiveFocus()

        function accept() {
            const name = nameField.text.trim()
            if (name === "" || name.indexOf("/") >= 0) { win.say("Not a valid name", true); return }
            let ok
            if (kind === "rename") {
                ok = fs.rename(win.focusEntry.path, fs.join(win.cwd, name))
            } else if (kind === "folder") {
                ok = fs.makeDir(fs.join(win.cwd, name))
            } else {
                ok = fs.makeFile(fs.join(win.cwd, name))
            }
            close()
            if (!ok) { win.say(fs.lastError, true); return }
            win.reload()
            for (let i = 0; i < win.shown.length; i++) if (win.shown[i].name === name) { win.select(i); break }
        }

        contentItem: Column {
            spacing: 14
            Text {
                text: dialog.kind === "rename" ? "Rename" : dialog.kind === "folder" ? "New folder" : "New file"
                color: pal.text; font.pixelSize: 15; font.bold: true
            }
            Rectangle {
                width: parent.width; height: 36; radius: 8
                color: pal.field; border.color: pal.selLine
                TextInput {
                    id: nameField
                    anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                    verticalAlignment: TextInput.AlignVCenter
                    color: pal.text; selectionColor: pal.selLine
                    font.pixelSize: 14
                    clip: true
                    Keys.onReturnPressed: dialog.accept()
                    Keys.onEnterPressed: dialog.accept()
                    Keys.onEscapePressed: dialog.close()
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                DlgBtn { text: "Cancel"; onClicked: dialog.close() }
                DlgBtn { text: dialog.kind === "rename" ? "Rename" : "Create"; primary: true; onClicked: dialog.accept() }
            }
        }
    }

    // ------------------------------------------------------------ delete confirm
    Popup {
        id: confirm
        anchors.centerIn: parent
        width: 380
        modal: true
        focus: true
        padding: 18
        background: Rectangle { color: "#1b2030"; radius: 12; border.color: "#2e3550" }
        Overlay.modal: Rectangle { color: "#80000000" }
        property var paths: []
        onAboutToShow: paths = win.targets()
        onClosed: keys.forceActiveFocus()
        contentItem: Column {
            spacing: 14
            focus: true
            Keys.onReturnPressed: { confirm.close(); win.removeSel() }
            Keys.onEscapePressed: confirm.close()
            Text {
                text: confirm.paths.length === 1
                      ? "Delete “" + confirm.paths[0].substring(confirm.paths[0].lastIndexOf("/") + 1) + "”?"
                      : "Delete " + confirm.paths.length + " items?"
                color: pal.text; font.pixelSize: 15; font.bold: true
                width: parent.width; wrapMode: Text.WrapAnywhere
            }
            Text {
                text: "This can't be undone. Folders are deleted with everything in them."
                color: pal.dim; font.pixelSize: 12
                width: parent.width; wrapMode: Text.WordWrap
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                DlgBtn { text: "Cancel"; onClicked: confirm.close() }
                DlgBtn { text: "Delete"; danger: true; onClicked: { confirm.close(); win.removeSel() } }
            }
        }
    }

    // ------------------------------------------------------------ viewer (narrow tiles)
    Popup {
        id: viewer
        property var entry: null
        x: 0; y: 0
        width: body.width; height: body.height
        modal: true
        focus: true
        padding: 0
        background: Rectangle { color: "#0b0d13" }
        onClosed: keys.forceActiveFocus()
        contentItem: Item {
            focus: true
            Keys.onEscapePressed: viewer.close()
            Keys.onReturnPressed: viewer.close()
            Rectangle {
                id: vbar
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 44
                color: pal.panel
                Text {
                    anchors { left: parent.left; leftMargin: 16; right: vclose.left; verticalCenter: parent.verticalCenter }
                    text: viewer.entry ? viewer.entry.name + "   ·   " + fs.sizeString(viewer.entry.size) : ""
                    color: pal.text; font.pixelSize: 14; font.bold: true
                    elide: Text.ElideMiddle
                }
                Rectangle {
                    id: vclose
                    anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    width: 30; height: 30; radius: 8
                    color: vcm.containsMouse ? pal.hover : "transparent"
                    Text { anchors.centerIn: parent; text: "✕"; color: pal.dim; font.pixelSize: 14 }
                    MouseArea { id: vcm; anchors.fill: parent; hoverEnabled: true; onClicked: viewer.close() }
                }
            }
            Image {
                anchors { fill: parent; topMargin: 56; margins: 12 }
                visible: !!(viewer.entry && viewer.entry.kind === "image")
                source: visible ? "file://" + viewer.entry.path : ""
                sourceSize.width: body.width
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }
            Flickable {
                id: vflick
                anchors { fill: parent; topMargin: 56; margins: 16 }
                visible: !!(viewer.entry && viewer.entry.kind !== "image")
                contentHeight: vtext.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Text {
                    id: vtext
                    width: vflick.width
                    text: vflick.visible ? fs.readText(viewer.entry.path, 65536) : ""
                    color: "#c4c9d8"
                    font.family: "DejaVu Sans Mono"; font.pixelSize: 12
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                }
            }
        }
    }

    // ------------------------------------------------------------ small components
    component ToolBtn: Rectangle {
        id: tb
        property string glyph: ""
        property int size: 16
        property bool checked: false
        property string tip: ""
        signal clicked()
        Layout.preferredWidth: 32
        Layout.preferredHeight: 32
        radius: 8
        color: checked ? pal.sel : tbMouse.containsMouse && enabled ? pal.hover : "transparent"
        opacity: enabled ? 1 : 0.35
        Text { anchors.centerIn: parent; text: tb.glyph; color: tb.checked ? pal.accent : pal.text; font.pixelSize: tb.size }
        MouseArea { id: tbMouse; anchors.fill: parent; hoverEnabled: true; onClicked: { if (tb.enabled) tb.clicked(); keys.forceActiveFocus() } }
    }

    component DlgBtn: Rectangle {
        id: db
        property string text: ""
        property bool primary: false
        property bool danger: false
        signal clicked()
        width: Math.max(84, label.width + 28); height: 32; radius: 8
        color: danger ? (dbMouse.containsMouse ? "#d9534f" : "#b8433f")
             : primary ? (dbMouse.containsMouse ? "#4b70c4" : "#41619f")
             : (dbMouse.containsMouse ? "#2a3044" : "#232838")
        Text { id: label; anchors.centerIn: parent; text: db.text; color: "white"; font.pixelSize: 13 }
        MouseArea { id: dbMouse; anchors.fill: parent; hoverEnabled: true; onClicked: db.clicked() }
    }
}
