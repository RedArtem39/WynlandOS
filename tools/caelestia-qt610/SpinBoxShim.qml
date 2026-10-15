// WynlandOS - QtQuick.Templates' DoubleSpinBox is Qt 6.11's; this is what
// Caelestia uses of it, over a Control, for the Qt 6.10 WynlandOS ships.
// tools/stage_caelestia.sh puts it in place of DoubleSpinBox.
import QtQuick
import QtQuick.Templates as T

T.Control {
    id: control

    property real from: 0
    property real to: 99
    property real value: 0
    property real stepSize: 1
    property int decimals: 2
    property bool editable: false
    property var validator: DoubleValidator {
        bottom: Math.min(control.from, control.to)
        top: Math.max(control.from, control.to)
        decimals: control.decimals
        notation: DoubleValidator.StandardNotation
    }
    property var textFromValue: function (v, locale) {
        return Number(v).toLocaleString(locale, "f", control.decimals);
    }
    property var valueFromText: function (text, locale) {
        return Number.fromLocaleString(locale, text);
    }
    signal valueModified

    property SpinButtonShim up: SpinButtonShim {}
    property SpinButtonShim down: SpinButtonShim {}

    function increase(): void { value = Math.min(to, value + stepSize); valueModified(); }
    function decrease(): void { value = Math.max(from, value - stepSize); valueModified(); }

    // the indicators are the control's children
    Binding { target: control.up.indicator; property: "parent"; value: control; when: control.up.indicator !== null }
    Binding { target: control.down.indicator; property: "parent"; value: control; when: control.down.indicator !== null }
}
