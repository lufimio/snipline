import QtQuick
import QtQuick.Controls

ToolButton {
    id: control
    property string glyph: ""
    property string tip: ""
    property real glyphRotation: 0
    implicitWidth: 29
    implicitHeight: 29
    hoverEnabled: true
    Accessible.name: tip
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 400
    ToolTip.text: tip
    background: Rectangle {
        radius: 6
        color: control.checked ? theme.selectedColor : control.hovered ? theme.hoverColor : "transparent"
    }
    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            width: 17; height: 17
            name: control.glyph
            rotation: control.glyphRotation
            Behavior on rotation { NumberAnimation { duration: 140 } }
            color: theme.foreground
            opacity: control.enabled ? (control.checked ? 1 : 0.82) : 0.32
        }
    }
}
