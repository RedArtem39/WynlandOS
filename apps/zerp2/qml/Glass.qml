// Frosted glass: this item's slice of the live blurred backdrop (Shell's
// `blurredWall`: the wallpaper through one GPU blur). Live -- a moving
// wallpaper (a video, an animation) shows through as it changes -- but
// re-rendered only when the backdrop changes, not every frame.
//
// gx, gy: this item's top-left in screen (Shell) coordinates.
import QtQuick

ShaderEffectSource {
    property real gx: 0
    property real gy: 0
    sourceItem: blurredWall   // Shell.qml's id: every user of this is created under Shell
    sourceRect: Qt.rect(gx, gy, width, height)
    live: true
    hideSource: false
    smooth: true
}
