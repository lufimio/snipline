import QtQuick

Item {
    id: glyph
    property string name: ""
    property color color: "white"
    implicitWidth: 18
    implicitHeight: 18

    Image {
        anchors.fill: parent
        source: Qt.resolvedUrl("icons/" + (glyph.name.length ? glyph.name : "capture") + "-" +
               ((glyph.color.r * 0.299 + glyph.color.g * 0.587 + glyph.color.b * 0.114) > 0.50
                 ? "light" : "dark") + ".svg")
        fillMode: Image.PreserveAspectFit
        smooth: true
        sourceSize: Qt.size(Math.max(1, Math.round(width * 2)), Math.max(1, Math.round(height * 2)))
    }
}
