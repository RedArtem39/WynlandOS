// WynlandOS - DoubleSpinBox's up/down for Qt 6.10 (see SpinBoxShim.qml)
import QtQuick

QtObject {
    property Item indicator
    readonly property bool pressed: indicator ? indicator.pressed === true : false
    readonly property bool hovered: indicator ? indicator.hovered === true : false
}
