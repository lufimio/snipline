import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    readonly property var audioController: controller
    readonly property bool editingOptions: sessionSettings.visible || audioSelector.popup.visible
                                          || advancedMenu.visible || targetSelector.popup.visible
                                          || windowPicker.popup.visible || imageFormatSelector.popup.visible
                                          || videoFormatSelector.popup.visible
    focus: true
    clip: true
    property real sx: width / outputRect.width
    property real sy: height / outputRect.height
    property rect targetRect: controller.mode === "window" ? controller.windowRect : selection.rect
    property bool highlightValid: targetRect.width > 0 && targetRect.height > 0
    property real rx: (targetRect.x - outputRect.x) * sx
    property real ry: (targetRect.y - outputRect.y) * sy
    property real rw: targetRect.width * sx
    property real rh: targetRect.height * sy
    property color dimColor: controller.mode === "window" ? theme.windowDimColor
                             : controller.mode === "monitor" ? theme.monitorDimColor : theme.dimColor
    property real dimOpacity: controller.mode === "window" ? theme.windowDimOpacity
                              : controller.mode === "monitor" ? theme.monitorDimOpacity : theme.dimOpacity
    property rect hoverRect: controller.hoveredWindowRect
    property var clearRects: {
        let result = []
        if (highlightValid) result.push(Qt.rect(rx, ry, rw, rh))
        if (controller.mode === "window" && hoverRect.width > 0 && hoverRect.height > 0)
            result.push(Qt.rect((hoverRect.x - outputRect.x) * sx,
                                (hoverRect.y - outputRect.y) * sy,
                                hoverRect.width * sx, hoverRect.height * sy))
        return result
    }
    function dimmedRectangles() {
        // Subtract both clear areas. This also handles overlapping windows and
        // regions crossing output boundaries without double-dimming any pixel.
        let pieces = [Qt.rect(0, 0, width, height)]
        for (let hole of clearRects) {
            let next = []
            for (let piece of pieces) {
                let left = Math.max(piece.x, hole.x)
                let right = Math.min(piece.x + piece.width, hole.x + hole.width)
                let top = Math.max(piece.y, hole.y)
                let bottom = Math.min(piece.y + piece.height, hole.y + hole.height)
                if (right <= left || bottom <= top) { next.push(piece); continue }
                if (top > piece.y) next.push(Qt.rect(piece.x, piece.y, piece.width, top - piece.y))
                if (bottom < piece.y + piece.height)
                    next.push(Qt.rect(piece.x, bottom, piece.width, piece.y + piece.height - bottom))
                if (left > piece.x) next.push(Qt.rect(piece.x, top, left - piece.x, bottom - top))
                if (right < piece.x + piece.width)
                    next.push(Qt.rect(right, top, piece.x + piece.width - right, bottom - top))
            }
            pieces = next
        }
        return pieces
    }
    Shortcut {
        sequences: controller.keybindings.confirm
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.confirm()
    }
    Shortcut {
        sequences: controller.keybindings.cancel
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.cancel()
    }
    Shortcut {
        sequences: controller.keybindings.region
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.setMode("region")
    }
    Shortcut {
        sequences: controller.keybindings.monitor
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.setMode("monitor")
    }
    Shortcut {
        sequences: controller.keybindings.window
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.setMode("window")
    }

    Image {
        anchors.fill: parent
        source: controller.live || controller.mode === "window" ? ""
                : "image://frames/" + outputIndex + "?rev=" + controller.frameRevision
        cache: false
        fillMode: Image.Stretch
    }
    Item {
        anchors.fill: parent
        Repeater {
            model: root.dimmedRectangles()
            Rectangle {
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: modelData.width
                height: modelData.height
                color: root.dimColor
                opacity: root.dimOpacity
            }
        }
    }
    Rectangle {
        visible: controller.mode === "window" && root.hoverRect.width > 0 && root.hoverRect.height > 0
        x: (root.hoverRect.x - outputRect.x) * root.sx
        y: (root.hoverRect.y - outputRect.y) * root.sy
        width: root.hoverRect.width * root.sx
        height: root.hoverRect.height * root.sy
        color: "transparent"
        border.color: theme.accent
        border.width: theme.outlineWidth
        opacity: 0.6
    }
    Rectangle {
        x: root.rx
        y: root.ry
        width: Math.max(0, root.rw)
        height: Math.max(0, root.rh)
        visible: root.highlightValid
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
        enabled: controller.mode === "window" ? controller.canRetarget
                 : !controller.busy && controller.error.length === 0
        hoverEnabled: true
        cursorShape: controller.mode === "region" ? Qt.CrossCursor : Qt.PointingHandCursor
        onEntered: {
            if (controller.mode === "monitor")
                controller.selectMonitor(outputIndex)
        }
        onPressed: mouse => {
            root.forceActiveFocus()
            if (controller.mode === "monitor")
                controller.selectMonitor(outputIndex)
            else if (controller.mode === "region")
                selection.begin(outputRect.x + mouse.x / root.sx,
                                outputRect.y + mouse.y / root.sy)
            else if (controller.mode === "window")
                controller.pickWindowAt(outputRect.x + mouse.x / root.sx,
                                        outputRect.y + mouse.y / root.sy)
        }
        onPositionChanged: mouse => {
            if (controller.mode === "monitor")
                controller.selectMonitor(outputIndex)
            else if (controller.mode === "window")
                controller.hoverWindowAt(outputRect.x + mouse.x / root.sx,
                                         outputRect.y + mouse.y / root.sy)
            else if (pressed && controller.mode === "region")
                selection.update(outputRect.x + mouse.x / root.sx,
                                 outputRect.y + mouse.y / root.sy)
        }
        onExited: { if (controller) controller.clearWindowHover() }
        onReleased: selection.end()
        onCanceled: selection.end()
    }
    // The control surface never uses platform-native ComboBox/CheckBox/Button styling.
    // This keeps its appearance consistent across Qt styles and compositors.
    property bool optionsExpanded: false
    // UI-only until corresponding options are exposed by the C++ controller.
    property bool captureCursorVisible: false
    property bool recordCursorVisible: false
    property real toolbarX: Math.max(16, (width - 510) / 2)
    property real toolbarBottom: 14
    readonly property color panelColor: "#111214"
    readonly property color panelText: "#f1f1f2"
    readonly property color secondaryText: "#a5a8ae"
    readonly property color hoverColor: "#24262a"
    readonly property color selectedColor: "#2b2e34"

    component FlatCombo: ComboBox {
        id: combo
        implicitHeight: 30
        implicitWidth: 146
        hoverEnabled: true
        leftPadding: 0
        rightPadding: 0
        background: Rectangle {
            radius: 6
            color: combo.hovered || combo.popup.visible ? root.hoverColor : "transparent"
        }
        // Text-only settings selector: identical hit/hover area for every row.
        contentItem: Text {
            text: combo.displayText
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            rightPadding: 28
            font.pixelSize: 12
            color: combo.enabled ? root.panelText : root.secondaryText
            elide: Text.ElideRight
        }
        indicator: Glyph {
            x: combo.width - width - 8
            y: (combo.height - height) / 2
            width: 12; height: 12
            name: "chevron"
            color: root.secondaryText
            rotation: combo.popup.visible ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 140 } }
        }
        delegate: ItemDelegate {
            id: option
            required property int index
            width: combo.popup.contentItem ? combo.popup.contentItem.width : combo.width - 8
            height: 30
            text: combo.textAt(index)
            font.pixelSize: 12
            contentItem: Text {
                text: option.text
                color: root.panelText
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                leftPadding: 8
                rightPadding: 8
                elide: Text.ElideRight
                font.pixelSize: 12
            }
            background: Rectangle {
                radius: 5
                color: option.hovered || option.highlighted ? root.hoverColor : "transparent"
            }
        }
        popup: Popup {
            y: combo.height + 3
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + 8, 240)
            padding: 4
            background: Rectangle {
                color: "#1a1b1f"
                radius: 8
                border.width: 1
                border.color: "#36383d"
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.delegateModel
                currentIndex: combo.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator { }
            }
        }
    }

    component FlatCheck: CheckBox {
        id: check
        implicitHeight: 28
        spacing: 8
        font.pixelSize: 12
        indicator: Rectangle {
            implicitWidth: 15; implicitHeight: 15
            x: check.width - width - 5
            y: (check.height - height) / 2
            radius: 3
            color: check.checked ? root.panelText : "transparent"
            border.width: 1
            border.color: check.checked ? root.panelText : "#74777e"
            Glyph {
                anchors.centerIn: parent
                width: 12; height: 12
                visible: check.checked
                name: "check"
                color: "#121316"
            }
        }
        contentItem: Text {
            text: check.text
            color: root.secondaryText
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            rightPadding: 32
        }
    }

    component FlatSpin: SpinBox {
        id: spin
        implicitWidth: 90
        implicitHeight: 30
        editable: true
        font.pixelSize: 12
        background: Rectangle {
            radius: 5
            color: root.hoverColor
        }
        contentItem: TextInput {
            text: spin.textFromValue(spin.value, spin.locale)
            color: root.panelText
            font.pixelSize: 12
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            selectByMouse: true
            Keys.onShortcutOverride: event => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    event.accepted = true
            }
            onEditingFinished: {
                const next = spin.valueFromText(text, spin.locale)
                if (spin.value !== next) {
                    spin.value = next
                    spin.valueModified()
                }
            }
        }
        up.indicator: Rectangle {
            x: spin.width - width
            height: spin.height / 2
            width: 20
            color: "transparent"
            Text { anchors.centerIn: parent; text: "+"; color: root.secondaryText; font.pixelSize: 13 }
        }
        down.indicator: Rectangle {
            x: spin.width - width
            y: spin.height / 2
            height: spin.height / 2
            width: 20
            color: "transparent"
            Text { anchors.centerIn: parent; text: "−"; color: root.secondaryText; font.pixelSize: 13 }
        }
    }

    Control {
        id: toolbar
        objectName: "multiTierToolbar"
        x: Math.max(16, Math.min(root.width - width - 16, root.toolbarX))
        y: Math.max(8, Math.min(root.height - height - 8, root.height - height - root.toolbarBottom))
        width: Math.max(0, Math.min(510, root.width - 32))
        implicitHeight: content.implicitHeight + topPadding + bottomPadding
        height: Math.min(implicitHeight, root.height - 16)
        topPadding: 9; bottomPadding: 9
        leftPadding: 12; rightPadding: 12
        font.family: theme.fontFamily
        font.pixelSize: 12
        palette.button: root.panelColor
        palette.base: root.panelColor
        palette.text: root.panelText
        palette.windowText: root.panelText
        palette.buttonText: root.panelText
        palette.highlight: root.panelText
        palette.highlightedText: root.panelColor
        background: Rectangle {
            color: root.panelColor
            radius: 10
            border.width: 1
            border.color: "#34353a"
        }
        // Swallow clicks in the toolbar rather than passing them to the capture area.
        MouseArea { anchors.fill: parent; hoverEnabled: true; onEntered: controller.clearWindowHover() }

        ColumnLayout {
            id: content
            // A child anchored directly to Control ignores Control's padding.
            // Inset the layout itself, so content never touches the panel border.
            anchors.fill: parent
            anchors.leftMargin: toolbar.leftPadding
            anchors.rightMargin: toolbar.rightPadding
            anchors.topMargin: toolbar.topPadding
            anchors.bottomMargin: toolbar.bottomPadding
            spacing: 0
            ScrollView {
                id: optionsScroll
                visible: root.optionsExpanded
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: optionFields.implicitHeight
                Layout.minimumHeight: 0
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                id: optionFields
                objectName: "actionTier"
                width: optionsScroll.availableWidth
                spacing: 1
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Target"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatCombo {
                        id: targetSelector
                        objectName: "targetSelector"
                        Layout.preferredWidth: 146
                        model: ["Region", "Window", "Monitor"]
                        currentIndex: Math.max(0, ["region", "window", "monitor"].indexOf(controller.mode))
                        enabled: !controller.busy && controller.error.length === 0
                        onActivated: controller.setMode(["region", "window", "monitor"][currentIndex])
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label {
                        text: "Show cursor"
                        color: root.secondaryText; font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatCheck {
                        objectName: "showCursor"
                        Layout.preferredWidth: 40
                        text: ""
                        checked: controller.recording ? root.recordCursorVisible : root.captureCursorVisible
                        onToggled: {
                            if (controller.recording) root.recordCursorVisible = checked
                            else root.captureCursorVisible = checked
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "UI preference only — cursor capture is not connected to the backend"
                    }
                }
                RowLayout {
                    visible: controller.mode === "window"
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Window"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatCombo {
                        id: windowPicker
                        objectName: "windowPicker"
                        Layout.preferredWidth: 146
                        model: controller.windows
                        textRole: "label"; valueRole: "id"
                        currentIndex: {
                            for (let i = 0; i < controller.windows.length; ++i)
                                if (controller.windows[i].id === controller.selectedWindow) return i
                            return -1
                        }
                        displayText: currentIndex < 0 ? "Choose window" : currentText
                        enabled: !controller.busy && controller.error.length === 0
                        onActivated: controller.selectWindow(currentValue)
                    }
                }
                RowLayout {
                    visible: !controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Freeze screen"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatCheck {
                        objectName: "freezeScreen"
                        Layout.preferredWidth: 40
                        text: ""
                        checked: !controller.live
                        enabled: !controller.busy
                        onToggled: controller.setLive(!checked)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Format"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatCombo {
                        id: imageFormatSelector
                        objectName: "imageFormatSelector"
                        visible: !controller.recording
                        Layout.preferredWidth: 146
                        model: ["PNG", "JPEG", "WebP"]
                        currentIndex: Math.max(0, ["png", "jpeg", "webp"].indexOf(controller.imageFormat))
                        onActivated: controller.imageFormat = ["png", "jpeg", "webp"][currentIndex]
                    }
                    FlatCombo {
                        id: videoFormatSelector
                        objectName: "videoFormatSelector"
                        visible: controller.recording
                        Layout.preferredWidth: 146
                        model: ["MP4", "WebM", "GIF"]
                        currentIndex: Math.max(0, ["mp4", "webm", "gif"].indexOf(controller.recordingFormat))
                        onActivated: controller.recordingFormat = ["mp4", "webm", "gif"][currentIndex]
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Audio sources"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    AudioSelector {
                        id: audioSelector
                        objectName: "audioSourcesButton"
                        controller: root.audioController
                        Layout.preferredWidth: 146
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Frame rate"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatSpin {
                        objectName: "recordingFps"
                        from: 1; to: 120
                        value: controller.recordingFps
                        onValueModified: controller.recordingFps = value
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Delay (seconds)"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatSpin {
                        objectName: "recordingCountdown"
                        from: 0; to: 30
                        value: controller.countdownSeconds
                        onValueModified: controller.countdownSeconds = value
                    }
                }
                RowLayout {
                    visible: !controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Copy to clipboard"; color: root.secondaryText; font.pixelSize: 12; Layout.fillWidth: true }
                    FlatCheck {
                        objectName: "copyToClipboard"
                        Layout.preferredWidth: 40
                        text: ""
                        checked: controller.copyToClipboard
                        onToggled: controller.copyToClipboard = checked
                    }
                }
                Label {
                    visible: controller.mode === "window" && (controller.windowUnavailable.length > 0 || controller.windows.length === 0)
                    text: controller.windowUnavailable.length > 0 ? controller.windowUnavailable : "No capturable windows found"
                    color: root.secondaryText; font.pixelSize: 12
                    wrapMode: Text.Wrap; Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 1
                    Layout.topMargin: 7; Layout.bottomMargin: 6
                    color: "#292b2f"
                }
                }
            }
            RowLayout {
                id: modeRow
                objectName: "modeTier"
                readonly property bool compact: toolbar.width < 460
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                spacing: compact ? 2 : 4
                Item {
                    visible: !modeRow.compact
                    Layout.preferredWidth: 13
                    Layout.preferredHeight: 30
                    Glyph {
                        anchors.centerIn: parent; width: 12; height: 17
                        name: "grip"; color: root.secondaryText; opacity: 0.75
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.SizeAllCursor
                        drag.target: toolbar
                        drag.minimumX: 16
                        drag.maximumX: Math.max(16, root.width - toolbar.width - 16)
                        drag.minimumY: 8
                        drag.maximumY: Math.max(8, root.height - toolbar.height - 8)
                        onReleased: { root.toolbarX = toolbar.x; root.toolbarBottom = root.height - toolbar.y - toolbar.height }
                    }
                }
                IconButton {
                    objectName: "captureAction"
                    glyph: "capture"; tip: "Screenshot"
                    checkable: true; checked: controller.action === "capture"
                    autoExclusive: true
                    enabled: !controller.busy
                    onClicked: controller.setAction("capture")
                }
                IconButton {
                    objectName: "recordAction"
                    glyph: "record"; tip: "Record screen"
                    checkable: true; checked: controller.action === "record"
                    autoExclusive: true
                    enabled: !controller.busy
                    onClicked: controller.setAction("record")
                }
                IconButton {
                    objectName: "qrAction"
                    visible: !modeRow.compact
                    glyph: "qr"; tip: "Scan QR · not implemented"
                    enabled: false
                }
                IconButton {
                    objectName: "ocrAction"
                    visible: !modeRow.compact
                    glyph: "ocr"; tip: "OCR · not implemented"
                    enabled: false
                }
                Item { Layout.fillWidth: true; Layout.minimumWidth: 4 }
                IconButton {
                    objectName: "toggleOptions"
                    glyph: "settings"; tip: root.optionsExpanded ? "Hide options" : "Show options"
                    checkable: true; checked: root.optionsExpanded
                    onClicked: root.optionsExpanded = !root.optionsExpanded
                }
                IconButton {
                    objectName: "sessionSettingsButton"
                    glyph: "more"; tip: "Session settings"
                    onClicked: sessionSettings.open()
                }
                Rectangle {
                    Layout.preferredWidth: 1; Layout.preferredHeight: 18
                    Layout.leftMargin: 4; Layout.rightMargin: 3
                    color: "#323338"
                }
                Button {
                    objectName: "cancelAction"
                    text: "Cancel"
                    Layout.preferredWidth: modeRow.compact ? 29 : 52; Layout.preferredHeight: 29
                    hoverEnabled: true
                    background: Rectangle {
                        color: parent.hovered ? root.hoverColor : "transparent"
                        radius: 6
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Cancel"
                    contentItem: Item {
                        Text {
                            anchors.fill: parent
                            visible: !modeRow.compact
                            text: "Cancel"; color: root.secondaryText
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Glyph { anchors.centerIn: parent; visible: modeRow.compact; name: "close"; color: root.secondaryText; width: 17; height: 17 }
                    }
                    onClicked: controller.cancel()
                }
                RowLayout {
                    spacing: 0
                    Layout.preferredHeight: 30
                    Button {
                        id: confirmAction
                        objectName: "confirmAction"
                        text: controller.recording ? "Record" : "Capture"
                        enabled: controller.canConfirm
                        Layout.preferredWidth: modeRow.compact ? 34 : 75
                        Layout.preferredHeight: 29
                        hoverEnabled: true
                        background: Rectangle {
                            color: !confirmAction.enabled ? "#34363b" : confirmAction.hovered ? "#e1e2e5" : "#f0f0f2"
                            radius: 6
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: confirmAction.text
                        contentItem: Item {
                            Text {
                                anchors.fill: parent
                                visible: !modeRow.compact
                                text: confirmAction.text
                                color: confirmAction.enabled ? "#131416" : "#8f939b"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 12; font.weight: Font.DemiBold
                            }
                            Glyph {
                                anchors.centerIn: parent
                                visible: modeRow.compact
                                name: controller.recording ? "record" : "capture"
                                color: confirmAction.enabled ? "#131416" : "#8f939b"
                                width: 17; height: 17
                            }
                        }
                        onClicked: controller.confirm()
                    }
                    ToolButton {
                        id: advancedCapture
                        objectName: "advancedCapture"
                        visible: !controller.recording && !modeRow.compact
                        enabled: true
                        Layout.preferredWidth: 24; Layout.preferredHeight: 29
                        hoverEnabled: true
                        background: Rectangle {
                            color: !advancedCapture.enabled ? "#34363b" : advancedCapture.hovered ? "#e1e2e5" : "#f0f0f2"
                            radius: 6
                            Rectangle {
                                width: 1; height: 14
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                color: "#c9cbd0"
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 12; height: 12
                                name: "chevron"; color: "#131416"
                                rotation: advancedMenu.visible ? 180 : 0
                                Behavior on rotation { NumberAnimation { duration: 140 } }
                            }
                        }
                        onClicked: advancedMenu.open()
                        Popup {
                            id: advancedMenu
                            x: advancedCapture.width - width
                            y: -implicitHeight - 8
                            width: 194
                            implicitHeight: actionList.implicitHeight + 10
                            padding: 5
                            focus: true
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                            background: Rectangle {
                                color: "#1a1b1f"
                                radius: 9
                                border.width: 1
                                border.color: "#37393e"
                            }
                            contentItem: ColumnLayout {
                                id: actionList
                                spacing: 1
                                Repeater {
                                    model: [
                                        { label: "Capture", icon: "capture", ready: true },
                                        { label: "Save", icon: "image", ready: false },
                                        { label: "Copy", icon: "check", ready: false },
                                        { label: "Annotate", icon: "ocr", ready: false },
                                        { label: "Open folder", icon: "window", ready: false },
                                        { label: "Open picture", icon: "image", ready: false }
                                    ]
                                    delegate: ItemDelegate {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 30
                                        enabled: modelData.ready && controller.canConfirm
                                        hoverEnabled: true
                                        onClicked: {
                                            advancedMenu.close()
                                            if (modelData.label === "Capture") controller.confirm()
                                        }
                                        background: Rectangle {
                                            radius: 5
                                            color: parent.hovered && parent.enabled ? root.hoverColor : "transparent"
                                        }
                                        contentItem: RowLayout {
                                            spacing: 10
                                            Glyph {
                                                name: modelData.icon
                                                color: root.panelText
                                                width: 15; height: 15
                                                opacity: parent.parent.enabled ? 1 : 0.42
                                            }
                                            Text {
                                                text: modelData.label
                                                color: root.panelText
                                                opacity: parent.parent.enabled ? 1 : 0.42
                                                font.pixelSize: 12
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Label {
                visible: controller.pickMessage.length > 0 || controller.error.length > 0
                text: controller.error.length ? controller.error : controller.pickMessage
                color: root.panelText; font.pixelSize: 12
                wrapMode: Text.Wrap; Layout.fillWidth: true
            }
        }
    }
    // Session settings use the same flat controls and spacing as the toolbar.
    Popup {
        id: sessionSettings
        objectName: "sessionSettings"
        parent: Overlay.overlay
        width: Math.max(0, Math.min(510, root.width - 32))
        height: Math.max(0, Math.min(root.height - 24, settingsFields.implicitHeight + topPadding + bottomPadding))
        x: Math.max(12, (root.width - width) / 2)
        y: Math.max(12, Math.min(root.height - height - 12, toolbar.y - height - 8))
        modal: true
        focus: true
        dim: false
        leftPadding: 14
        rightPadding: 14
        topPadding: 12
        bottomPadding: 12
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: {
            savePath.text = controller.saveDirectory
            nameTemplate.text = controller.filenameTemplate
        }
        background: Rectangle {
            color: root.panelColor
            radius: 10
            border.width: 1
            border.color: "#34353a"
        }
        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: settingsFields
                width: sessionSettings.availableWidth
                spacing: 5
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    Text {
                        text: "Session settings"
                        font.pixelSize: 13
                        font.bold: true
                        color: root.panelText
                        Layout.fillWidth: true
                        verticalAlignment: Text.AlignVCenter
                    }
                    Button {
                        id: closeSessionSettings
                        implicitWidth: 28
                        implicitHeight: 28
                        hoverEnabled: true
                        onClicked: sessionSettings.close()
                        ToolTip.visible: hovered
                        ToolTip.text: "Close settings"
                        background: Rectangle {
                            radius: 6
                            color: closeSessionSettings.hovered ? root.hoverColor : "transparent"
                        }
                        contentItem: Text {
                            text: "×"
                            color: root.secondaryText
                            font.pixelSize: 19
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                Text {
                    text: "Changes apply to this session. Edit config.json to make them permanent."
                    color: root.secondaryText
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 9
                }
                Text {
                    text: "Save directory"
                    color: root.secondaryText
                    font.pixelSize: 12
                    Layout.topMargin: 3
                }
                TextField {
                    id: savePath
                    objectName: "saveDirectoryField"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    selectByMouse: true
                    enabled: !controller.busy
                    font.pixelSize: 12
                    color: root.panelText
                    selectionColor: "#50545d"
                    selectedTextColor: root.panelText
                    leftPadding: 10
                    rightPadding: 10
                    onEditingFinished: {
                        controller.saveDirectory = text
                        text = controller.saveDirectory
                    }
                    background: Rectangle {
                        radius: 6
                        color: "#1b1d21"
                        border.width: 1
                        border.color: savePath.activeFocus ? "#696d76" : "#34363b"
                    }
                }
                Text {
                    text: "Filename template"
                    color: root.secondaryText
                    font.pixelSize: 12
                    Layout.topMargin: 10
                }
                TextField {
                    id: nameTemplate
                    objectName: "filenameTemplateField"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    selectByMouse: true
                    enabled: !controller.busy
                    font.pixelSize: 12
                    color: root.panelText
                    selectionColor: "#50545d"
                    selectedTextColor: root.panelText
                    leftPadding: 10
                    rightPadding: 10
                    onEditingFinished: {
                        controller.filenameTemplate = text
                        text = controller.filenameTemplate
                    }
                    background: Rectangle {
                        radius: 6
                        color: "#1b1d21"
                        border.width: 1
                        border.color: nameTemplate.activeFocus ? "#696d76" : "#34363b"
                    }
                }
                Text {
                    text: "File extension is added automatically"
                    color: root.secondaryText
                    opacity: 0.8
                    font.pixelSize: 11
                    Layout.topMargin: 1
                    Layout.bottomMargin: 7
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.topMargin: 5
                    Layout.bottomMargin: 6
                    color: "#292b2f"
                }
                RowLayout {
                    visible: !controller.recording
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Copy to clipboard"
                        color: root.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatCheck {
                        Layout.preferredWidth: 40
                        text: ""
                        checked: controller.copyToClipboard
                        enabled: !controller.busy
                        onToggled: controller.copyToClipboard = checked
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Show notifications"
                        color: root.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatCheck {
                        Layout.preferredWidth: 40
                        text: ""
                        checked: controller.notificationsEnabled
                        enabled: !controller.busy
                        onToggled: controller.notificationsEnabled = checked
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Frame rate"
                        color: root.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatSpin {
                        objectName: "recordingFps"
                        from: 1; to: 120
                        value: controller.recordingFps
                        enabled: !controller.busy
                        onValueModified: controller.recordingFps = value
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Delay (seconds)"
                        color: root.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }
                    FlatSpin {
                        objectName: "recordingCountdown"
                        from: 0; to: 30
                        value: controller.countdownSeconds
                        enabled: !controller.busy
                        onValueModified: controller.countdownSeconds = value
                    }
                }
                Text {
                    visible: controller.pickMessage.length > 0
                    text: controller.pickMessage
                    textFormat: Text.PlainText
                    color: root.secondaryText
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.topMargin: 8
                    Layout.bottomMargin: 4
                    color: "#292b2f"
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Item { Layout.fillWidth: true }
                    Button {
                        id: doneSettings
                        text: "Done"
                        implicitWidth: 74
                        implicitHeight: 30
                        hoverEnabled: true
                        onClicked: {
                            // Commit pending text edits before closing the panel.
                            if (controller.saveDirectory !== savePath.text)
                                controller.saveDirectory = savePath.text
                            if (controller.filenameTemplate !== nameTemplate.text)
                                controller.filenameTemplate = nameTemplate.text
                            sessionSettings.close()
                        }
                        background: Rectangle {
                            radius: 6
                            color: doneSettings.down ? "#d1d3d6" : doneSettings.hovered ? "#ffffff" : "#e7e8ea"
                        }
                        contentItem: Text {
                            text: doneSettings.text
                            color: "#16171a"
                            font.pixelSize: 12
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }
    }
}
