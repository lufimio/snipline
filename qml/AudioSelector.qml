import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ToolButton {
    id: control
    property var controller
    implicitWidth: 146
    implicitHeight: 29
    hoverEnabled: true
    Accessible.name: "Audio sources: " + controller.selectedAudioCount + " selected"
    ToolTip.visible: hovered
    ToolTip.delay: 400
    ToolTip.text: "Audio sources · " + controller.selectedAudioCount + " selected"
                  + (controller.audioMessage.length > 0 ? "\n" + controller.audioMessage : "")
    onClicked: {
        if (dropdown.visible) {
            dropdown.close()
        } else {
            dropdown.open()
        }
    }
    background: Rectangle {
        radius: 6
        color: control.hovered || dropdown.visible ? theme.hoverColor : "transparent"
    }
    // Same 146px hover surface and text/chevron alignment as FlatCombo.
    contentItem: Item {
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 28
            text: control.controller.selectedAudioCount + " selected"
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            color: theme.foreground
            font.pixelSize: theme.fontSize
            elide: Text.ElideRight
        }
        Glyph {
            objectName: "dropdownArrow"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 12; height: 12
            name: "chevron"
            color: theme.secondaryText
            rotation: dropdown.visible ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 140 } }
        }
    }
    Popup {
        id: dropdown
        objectName: "audioDropdown"

        // A window-sized, transparent popup prevents Qt Quick Controls from
        // flipping or clamping the *whole* audio panel below its trigger.
        // Only the panel below is moved, using window-local coordinates.
        parent: control.Window.window ? control.Window.window.contentItem : control
        x: 0
        y: 0
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        margins: 0
        padding: 0
        focus: true
        modal: true
        dim: false
        closePolicy: Popup.CloseOnEscape
        background: Item {}

        property real edgeInset: 12
        property real anchorGap: 8
        property real desiredWidth: 520
        property real desiredHeight: 480
        property real panelX: 12
        property real panelY: 12
        property real panelWidth: 520
        property real panelHeight: 480

        function placePanel() {
            if (!parent)
                return
            // Both coordinates and available space are measured in the same
            // window-local coordinate system, not relative to the button.
            const anchor = control.mapToItem(parent, 0, 0)
            const boundsW = width
            const boundsH = height
            const above = Math.max(0, anchor.y - edgeInset - anchorGap)
            const below = Math.max(0, boundsH - anchor.y - control.height
                                   - edgeInset - anchorGap)
            const useAbove = above >= below
            const available = useAbove ? above : below

            panelWidth = Math.min(desiredWidth, Math.max(0, boundsW - 2 * edgeInset))
            panelHeight = Math.min(desiredHeight, available)
            panelX = Math.max(edgeInset, Math.min(boundsW - edgeInset - panelWidth,
                         anchor.x + control.width - panelWidth))
            panelY = useAbove ? anchor.y - anchorGap - panelHeight
                              : anchor.y + control.height + anchorGap
        }

        onAboutToShow: placePanel()
        onOpened: {
            placePanel()
            control.controller.refreshAudioSources()
        }

        contentItem: Item {
            // Clicking outside the actual panel dismisses the popup.
            MouseArea {
                anchors.fill: parent
                onClicked: dropdown.close()
            }

            Rectangle {
                id: panel
                x: dropdown.panelX
                y: dropdown.panelY
                width: dropdown.panelWidth
                height: dropdown.panelHeight
                radius: 8
                color: theme.background
                border.color: theme.borderColor
                border.width: 1
                clip: true

                // Prevent background dismiss interaction within the panel.
                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        TextField {
                            id: search
                            objectName: "audioSearch"
                            Layout.fillWidth: true
                            placeholderText: "Search apps and devices"
                            selectByMouse: true
                        }
                        IconButton {
                            glyph: "refresh"
                            tip: "Refresh audio sources"
                            enabled: !control.controller.audioLoading
                            onClicked: control.controller.refreshAudioSources()
                        }
                    }
                    Label {
                        color: control.palette.text
                        visible: control.controller.audioMessage.length > 0
                        text: control.controller.audioMessage
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    ListView {
                        id: sourceList
                        objectName: "audioSourceList"
                        property real rememberedY: 0
                        property bool restoring: false
                        function restoreScroll() {
                            if (!restoring) return
                            contentY = Math.min(rememberedY,
                                Math.max(0, contentHeight - height))
                            restoring = false
                        }
                        onModelChanged: {
                            if (restoring) Qt.callLater(restoreScroll)
                        }
                        onCountChanged: {
                            if (restoring) Qt.callLater(restoreScroll)
                        }
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: control.controller.audioSources
                        ScrollBar.vertical: ScrollBar {}
                        delegate: CheckDelegate {
                            required property var modelData
                            objectName: "audioChoice_" + modelData.id
                            width: ListView.view.width
                            visible: search.text.length === 0 || modelData.label.toLowerCase().includes(search.text.toLowerCase())
                            height: visible ? 46 : 0
                            checked: modelData.selected
                            Accessible.name: modelData.label
                            onClicked: {
                                sourceList.rememberedY = sourceList.contentY
                                sourceList.restoring = true
                                control.controller.toggleAudioSource(modelData.id)
                                Qt.callLater(sourceList.restoreScroll)
                            }
                            contentItem: RowLayout {
                                Glyph {
                                    name: modelData.icon === "mic" ? "audio" : (modelData.icon || "audio")
                                    color: control.palette.text
                                    width: 20; height: 20
                                }
                                Text {
                                    text: modelData.label
                                    textFormat: Text.PlainText
                                    color: control.palette.text
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    rightPadding: 26
                                }
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.label
                        }
                    }
                }
            }
        }
    }
    property alias popup: dropdown
}
