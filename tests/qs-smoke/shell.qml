// WynlandOS - Quickshell smoke test (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/run.sh"):
// a window through xdg-shell, and what Quickshell.Hyprland sees of Zerp 2.0.
import QtQuick
import Quickshell
import Quickshell.Hyprland

ShellRoot {
    // a panel through wlr-layer-shell: across the top, keeping its strip
    PanelWindow {
        anchors { top: true; left: true; right: true }
        implicitHeight: 36
        color: "#e01e2030"
        Text {
            anchors.centerIn: parent
            color: "#8aadf4"
            font.pixelSize: 16
            text: "a Quickshell panel (wlr-layer-shell)  ·  workspace " + (Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : "?")
        }
    }

    FloatingWindow {
        implicitWidth: 420
        implicitHeight: 160
        color: "#1e2030"
        Text {
            anchors.centerIn: parent
            color: "#cad3f5"
            font.pixelSize: 20
            text: "Quickshell on Zerp 2.0\nworkspace " + (Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : "?")
        }
    }

    Timer {
        interval: 6000
        running: true
        onTriggered: {
            console.log("[qs-smoke] monitors=" + Hyprland.monitors.values.length +
                        " workspaces=" + Hyprland.workspaces.values.length +
                        " toplevels=" + Hyprland.toplevels.values.length +
                        " focused=" + (Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : "none") +
                        " screen=" + (Quickshell.screens.length > 0 ? Quickshell.screens[0].name : "none") +
                        " monitorFor=" + (Quickshell.screens.length > 0 && Hyprland.monitorFor(Quickshell.screens[0]) ? Hyprland.monitorFor(Quickshell.screens[0]).name : "none"));
            Hyprland.dispatch("workspace 2");
        }
    }
    Timer {
        interval: 9000
        running: true
        onTriggered: {
            console.log("[qs-smoke] after dispatch: focused=" + (Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : "none"));
            Hyprland.dispatch("workspace 1");
        }
    }
    Timer {
        interval: 120000
        running: true
        onTriggered: Qt.quit()
    }
}
