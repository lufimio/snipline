import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    focus: true
    clip: true
    property real sx: width / outputRect.width
    property real sy: height / outputRect.height
    property real rx: (selection.rect.x - outputRect.x) * sx
    property real ry: (selection.rect.y - outputRect.y) * sy
    property real rw: selection.rect.width * sx
    property real rh: selection.rect.height * sy
    Keys.onEscapePressed: controller.cancel()
    Keys.onReturnPressed: controller.confirm()
    Keys.onEnterPressed: controller.confirm()

    Image {
        anchors.fill: parent
        source: controller.live || controller.mode === "window" ? "" : "image://frames/" + outputIndex
        cache: false
        fillMode: Image.Stretch
    }
    Rectangle {
        anchors.fill: parent
        color: theme.dimColor
        opacity: theme.dimOpacity
        visible: !controller.live
    }
    Item {
        anchors.fill: parent
        visible: controller.live
        property real leftEdge: selection.valid ? Math.max(0, Math.min(width, root.rx)) : 0
        property real rightEdge: selection.valid ? Math.max(0, Math.min(width, root.rx + root.rw)) : 0
        property real topEdge: selection.valid ? Math.max(0, Math.min(height, root.ry)) : 0
        property real bottomEdge: selection.valid ? Math.max(0, Math.min(height, root.ry + root.rh)) : 0
        Repeater {
            model: [
                [0, 0, root.width, parent.topEdge],
                [0, parent.bottomEdge, root.width, root.height - parent.bottomEdge],
                [0, parent.topEdge, parent.leftEdge, parent.bottomEdge - parent.topEdge],
                [parent.rightEdge, parent.topEdge, root.width - parent.rightEdge, parent.bottomEdge - parent.topEdge]
            ]
            Rectangle {
                required property var modelData
                x: modelData[0]
                y: modelData[1]
                width: modelData[2]
                height: modelData[3]
                color: theme.dimColor
                opacity: theme.dimOpacity
            }
        }
    }
    Item {
        x: root.rx
        y: root.ry
        width: Math.max(0, root.rw)
        height: Math.max(0, root.rh)
        visible: selection.valid && !controller.live && controller.mode !== "window"
        clip: true
        Image {
            x: -root.rx
            y: -root.ry
            width: root.width
            height: root.height
            source: controller.live || controller.mode === "window" ? "" : "image://frames/" + outputIndex
            cache: false
            fillMode: Image.Stretch
        }
    }
    Rectangle {
        x: root.rx
        y: root.ry
        width: Math.max(0, root.rw)
        height: Math.max(0, root.rh)
        visible: selection.valid && controller.mode !== "window"
        color: "transparent"
        border.color: theme.accent
        border.width: theme.outlineWidth
        Repeater {
            model: [[0, 0], [1, 0], [0, 1], [1, 1]]
            Rectangle {
                required property var modelData
                x: modelData[0] * root.rw - width / 2
                y: modelData[1] * root.rh - height / 2
                width: theme.handleSize
                height: width
                color: theme.accent
                visible: controller.mode === "region"
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: !controller.busy && controller.error.length === 0
        cursorShape: controller.mode === "region" ? Qt.CrossCursor : Qt.PointingHandCursor
        onPressed: mouse => {
            root.forceActiveFocus()
            if (controller.mode === "monitor")
                controller.selectMonitor(outputIndex)
            else if (controller.mode === "region")
                selection.begin(outputRect.x + mouse.x / root.sx,
                                outputRect.y + mouse.y / root.sy)
            else if (controller.mode === "window" && controller.desktopPicking)
                controller.pickWindowAt(outputRect.x + mouse.x / root.sx,
                                        outputRect.y + mouse.y / root.sy)
        }
        onPositionChanged: mouse => {
            if (pressed && controller.mode === "region")
                selection.update(outputRect.x + mouse.x / root.sx,
                                 outputRect.y + mouse.y / root.sy)
        }
        onReleased: selection.end()
        onCanceled: selection.end()
    }
    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        padding: theme.spacing
        color: theme.foreground
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: (controller.live ? "Live" : "Frozen") + " · "
              + (controller.mode === "window" ? (controller.nativeWindowPicker ? "Use Pick window, then click a window"
                   : controller.desktopPicking ? "Click a window (topmost wins)" : "Window selected — confirm or pick another")
                 : controller.mode === "monitor" ? "Click a monitor" : "Drag to select, move or resize")
              + " · Enter to capture · Esc to cancel"
        background: Rectangle {
            color: theme.background
            radius: theme.radius
        }
    }
    Image {
        anchors.centerIn: parent
        width: parent.width * 0.65
        height: parent.height * 0.6
        visible: controller.mode === "window" && !controller.live && controller.selectedWindow.length > 0
                 && !controller.desktopPicking && !controller.busy && controller.error.length === 0
        source: visible ? "image://frames/window/" + controller.previewRevision : ""
        fillMode: Image.PreserveAspectFit
        cache: false
    }
    Control {
        id: toolbar
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: Math.min(parent.width - 24, content.implicitWidth + 2 * theme.spacing)
        height: content.implicitHeight + 2 * theme.spacing
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        palette.button: theme.background
        palette.base: theme.background
        palette.text: theme.foreground
        palette.windowText: theme.foreground
        palette.buttonText: theme.foreground
        palette.highlight: theme.accent
        palette.highlightedText: theme.background
        background: Rectangle {
            radius: theme.radius
            color: theme.background
        }
        // Consume clicks in toolbar gaps so they do not replace the selection.
        MouseArea { anchors.fill: parent }
        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: theme.spacing
            spacing: theme.spacing
            RowLayout {
                spacing: theme.spacing
                ComboBox {
                    model: ["Region", "Monitor", "Window"]
                    currentIndex: ["region", "monitor", "window"].indexOf(controller.mode)
                    enabled: !controller.busy && controller.error.length === 0
                    onActivated: controller.setMode(["region", "monitor", "window"][currentIndex])
                }
                ComboBox {
                    model: ["Frozen", "Live"]
                    currentIndex: controller.live ? 1 : 0
                    enabled: !controller.busy && controller.error.length === 0
                    onActivated: controller.setLive(currentIndex === 1)
                }
                Label {
                    text: controller.mode === "window" ? "Isolated window" : selection.valid
                          ? Math.round(selection.rect.width) + " × " + Math.round(selection.rect.height)
                          : "Drag to select"
                    color: theme.foreground
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    Layout.fillWidth: true
                }
                Button {
                    text: controller.nativeWindowPicker ? "Pick window" : "Pick another window"
                    visible: controller.mode === "window"
                    enabled: !controller.busy && controller.error.length === 0 && controller.windows.length > 0
                    onClicked: controller.startWindowPick()
                }
                Button {
                    text: controller.busy ? "Working…" : "Capture"
                    highlighted: true
                    enabled: controller.canConfirm
                    onClicked: controller.confirm()
                }
                Button {
                    text: "Cancel"
                    onClicked: controller.cancel()
                }
            }
            ComboBox {
                id: windowPicker
                objectName: "windowPicker"
                visible: controller.mode === "window"
                Layout.fillWidth: true
                Layout.maximumWidth: 640
                model: controller.windows
                textRole: "label"
                valueRole: "id"
                currentIndex: {
                    for (let i = 0; i < controller.windows.length; ++i)
                        if (controller.windows[i].id === controller.selectedWindow) return i
                    return -1
                }
                displayText: currentIndex < 0 ? "Choose a window (selection does not save)" : currentText
                enabled: !controller.busy && controller.error.length === 0
                onActivated: controller.selectWindow(currentValue)
                contentItem: Text {
                    text: windowPicker.displayText
                    textFormat: Text.PlainText
                    font: windowPicker.font
                    color: theme.foreground
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8
                    rightPadding: windowPicker.indicator.width + 8
                }
                delegate: ItemDelegate {
                    required property var modelData
                    width: windowPicker.width
                    contentItem: Text {
                        text: modelData.label
                        textFormat: Text.PlainText
                        font: windowPicker.font
                        color: theme.foreground
                        elide: Text.ElideRight
                    }
                }
            }
            Label {
                visible: controller.mode === "window" && (controller.windowUnavailable.length > 0 || controller.windows.length === 0)
                text: controller.windowUnavailable.length > 0 ? controller.windowUnavailable : "No capturable windows are currently mapped."
                color: theme.foreground
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                visible: controller.mode === "window" && controller.pickMessage.length > 0
                text: controller.pickMessage
                textFormat: Text.PlainText
                color: theme.foreground
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                visible: controller.error.length > 0
                text: controller.error
                color: theme.foreground
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }
}
