// A rounded-corner cap: fills the part of an r x r square outside a
// quarter circle with a colour. Laid over a tile's corners in the border
// colour it rounds the tile without an offscreen pass (a mask layer per
// tile re-rendered every animation frame).
import QtQuick

Canvas {
    id: cap
    property color fill: "black"
    // 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left
    property int corner: 0
    rotation: corner * 90
    renderStrategy: Canvas.Cooperative
    onFillChanged: requestPaint()
    onWidthChanged: requestPaint()
    onPaint: {
        var c = getContext("2d");
        var r = width;
        c.reset();
        c.fillStyle = cap.fill;
        c.beginPath();
        c.moveTo(0, 0);
        c.lineTo(r, 0);
        c.arc(r, r, r, -Math.PI / 2, Math.PI, true);
        c.lineTo(0, 0);
        c.closePath();
        c.fill();
    }
}
