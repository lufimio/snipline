import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ToolButton {
    id: control
    property var recognition
    property bool locked: false
    property alias popup: dropdown
    implicitWidth: 146
    implicitHeight: 29
    hoverEnabled: true
    Accessible.name: "OCR languages: " + recognition.selectedLanguages.join(", ")
    ToolTip.visible: hovered
    ToolTip.delay: 400
    ToolTip.text: recognition.selectedLanguages.length ? recognition.selectedLanguages.join(" + ") : "Select OCR languages"
    onClicked: dropdown.visible ? dropdown.close() : dropdown.open()
    background: Rectangle {
        radius: 6
        color: control.hovered || dropdown.visible ? theme.hoverColor : "transparent"
    }
    contentItem: Item {
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 28
            text: control.recognition.selectedLanguages.length + " selected"
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
        objectName: "ocrLanguagesPopup"
        // Match AudioSelector: position the panel in window-local coordinates,
        // independently of Qt's popup flipping/clamping.
        parent: control.Window.window ? control.Window.window.contentItem : control
        x: 0; y: 0
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
            if (!parent) return
            const anchor = control.mapToItem(parent, 0, 0)
            const above = Math.max(0, anchor.y - edgeInset - anchorGap)
            const below = Math.max(0, height - anchor.y - control.height - edgeInset - anchorGap)
            const useAbove = above >= below
            const available = useAbove ? above : below
            panelWidth = Math.min(desiredWidth, Math.max(0, width - 2 * edgeInset))
            panelHeight = Math.min(desiredHeight, available)
            panelX = Math.max(edgeInset, Math.min(width - edgeInset - panelWidth,
                anchor.x + control.width - panelWidth))
            panelY = useAbove ? anchor.y - anchorGap - panelHeight : anchor.y + control.height + anchorGap
        }
        onAboutToShow: placePanel()
        onOpened: placePanel()
        contentItem: Item {
            MouseArea { anchors.fill: parent; onClicked: dropdown.close() }
            Rectangle {
                x: dropdown.panelX; y: dropdown.panelY
                width: dropdown.panelWidth; height: dropdown.panelHeight
                radius: 8
                color: theme.background
                border.color: theme.borderColor
                border.width: 1
                clip: true
                MouseArea { anchors.fill: parent }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8
                    RowLayout {
                        TextField {
                            id: search
                            objectName: "ocrLanguageSearch"
                            Layout.fillWidth: true
                            placeholderText: "Search installed languages"
                            selectByMouse: true
                        }
                        IconButton {
                            glyph: "refresh"; tip: "Refresh OCR languages"
                            enabled: !control.recognition.busy && !control.locked
                            onClicked: control.recognition.refreshLanguages()
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: control.recognition.error.length > 0 || control.recognition.busy
                        text: control.recognition.busy ? "Loading languages…" : control.recognition.error
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: control.palette.text
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: control.recognition.languages
                        ScrollBar.vertical: ScrollBar {}
                        delegate: CheckDelegate {
                            required property string modelData
                            objectName: "ocrLanguage_" + modelData
                            width: ListView.view.width
                            visible: search.text.length === 0 || modelData.toLowerCase().includes(search.text.toLowerCase())
                            height: visible ? 46 : 0
                            checked: control.recognition.selectedLanguages.indexOf(modelData) >= 0
                            enabled: !control.recognition.busy && !control.locked
                            onClicked: control.recognition.toggleLanguage(modelData)
                            contentItem: RowLayout {
                                Glyph {
                                    name: "ocr"
                                    color: control.palette.text
                                    width: 20; height: 20
                                }
                                Text {
                                    text: modelData
                                    textFormat: Text.PlainText
                                    color: control.palette.text
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    rightPadding: 26
                                }
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: modelData
                        }
                    }
                }
            }
        }
    }
}
