import QtQuick
import QtQuick.Controls

ToolButton {
    id: control
    property string glyph: ""
    property string tip: ""
    implicitWidth: 29
    implicitHeight: 29
    hoverEnabled: true
    Accessible.name: tip
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 400
    ToolTip.text: tip
    background: Rectangle {
        radius: 6
        color: control.checked ? "#2b2e34" : control.hovered ? "#24262a" : "transparent"
    }
    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            width: 17; height: 17
            name: control.glyph
            color: "#f1f1f2"
            opacity: control.enabled ? (control.checked ? 1 : 0.82) : 0.32
        }
    }
}
