import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    readonly property var audioController: controller
    readonly property string statusMessage: controller.error.length ? controller.error : controller.pickMessage
    function showStatusMessage() {
        if (!errorToast) return
        errorToastTimer.stop()
        if (!statusMessage.length) {
            errorToast.opacity = 0
            return
        }
        errorToast.message = statusMessage
        errorToast.opacity = 1
        errorToastTimer.restart()
    }
    onStatusMessageChanged: showStatusMessage()
    function canUseResultShortcut(action) {
        if (root.editingOptions) return false
        if (action === "copy" && controller.action !== "capture") return false
        if (root.selecting)
            return (controller.action === "capture" || controller.action === "record") && controller.canConfirm
        return controller.recordingControls && !controller.pausePending
                && (controller.recordingState === "Recording" || controller.recordingState === "Paused")
    }
    function useResultShortcut(action) {
        if (!canUseResultShortcut(action)) return
        advancedMenu.close()
        if (root.selecting) controller.confirmWithAction(action)
        else controller.stopWithAction(action)
    }
    function resultShortcut(action) {
        const keys = action === "capture" ? controller.keybindings.confirm : controller.keybindings[action]
        return keys && keys.length ? keys[0] : ""
    }
    Component.onCompleted: showStatusMessage()
    readonly property bool selecting: !controller.recording || controller.recordingState === "Selecting"
    function recordingElapsedText() {
        const seconds = Math.floor(controller.recordingElapsedMs / 1000)
        return Math.floor(seconds / 3600).toString().padStart(2, "0") + ":"
                + Math.floor(seconds / 60 % 60).toString().padStart(2, "0") + ":"
                + (seconds % 60).toString().padStart(2, "0")
    }
    readonly property bool editingOptions: sessionSettings.visible || audioSelector.popup.visible || ocrLanguageSelector.popup.visible
                                          || advancedMenu.visible || targetSelector.popup.visible
                                          || windowPicker.popup.visible || imageFormatSelector.popup.visible
                                          || videoFormatSelector.popup.visible || videoCodecSelector.popup.visible
                                          || videoEncoderSelector.popup.visible
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
        enabled: !root.editingOptions && root.selecting
        context: Qt.WindowShortcut
        onActivated: controller.confirm()
    }
    Shortcut {
        sequences: controller.keybindings.cancel
        enabled: !root.editingOptions
        context: Qt.WindowShortcut
        onActivated: controller.recordingControls ? controller.hideRecordingControls() : controller.cancel()
    }
    Shortcut {
        sequences: controller.keybindings.save
        enabled: root.canUseResultShortcut("save")
        context: Qt.WindowShortcut
        onActivated: root.useResultShortcut("save")
    }
    Shortcut {
        sequences: controller.keybindings.copy
        enabled: root.canUseResultShortcut("copy")
        context: Qt.WindowShortcut
        onActivated: root.useResultShortcut("copy")
    }
    Shortcut {
        sequences: controller.keybindings.open
        enabled: root.canUseResultShortcut("open")
        context: Qt.WindowShortcut
        onActivated: root.useResultShortcut("open")
    }
    Shortcut {
        sequences: controller.keybindings.folder
        enabled: root.canUseResultShortcut("folder")
        context: Qt.WindowShortcut
        onActivated: root.useResultShortcut("folder")
    }
    Shortcut {
        sequences: controller.keybindings.edit
        enabled: root.canUseResultShortcut("edit")
        context: Qt.WindowShortcut
        onActivated: root.useResultShortcut("edit")
    }
    Shortcut {
        sequences: controller.keybindings.cursor
        enabled: !root.editingOptions && root.selecting && !controller.busy
                 && (controller.action === "capture" || controller.action === "record")
        context: Qt.WindowShortcut
        onActivated: {
            if (controller.recording) root.recordCursorVisible = !root.recordCursorVisible
            else root.captureCursorVisible = !root.captureCursorVisible
        }
    }
    Shortcut {
        sequences: controller.keybindings.live
        enabled: !root.editingOptions && root.selecting && !controller.recording && !controller.busy
        context: Qt.WindowShortcut
        onActivated: controller.setLive(!controller.live)
    }
    Shortcut {
        sequences: controller.keybindings.capture
        enabled: !root.editingOptions && root.selecting && !controller.busy
        context: Qt.WindowShortcut
        onActivated: controller.setAction("capture")
    }
    Shortcut {
        sequences: controller.keybindings.record
        enabled: !root.editingOptions && root.selecting && !controller.busy
        context: Qt.WindowShortcut
        onActivated: controller.setAction("record")
    }
    Shortcut {
        sequences: controller.keybindings.qr
        enabled: !root.editingOptions && root.selecting && !controller.busy
        context: Qt.WindowShortcut
        onActivated: controller.setAction("qr")
    }
    Shortcut {
        sequences: controller.keybindings.ocr
        enabled: !root.editingOptions && root.selecting && !controller.busy
        context: Qt.WindowShortcut
        onActivated: controller.setAction("ocr")
    }
    Shortcut {
        sequences: controller.keybindings.region
        enabled: !root.editingOptions && root.selecting
        context: Qt.WindowShortcut
        onActivated: controller.setMode("region")
    }
    Shortcut {
        sequences: controller.keybindings.monitor
        enabled: !root.editingOptions && root.selecting
        context: Qt.WindowShortcut
        onActivated: controller.setMode("monitor")
    }
    Shortcut {
        sequences: controller.keybindings.window
        enabled: !root.editingOptions && root.selecting
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
        objectName: "selectionDimming"
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
        visible: root.selecting && controller.mode === "window" && root.hoverRect.width > 0 && root.hoverRect.height > 0
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
                visible: root.selecting && controller.mode === "region"
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.selecting && (controller.mode === "window" ? controller.canRetarget
                 : !controller.busy && controller.error.length === 0)
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
    Timer {
        id: errorToastTimer
        interval: 5000
        onTriggered: errorToast.opacity = 0
    }
    Rectangle {
        id: errorToast
        objectName: "errorToast"
        property string message: ""
        readonly property Item bar: controller.recordingControls ? recordingToolbar : toolbar
        x: bar.x
        y: Math.max(8, bar.y - height - 8)
        width: bar.width
        height: errorToastText.implicitHeight + 20
        z: 10
        radius: theme.radius
        color: theme.popupColor
        border.color: theme.borderColor
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 250 } }
        Text {
            id: errorToastText
            objectName: "errorToastText"
            x: 12; y: 10
            width: Math.max(0, parent.width - 24)
            text: errorToast.message
            textFormat: Text.PlainText
            color: theme.foreground
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            wrapMode: Text.Wrap
        }
    }
    Rectangle {
        objectName: "recordingCountdown"
        x: root.rx + (root.rw - width) / 2
        y: root.ry + (root.rh - height) / 2
        width: 144; height: 144; radius: 72
        color: root.panelColor
        visible: controller.recording && controller.recordingState === "Countdown"
                 && root.highlightValid
                 && root.rx + root.rw > 0 && root.ry + root.rh > 0 && root.rx < root.width && root.ry < root.height
        Text {
            objectName: "countdownNumber"
            anchors.centerIn: parent
            text: controller.countdownRemaining
            color: root.panelText
            font.pixelSize: 80
            font.family: theme.fontFamily
            font.bold: true
        }
    }
    Rectangle {
        id: recordingToolbar
        objectName: "recordingControls"
        visible: controller.recordingControls
        x: Math.max(16, Math.min(root.width - width - 16, root.toolbarX))
        y: Math.max(8, Math.min(root.height - height - 8, root.height - height - root.toolbarBottom))
        width: Math.min(parent.width - 32, 320)
        height: 47
        radius: theme.radius + 2; color: root.panelColor
        border.width: 1
        border.color: theme.borderColor
        MouseArea { anchors.fill: parent }
        RowLayout {
            id: recordingControlFields
            anchors.fill: parent
            anchors.leftMargin: 12; anchors.rightMargin: 12
            anchors.topMargin: 9; anchors.bottomMargin: 9
            spacing: 4
            Item {
                Layout.preferredWidth: 18; Layout.fillHeight: true
                Glyph {
                    anchors.centerIn: parent
                    name: "grip"; color: root.secondaryText
                    width: 12; height: 17
                }
                DragHandler {
                    target: null
                    property real startX: 0
                    property real startBottom: 0
                    onActiveChanged: if (active) {
                        startX = recordingToolbar.x
                        startBottom = root.height - recordingToolbar.y - recordingToolbar.height
                    }
                    onTranslationChanged: if (active) {
                        root.toolbarX = startX + translation.x
                        root.toolbarBottom = startBottom - translation.y
                    }
                }
            }
            Text {
                objectName: "recordingElapsed"
                Layout.fillWidth: true
                text: controller.recordingState === "Finalizing" ? "Saving recording…"
                      : (controller.recordingState === "Paused" ? "Paused · " : "Recording · ") + root.recordingElapsedText()
                color: root.panelText
                font.pixelSize: theme.fontSize
                font.family: theme.fontFamily
                elide: Text.ElideRight
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: theme.separatorColor }
                IconButton {
                    objectName: "recordingPauseResume"
                    glyph: controller.recordingState === "Paused" ? "play" : "pause"
                    tip: controller.recordingState === "Paused" ? "Resume recording" : "Pause recording"
                    enabled: !controller.pausePending &&
                             (controller.recordingState === "Paused" || controller.recordingState === "Recording")
                    onClicked: controller.TogglePause()
                }
                IconButton {
                    objectName: "recordingStop"
                    glyph: "stop"; tip: "Finish recording"
                    enabled: controller.recordingState === "Paused" || controller.recordingState === "Recording"
                    onClicked: controller.stopWithAction(controller.recordingResultAction)
                }
                IconButton {
                    objectName: "recordingDiscard"
                    glyph: "discard"; tip: "Discard recording"
                    onClicked: controller.cancel()
                }
                IconButton {
                    objectName: "recordingHideControls"
                    glyph: "close"; tip: "Hide controls (keep recording)"
                    onClicked: controller.hideRecordingControls()
                }
        }
    }
    readonly property color panelColor: theme.background
    readonly property color panelText: theme.foreground
    readonly property color secondaryText: theme.secondaryText
    readonly property color hoverColor: theme.hoverColor
    readonly property color selectedColor: theme.selectedColor
    component ResultMenu: Popup {
        id: resultMenu
        property bool video: false
        property bool stopping: false
        width: 230
        implicitHeight: resultActions.implicitHeight + 10
        padding: 5
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.popupColor; radius: theme.radius + 1
            border.width: 1; border.color: theme.borderColor
        }
        contentItem: ColumnLayout {
            id: resultActions
            spacing: 1
            Repeater {
                model: resultMenu.video ? [
                    { label: resultMenu.stopping ? "Save recording" : "Record", icon: "record", action: "save" },
                    { label: "Upload", icon: "upload", action: "upload" },
                    { label: "Edit", icon: "ocr", action: "edit" },
                    { label: "Open folder", icon: "window", action: "folder" },
                    { label: "Open video", icon: "record", action: "open" },
                ] : [
                    { label: "Capture", icon: "capture", action: "capture" },
                    { label: "Save", icon: "image", action: "save" },
                    { label: "Copy", icon: "check", action: "copy" },
                    { label: "Upload", icon: "upload", action: "upload" },
                    { label: "Edit", icon: "ocr", action: "edit" },
                    { label: "Open folder", icon: "window", action: "folder" },
                    { label: "Open picture", icon: "image", action: "open" },
                ]
                delegate: ItemDelegate {
                    id: resultAction
                    required property var modelData
                    objectName: "resultAction_" + modelData.action
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    leftPadding: 10; rightPadding: 10
                    topPadding: 0; bottomPadding: 0
                    enabled: modelData.action !== "upload" && (resultMenu.stopping
                             ? !controller.pausePending && (controller.recordingState === "Paused" || controller.recordingState === "Recording")
                             : controller.canConfirm)
                    hoverEnabled: true
                    onClicked: {
                        resultMenu.close()
                        if (resultMenu.stopping) controller.stopWithAction(modelData.action)
                        else controller.confirmWithAction(modelData.action)
                    }
                    background: Rectangle {
                        radius: 5
                        color: resultAction.hovered && resultAction.enabled ? theme.hoverColor : "transparent"
                    }
                    contentItem: Item {
                        opacity: resultAction.enabled ? 1 : 0.42
                        Glyph {
                            objectName: "resultItemIcon"
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            name: resultAction.modelData.icon
                            color: theme.foreground
                            width: 15; height: 15
                        }
                        Text {
                            objectName: "resultItemLabel"
                            anchors.left: parent.left
                            anchors.leftMargin: 25
                            anchors.right: resultItemShortcut.left
                            anchors.rightMargin: resultItemShortcut.text.length ? 10 : 0
                            anchors.verticalCenter: parent.verticalCenter
                            text: resultAction.modelData.label; color: theme.foreground
                            font.pixelSize: theme.fontSize; font.family: theme.fontFamily
                            elide: Text.ElideRight
                        }
                        Text {
                            id: resultItemShortcut
                            objectName: "resultItemShortcut"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.resultShortcut(resultAction.modelData.action)
                            color: theme.secondaryText
                            font.pixelSize: theme.fontSize - 1
                            font.family: theme.fontFamily
                        }
                    }
                }
            }
        }
    }

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
            font.pixelSize: theme.fontSize
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
            font.pixelSize: theme.fontSize
            contentItem: Text {
                text: option.text
                color: root.panelText
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                leftPadding: 8
                rightPadding: 8
                elide: Text.ElideRight
                font.pixelSize: theme.fontSize
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
                color: theme.popupColor
                radius: 8
                border.width: 1
                border.color: theme.borderColor
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
        font.pixelSize: theme.fontSize
        indicator: Rectangle {
            implicitWidth: 15; implicitHeight: 15
            x: check.width - width - 5
            y: (check.height - height) / 2
            radius: 3
            color: check.checked ? root.panelText : "transparent"
            border.width: 1
            border.color: check.checked ? root.panelText : theme.mutedBorder
            Glyph {
                anchors.centerIn: parent
                width: 12; height: 12
                visible: check.checked
                name: "check"
                color: theme.buttonText
            }
        }
        contentItem: Text {
            text: check.text
            color: root.secondaryText
            font.pixelSize: theme.fontSize
            verticalAlignment: Text.AlignVCenter
            rightPadding: 32
        }
    }

    component FlatSpin: SpinBox {
        id: spin
        implicitWidth: 90
        implicitHeight: 30
        editable: true
        font.pixelSize: theme.fontSize
        background: Rectangle {
            radius: 5
            color: root.hoverColor
        }
        contentItem: TextInput {
            text: spin.textFromValue(spin.value, spin.locale)
            color: root.panelText
            font.pixelSize: theme.fontSize
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
        visible: root.selecting
        objectName: "multiTierToolbar"
        x: Math.max(16, Math.min(root.width - width - 16, root.toolbarX))
        y: Math.max(8, Math.min(root.height - height - 8, root.height - height - root.toolbarBottom))
        width: Math.max(0, Math.min(510, root.width - 32))
        implicitHeight: content.implicitHeight + topPadding + bottomPadding
        height: Math.min(implicitHeight, root.height - 16)
        topPadding: 9; bottomPadding: 9
        leftPadding: 12; rightPadding: 12
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        palette.button: root.panelColor
        palette.base: root.panelColor
        palette.text: root.panelText
        palette.windowText: root.panelText
        palette.buttonText: root.panelText
        palette.highlight: root.panelText
        palette.highlightedText: root.panelColor
        background: Rectangle {
            color: root.panelColor
            radius: theme.radius + 2
            border.width: 1
            border.color: theme.borderColor
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
                    visible: controller.action === "ocr"
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "OCR languages"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
                    LanguageSelector {
                        id: ocrLanguageSelector
                        objectName: "ocrLanguageSelector"
                        recognition: controller.recognition
                        locked: controller.busy
                        enabled: !controller.busy
                        Layout.preferredWidth: 146
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Target"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    visible: controller.action === "capture" || controller.action === "record"
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label {
                        text: "Show cursor"
                        color: root.secondaryText; font.pixelSize: theme.fontSize
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
                    Label { text: "Window"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    Label { text: "Freeze screen"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    visible: controller.action === "capture" || controller.action === "record"
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Format"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    Label { text: "Video codec"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
                    FlatCombo {
                        id: videoCodecSelector
                        objectName: "videoCodecSelector"
                        Layout.preferredWidth: 146
                        model: controller.recordingCodecs
                        displayText: ({"auto": "Automatic", "h264": "H.264", "h265": "H.265 / HEVC",
                                       "vp8": "VP8", "vp9": "VP9", "av1": "AV1"})[currentValue] || "Automatic"
                        currentIndex: Math.max(0, controller.recordingCodecs.indexOf(controller.recordingCodec))
                        onActivated: controller.recordingCodec = controller.recordingCodecs[currentIndex]
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Video encoder"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
                    FlatCombo {
                        id: videoEncoderSelector
                        objectName: "videoEncoderSelector"
                        Layout.preferredWidth: 146
                        model: controller.recordingFormat === "gif" ? ["Software", "Automatic"]
                               : ["Software", "Automatic", "VA-API (Intel / AMD)", "NVIDIA NVENC", "Intel QSV"]
                        readonly property var backends: ["software", "auto", "vaapi", "nvenc", "qsv"]
                        currentIndex: Math.max(0, backends.indexOf(controller.recordingEncoder))
                        onActivated: controller.recordingEncoder = backends[currentIndex]
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true; Layout.preferredHeight: 30
                    Label { text: "Audio sources"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    Label { text: "Frame rate"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
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
                    Label { text: "Delay (seconds)"; color: root.secondaryText; font.pixelSize: theme.fontSize; Layout.fillWidth: true }
                    FlatSpin {
                        objectName: "recordingCountdown"
                        from: 0; to: 30
                        value: controller.countdownSeconds
                        onValueModified: controller.countdownSeconds = value
                    }
                }
                Label {
                    visible: controller.mode === "window" && (controller.windowUnavailable.length > 0 || controller.windows.length === 0)
                    text: controller.windowUnavailable.length > 0 ? controller.windowUnavailable : "No capturable windows found"
                    color: root.secondaryText; font.pixelSize: theme.fontSize
                    wrapMode: Text.Wrap; Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 1
                    Layout.topMargin: 7; Layout.bottomMargin: 6
                    color: theme.separatorColor
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
                    glyph: "qr"; tip: "Scan QR / barcodes"
                    checkable: true; checked: controller.action === "qr"
                    autoExclusive: true
                    enabled: !controller.busy
                    onClicked: controller.setAction("qr")
                }
                IconButton {
                    objectName: "ocrAction"
                    visible: !modeRow.compact
                    glyph: "ocr"; tip: "Recognize text"
                    checkable: true; checked: controller.action === "ocr"
                    autoExclusive: true
                    enabled: !controller.busy
                    onClicked: controller.setAction("ocr")
                }
                IconButton {
                    objectName: "lensAction"
                    visible: !modeRow.compact
                    glyph: "lens"; tip: "Google Lens · not implemented"
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
                    color: theme.separatorColor
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
                            font.pixelSize: theme.fontSize
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Glyph { anchors.centerIn: parent; visible: modeRow.compact; name: "close"; color: root.secondaryText; width: 17; height: 17 }
                    }
                    onClicked: controller.cancel()
                }
                Control {
                    id: confirmSplit
                    objectName: "confirmSplitButton"
                    Layout.preferredWidth: (modeRow.compact ? 34 : 75) + (advancedCapture.visible ? 24 : 0)
                    Layout.preferredHeight: 29
                    padding: 0
                    background: Rectangle {
                        radius: 6
                        color: confirmAction.enabled ? theme.buttonColor : theme.disabledColor
                    }
                    contentItem: RowLayout {
                    spacing: 0
                    Button {
                        id: confirmAction
                        objectName: "confirmAction"
                        text: controller.recording ? "Record" : controller.action === "qr" ? "Scan" : controller.action === "ocr" ? "OCR" : "Capture"
                        enabled: controller.canConfirm
                        Layout.preferredWidth: modeRow.compact ? 34 : 75
                        Layout.preferredHeight: 29
                        hoverEnabled: true
                        background: Rectangle {
                            color: confirmAction.enabled && confirmAction.hovered ? theme.buttonHoverColor : "transparent"
                            radius: 6
                            Rectangle {
                                visible: advancedCapture.visible
                                anchors.right: parent.right
                                width: parent.width / 2; height: parent.height
                                color: parent.color
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: confirmAction.text
                        contentItem: Item {
                            Text {
                                anchors.fill: parent
                                visible: !modeRow.compact
                                text: confirmAction.text
                                color: confirmAction.enabled ? theme.buttonText : theme.disabledText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: theme.fontSize; font.weight: Font.DemiBold
                            }
                            Glyph {
                                anchors.centerIn: parent
                                visible: modeRow.compact
                                name: controller.recording ? "record" : "capture"
                                color: confirmAction.enabled ? theme.buttonText : theme.disabledText
                                width: 17; height: 17
                            }
                        }
                        onClicked: controller.confirm()
                    }
                    ToolButton {
                        id: advancedCapture
                        objectName: "advancedCapture"
                        visible: !modeRow.compact && (controller.action === "capture" || controller.action === "record")
                        enabled: confirmAction.enabled
                        Layout.preferredWidth: 24; Layout.preferredHeight: 29
                        hoverEnabled: true
                        background: Rectangle {
                            color: advancedCapture.enabled && advancedCapture.hovered ? theme.buttonHoverColor : "transparent"
                            radius: 6
                            Rectangle {
                                anchors.left: parent.left
                                width: parent.width / 2; height: parent.height
                                color: parent.color
                            }
                            Rectangle {
                                objectName: "splitButtonDivider"
                                width: 1; height: 10
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                color: advancedCapture.enabled ? theme.buttonText : theme.disabledText
                                opacity: 0.22
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 12; height: 12
                                name: "chevron"; color: advancedCapture.enabled ? theme.buttonText : theme.disabledText
                                opacity: advancedCapture.enabled ? 1 : 0.42
                                rotation: advancedMenu.visible ? 180 : 0
                                Behavior on rotation { NumberAnimation { duration: 140 } }
                            }
                        }
                        onClicked: advancedMenu.open()
                        ResultMenu {
                            id: advancedMenu
                            objectName: "advancedResultMenu"
                            video: controller.recording
                            x: advancedCapture.width - width
                            y: -implicitHeight - 8
                        }
                    }
                    }
                }
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
            radius: theme.radius + 2
            border.width: 1
            border.color: theme.borderColor
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
                    font.pixelSize: theme.fontSize
                    Layout.topMargin: 3
                }
                TextField {
                    id: savePath
                    objectName: "saveDirectoryField"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    selectByMouse: true
                    enabled: !controller.busy
                    font.pixelSize: theme.fontSize
                    color: root.panelText
                    selectionColor: theme.selectionColor
                    selectedTextColor: root.panelText
                    leftPadding: 10
                    rightPadding: 10
                    onEditingFinished: {
                        controller.saveDirectory = text
                        text = controller.saveDirectory
                    }
                    background: Rectangle {
                        radius: 6
                        color: theme.inputColor
                        border.width: 1
                        border.color: savePath.activeFocus ? theme.focusColor : theme.borderColor
                    }
                }
                Text {
                    text: "Filename template"
                    color: root.secondaryText
                    font.pixelSize: theme.fontSize
                    Layout.topMargin: 10
                }
                TextField {
                    id: nameTemplate
                    objectName: "filenameTemplateField"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    selectByMouse: true
                    enabled: !controller.busy
                    font.pixelSize: theme.fontSize
                    color: root.panelText
                    selectionColor: theme.selectionColor
                    selectedTextColor: root.panelText
                    leftPadding: 10
                    rightPadding: 10
                    onEditingFinished: {
                        controller.filenameTemplate = text
                        text = controller.filenameTemplate
                    }
                    background: Rectangle {
                        radius: 6
                        color: theme.inputColor
                        border.width: 1
                        border.color: nameTemplate.activeFocus ? theme.focusColor : theme.borderColor
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
                    color: theme.separatorColor
                }
                RowLayout {
                    visible: !controller.recording
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Copy to clipboard"
                        color: root.secondaryText
                        font.pixelSize: theme.fontSize
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
                        font.pixelSize: theme.fontSize
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
                        font.pixelSize: theme.fontSize
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
                TextField {
                    id: videoEditorField
                    objectName: "videoEditorCommand"
                    visible: controller.action === "capture" || controller.action === "record"
                    Layout.fillWidth: true
                    placeholderText: controller.recording ? "Video editor: losslesscut {}" : "Screenshot editor: satty --filename -"
                    text: controller.editor
                    color: root.panelText
                    placeholderTextColor: root.secondaryText
                    selectionColor: theme.selectionColor
                    font.pixelSize: theme.fontSize
                    enabled: !controller.busy
                    selectByMouse: true
                    onEditingFinished: controller.editor = text
                    background: Rectangle {
                        color: theme.inputColor; radius: 6
                        border.color: videoEditorField.activeFocus ? theme.focusColor : theme.borderColor
                    }
                }
                RowLayout {
                    visible: controller.recording
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Text {
                        text: "Delay (seconds)"
                        color: root.secondaryText
                        font.pixelSize: theme.fontSize
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
                    font.pixelSize: theme.fontSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.topMargin: 8
                    Layout.bottomMargin: 4
                    color: theme.separatorColor
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
                            if (controller.videoEditor !== videoEditorField.text)
                                controller.videoEditor = videoEditorField.text
                            sessionSettings.close()
                        }
                        background: Rectangle {
                            radius: 6
                            color: doneSettings.down ? theme.buttonPressedColor : doneSettings.hovered ? theme.buttonHoverColor : theme.buttonColor
                        }
                        contentItem: Text {
                            text: doneSettings.text
                            color: theme.buttonText
                            font.pixelSize: theme.fontSize
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
