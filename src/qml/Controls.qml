import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var mainWindow
    implicitHeight: content.implicitHeight
    signal openMidiRequested()
    signal colorRequested(int channel)

    property var postValues: [-1, 0.0, 0.1, 0.5, 1.0, 2.0]
    property bool synthAdvancedVisible: false

    component SectionLabel: Text {
        color: "#6f6685"
        font.pixelSize: 9
        font.bold: true
        font.letterSpacing: 1.1
    }

    component FlatButton: Button {
        id: control
        property color normalColor: "#151126"
        property color hoverColor: "#211739"
        property color borderColor: "#352550"
        property color textColor: "#a99bc4"
        implicitHeight: 27
        leftPadding: 8
        rightPadding: 8
        background: Rectangle {
            radius: 5
            color: control.down ? "#2b1c4d" : (control.hovered ? control.hoverColor : control.normalColor)
            border.color: control.borderColor
            border.width: 1
            opacity: control.enabled ? 1.0 : 0.45
        }
        contentItem: Text {
            text: control.text
            color: control.textColor
            font.pixelSize: 9
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    component ConfigField: TextField {
        id: control
        property int minValue: 0
        property int maxValue: 2147483647
        implicitHeight: 23
        color: "#c4b5fd"
        font.pixelSize: 8
        font.bold: true
        horizontalAlignment: Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        selectByMouse: true
        leftPadding: 5
        rightPadding: 5
        validator: IntValidator {
            bottom: control.minValue
            top: control.maxValue
        }
        background: Rectangle {
            radius: 5
            color: "#151126"
            border.color: control.activeFocus ? "#7652d6" : "#352550"
            border.width: 1
            opacity: control.enabled ? 1.0 : 0.55
        }
    }

    component AccentButton: Button {
        id: control
        implicitWidth: 32
        implicitHeight: 32
        background: Rectangle {
            radius: 7
            color: control.enabled ? (control.down ? "#7c5ce0" : "#6d3fd6") : "#211b31"
            border.color: control.enabled ? "#9e7af0" : "#30293b"
            border.width: 1
        }
        contentItem: Text {
            text: control.text
            color: control.enabled ? "#ffffff" : "#6a6374"
            font.pixelSize: 12
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    component PurpleSlider: Slider {
        id: control
        implicitHeight: 18
        background: Rectangle {
            x: control.leftPadding
            y: control.topPadding + control.availableHeight / 2 - height / 2
            width: control.availableWidth
            height: 3
            radius: 2
            color: "#2b2440"
            Rectangle {
                width: control.visualPosition * parent.width
                height: parent.height
                radius: 2
                color: "#7652d6"
            }
        }
        handle: Rectangle {
            x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
            y: control.topPadding + control.availableHeight / 2 - height / 2
            implicitWidth: 11
            implicitHeight: 11
            radius: 6
            color: control.pressed ? "#c4b5fd" : "#9b7be8"
            border.color: "#d2c8f7"
            border.width: 1
        }
    }

    component LiveCard: Rectangle {
        property string label: ""
        property string value: "0"
        Layout.fillWidth: true
        Layout.preferredHeight: 36
        radius: 6
        color: "#0d0b1d"
        border.color: "#241b3b"
        border.width: 1

        Column {
            anchors.fill: parent
            anchors.leftMargin: 7
            anchors.rightMargin: 5
            anchors.topMargin: 4
            spacing: 0

            Text {
                width: parent.width
                text: parent.parent.label
                color: "#5d5470"
                font.pixelSize: 7
                font.bold: true
                font.letterSpacing: 0.7
                wrapMode: Text.NoWrap
            }

            Text {
                width: parent.width
                height: 18
                text: parent.parent.value
                color: "#c4b5fd"
                font.pixelSize: 14
                minimumPixelSize: 7
                fontSizeMode: Text.HorizontalFit
                font.bold: true
                wrapMode: Text.NoWrap
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    component MiniChart: Rectangle {
        id: chartBox
        property string title: "NPS"
        property real value: 0
        property color accent: "#a78bfa"
        property int historyLength: 280
        property var samples: []
        property int sampleIndex: 0
        property var revision: root.mainWindow.documentRevision
        implicitHeight: 56
        radius: 6
        color: "#0b0a18"
        border.color: "#211a35"
        border.width: 1

        function resetHistory() {
            var arr = []
            for (var i = 0; i < historyLength; ++i)
                arr.push(0)
            samples = arr
            sampleIndex = 0
            spark.requestPaint()
        }

        Component.onCompleted: resetHistory()
        onRevisionChanged: resetHistory()

        // MPWGL2 uses _HIST_RATE = 1000/30. Keep the same 30 Hz sample
        // cadence while repainting on the display cadence below.
        Timer {
            interval: 33
            running: chartBox.visible && root.mainWindow.hasMidi
            repeat: true
            onTriggered: {
                if (chartBox.samples.length !== chartBox.historyLength)
                    chartBox.resetHistory()
                chartBox.samples[chartBox.sampleIndex] = Number(chartBox.value)
                chartBox.sampleIndex = (chartBox.sampleIndex + 1) % chartBox.historyLength
            }
        }

        // The legacy _glLoop draws the charts every requestAnimationFrame.
        Timer {
            interval: 16
            running: chartBox.visible
            repeat: true
            onTriggered: spark.requestPaint()
        }

        Canvas {
            id: spark
            anchors.fill: parent
            opacity: 0.92
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var values = chartBox.samples
                var count = values.length
                if (!count)
                    return

                var maxValue = 1
                for (var i = 0; i < count; ++i)
                    maxValue = Math.max(maxValue, Number(values[i]) || 0)

                var topPad = 7
                var bottomPad = 3
                var graphHeight = Math.max(1, height - topPad - bottomPad)

                // Filled area exactly like _drawMiniChart in MPWGL2.
                ctx.beginPath()
                for (var x = 0; x < Math.max(2, width); ++x) {
                    var logical = Math.floor(x * count / Math.max(1, width))
                    var idx = (chartBox.sampleIndex + logical) % count
                    var v = Number(values[idx]) || 0
                    var y = height - bottomPad - (v / maxValue) * graphHeight
                    if (x === 0) ctx.moveTo(0, y); else ctx.lineTo(x, y)
                }
                ctx.lineTo(width, height)
                ctx.lineTo(0, height)
                ctx.closePath()
                ctx.globalAlpha = 0.20
                ctx.fillStyle = chartBox.accent
                ctx.fill()
                ctx.globalAlpha = 1.0

                ctx.beginPath()
                for (var x2 = 0; x2 < Math.max(2, width); ++x2) {
                    var logical2 = Math.floor(x2 * count / Math.max(1, width))
                    var idx2 = (chartBox.sampleIndex + logical2) % count
                    var v2 = Number(values[idx2]) || 0
                    var y2 = height - bottomPad - (v2 / maxValue) * graphHeight
                    if (x2 === 0) ctx.moveTo(0, y2); else ctx.lineTo(x2, y2)
                }
                ctx.strokeStyle = chartBox.accent
                ctx.lineWidth = 1.5
                ctx.stroke()
            }
        }

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 7
            anchors.topMargin: 5
            text: chartBox.title.toUpperCase()
            color: chartBox.title === "Skipped vel" ? "#a95368" : "#5d5470"
            font.pixelSize: 7
            font.bold: true
        }
        Text {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 7
            anchors.topMargin: 4
            width: Math.max(38, parent.width * 0.48)
            text: root.formatInteger(chartBox.value)
            color: chartBox.title === "Skipped vel" ? "#fb7185" : chartBox.accent
            font.pixelSize: 9
            minimumPixelSize: 6
            fontSizeMode: Text.HorizontalFit
            font.bold: true
            wrapMode: Text.NoWrap
            horizontalAlignment: Text.AlignRight
        }
    }

    component TimelineChart: Rectangle {
        id: timelineBox
        property var values: root.mainWindow.npsTimeline
        implicitHeight: 52
        radius: 6
        color: "#0b0a18"
        border.color: "#211a35"
        border.width: 1

        Timer {
            interval: 16
            running: timelineBox.visible
            repeat: true
            onTriggered: timeline.requestPaint()
        }

        Canvas {
            id: timeline
            anchors.fill: parent
            anchors.margins: 2
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var data = timelineBox.values
                if (!data || data.length === 0)
                    return

                var maxValue = 1
                for (var i = 0; i < data.length; ++i)
                    maxValue = Math.max(maxValue, Number(data[i]) || 0)

                ctx.beginPath()
                ctx.moveTo(0, height)
                for (var x = 0; x < width; ++x) {
                    var sample = Math.min(data.length - 1, Math.floor(x / Math.max(1, width) * data.length))
                    var y = height - ((Number(data[sample]) || 0) / maxValue) * (height - 4) - 1
                    ctx.lineTo(x, y)
                }
                ctx.lineTo(width, height)
                ctx.closePath()
                ctx.globalAlpha = 0.32
                ctx.fillStyle = "#818cf8"
                ctx.fill()
                ctx.globalAlpha = 1.0

                ctx.beginPath()
                for (var x2 = 0; x2 < width; ++x2) {
                    var sample2 = Math.min(data.length - 1, Math.floor(x2 / Math.max(1, width) * data.length))
                    var y2 = height - ((Number(data[sample2]) || 0) / maxValue) * (height - 4) - 1
                    if (x2 === 0) ctx.moveTo(0, y2); else ctx.lineTo(x2, y2)
                }
                ctx.strokeStyle = "#818cf8"
                ctx.lineWidth = 1.4
                ctx.stroke()

                if (root.mainWindow.duration > 0) {
                    var playX = Math.max(0, Math.min(width, root.mainWindow.currentTime / root.mainWindow.duration * width))
                    ctx.beginPath()
                    ctx.moveTo(playX, 0)
                    ctx.lineTo(playX, height)
                    ctx.strokeStyle = "rgba(196,181,253,0.55)"
                    ctx.lineWidth = 1
                    ctx.stroke()

                    if (root.mainWindow.peakNpsTime > 0) {
                        var peakX = Math.max(0, Math.min(width, root.mainWindow.peakNpsTime / root.mainWindow.duration * width))
                        ctx.beginPath()
                        ctx.moveTo(peakX, 0)
                        ctx.lineTo(peakX, height)
                        ctx.strokeStyle = "#fb923c"
                        ctx.lineWidth = 1.2
                        ctx.stroke()
                    }
                }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 7
            anchors.topMargin: 5
            text: "NPS TIMELINE"
            color: "#5d5470"
            font.pixelSize: 7
            font.bold: true
        }
        Text {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 7
            anchors.topMargin: 4
            width: Math.max(38, parent.width * 0.48)
            text: root.formatInteger(root.mainWindow.nps)
            color: "#818cf8"
            font.pixelSize: 9
            minimumPixelSize: 6
            fontSizeMode: Text.HorizontalFit
            font.bold: true
            wrapMode: Text.NoWrap
            horizontalAlignment: Text.AlignRight
        }
    }

    ColumnLayout {
        id: content
        width: root.width
        spacing: 6

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            radius: 13
            color: "#1a1036"
            border.color: "#5b3a93"
            border.width: 1
            Row {
                anchors.centerIn: parent
                spacing: 7
                Rectangle { width: 5; height: 5; radius: 3; color: "#a78bfa"; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "BLACK MIDI PLAYER"; color: "#c4b5fd"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.8 }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.mainWindow.hasMidi ? 34 : 0
            visible: root.mainWindow.hasMidi
            radius: 6
            color: "#120d27"
            border.color: "#2b1d4c"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 7
                anchors.rightMargin: 5
                spacing: 5
                Text { text: "♪"; color: "#a78bfa"; font.pixelSize: 12 }
                Text {
                    Layout.fillWidth: true
                    text: root.mainWindow.fileName
                    color: "#c4b5fd"
                    font.pixelSize: 9
                    font.bold: true
                    elide: Text.ElideMiddle
                }
                Text { text: root.formatInteger(root.mainWindow.noteCount) + " notes"; color: "#645a79"; font.pixelSize: 8 }
                FlatButton {
                    Layout.preferredWidth: 24
                    implicitHeight: 22
                    text: "✕"
                    onClicked: root.mainWindow.clearFile()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            LiveCard { label: "ACTIVE"; value: root.formatInteger(root.mainWindow.activeVoices) }
            LiveCard { label: "NPS"; value: root.formatInteger(root.mainWindow.nps) }
            LiveCard { label: "BPM"; value: root.mainWindow.hasMidi ? root.formatNumber(root.mainWindow.bpm, 2) : "—" }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5

            AccentButton {
                enabled: root.mainWindow.hasMidi
                text: root.mainWindow.isPlaying ? "Ⅱ" : "▶"
                onClicked: root.mainWindow.isPlaying ? root.mainWindow.pause() : root.mainWindow.play()
            }
            FlatButton {
                Layout.preferredWidth: 29
                implicitHeight: 29
                enabled: root.mainWindow.hasMidi
                text: "■"
                onClicked: root.mainWindow.stop()
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                PurpleSlider {
                    id: timeSlider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(0.001, root.mainWindow.duration)
                    value: root.mainWindow.currentTime
                    enabled: root.mainWindow.hasMidi
                    onMoved: root.mainWindow.seek(value)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: formatTime(root.mainWindow.currentTime); color: "#625a73"; font.pixelSize: 7 }
                    Item { Layout.fillWidth: true }
                    Text { text: formatTime(root.mainWindow.duration); color: "#625a73"; font.pixelSize: 7 }
                }
            }

            Text { text: root.mainWindow.volume === 0 ? "×" : "♪"; color: "#8e82a6"; font.pixelSize: 10 }
            PurpleSlider {
                Layout.preferredWidth: 62
                from: 0
                to: 100
                value: root.mainWindow.volume
                onMoved: root.mainWindow.volume = Math.round(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5
            Text { text: "Note Speed"; color: "#6c627d"; font.pixelSize: 8; Layout.preferredWidth: 56 }
            PurpleSlider {
                Layout.fillWidth: true
                from: 0.1
                to: 60.0
                value: root.mainWindow.noteSpeed
                onMoved: root.mainWindow.noteSpeed = value
            }
            Text { text: root.mainWindow.noteSpeed.toFixed(1) + "s"; color: "#a78bfa"; font.pixelSize: 8; Layout.preferredWidth: 30 }
        }

        RowLayout {
            Layout.fillWidth: true
            Text { text: "Post-buffer"; color: "#6c627d"; font.pixelSize: 8 }
            Item { Layout.fillWidth: true }
            FlatButton {
                Layout.preferredWidth: 55
                implicitHeight: 21
                text: root.mainWindow.postBufferAuto ? "auto" : root.mainWindow.postBuffer.toFixed(1) + "s"
                onClicked: cyclePostBuffer()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel { text: "COLORS" }
            Item { Layout.fillWidth: true }
            FlatButton {
                Layout.preferredWidth: 62
                implicitHeight: 21
                text: "Per Track"
                normalColor: root.mainWindow.perTrackColors ? "#2a194b" : "#120f22"
                borderColor: root.mainWindow.perTrackColors ? "#6b48a8" : "#352550"
                textColor: root.mainWindow.perTrackColors ? "#c4b5fd" : "#7a708c"
                onClicked: root.mainWindow.perTrackColors = !root.mainWindow.perTrackColors
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 4
            rowSpacing: 4
            visible: root.mainWindow.hasMidi
            Repeater {
                model: 16
                delegate: Rectangle {
                    required property int index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 22
                    radius: 4
                    color: "#0f0d1e"
                    border.color: root.mainWindow.channelColorList[index]
                    border.width: 1
                    Row {
                        anchors.centerIn: parent
                        spacing: 4
                        Rectangle { width: 5; height: 5; radius: 3; color: root.mainWindow.channelColorList[parent.parent.index] }
                        Text { text: "CH " + (parent.parent.index + 1); color: "#9d91b2"; font.pixelSize: 7; font.bold: true }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.colorRequested(parent.index)
                    }
                }
            }
        }

        TimelineChart {
            Layout.fillWidth: true
            visible: root.mainWindow.hasMidi
            implicitHeight: root.mainWindow.hasMidi ? 52 : 0
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5
            MiniChart { Layout.fillWidth: true; title: "NPS"; value: root.mainWindow.nps; accent: "#a78bfa" }
            MiniChart { Layout.fillWidth: true; title: "Polyphony"; value: root.mainWindow.activeVoices; accent: "#38bdf8" }
            MiniChart { Layout.fillWidth: true; title: "BPM"; value: root.mainWindow.hasMidi ? Math.round(root.mainWindow.bpm) : 0; accent: "#fb923c" }
            MiniChart { Layout.fillWidth: true; title: "CC/s"; value: root.mainWindow.ccPerSecond; accent: "#34d399" }
            MiniChart { Layout.fillWidth: true; title: "Skipped vel"; value: root.mainWindow.skippedVelocity; accent: "#fb7185" }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5

            SectionLabel { text: "SNAPPYSYNTH V2" }
            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: 57
                Layout.preferredHeight: 20
                radius: 4
                color: root.mainWindow.soundfontLoaded ? "#0e211c"
                     : root.mainWindow.synthReady ? "#171222"
                     : "#120f20"
                border.color: root.mainWindow.soundfontLoaded ? "#236a55"
                            : root.mainWindow.synthReady ? "#382c4d"
                            : "#2c2341"

                Text {
                    anchors.centerIn: parent
                    text: root.mainWindow.soundfontLoaded ? "SF2 READY"
                        : root.mainWindow.synthReady ? "READY"
                        : "IDLE"
                    color: root.mainWindow.soundfontLoaded ? "#34d399" : "#81758f"
                    font.pixelSize: 7
                    font.bold: true
                }
            }

            FlatButton {
                Layout.preferredWidth: 68
                implicitHeight: 21
                enabled: !root.mainWindow.isPlaying
                text: root.mainWindow.soundfontLoaded ? "Add Layer" : "Load SF2"
                onClicked: root.mainWindow.openSoundfontPicker()
            }

            FlatButton {
                Layout.preferredWidth: 43
                implicitHeight: 21
                visible: root.mainWindow.soundfontLoaded
                enabled: !root.mainWindow.isPlaying
                text: "Clear"
                onClicked: root.mainWindow.clearSoundfonts()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.synthAdvancedVisible ? 366 : 218
            radius: 6
            color: "#0c0a1a"
            border.color: "#211834"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 7
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Text {
                        Layout.preferredWidth: 28
                        text: "SF2"
                        color: "#6c627d"
                        font.pixelSize: 8
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.mainWindow.soundfontLoaded
                            ? root.mainWindow.soundfontName
                            : "No SoundFont loaded"
                        color: root.mainWindow.soundfontLoaded ? "#c4b5fd" : "#6b6378"
                        font.pixelSize: 8
                        font.bold: root.mainWindow.soundfontLoaded
                        elide: Text.ElideMiddle
                    }

                    Text {
                        text: root.formatInteger(root.mainWindow.synthLayers) + " layer" +
                              (root.mainWindow.synthLayers === 1 ? "" : "s")
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: root.mainWindow.synthStatus
                    color: root.mainWindow.soundfontLoaded ? "#6f998c" : "#5c5368"
                    font.pixelSize: 7
                    elide: Text.ElideRight
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text { text: "Voices"; color: "#6c627d"; font.pixelSize: 8 }
                    ConfigField {
                        id: maxVoicesField
                        Layout.preferredWidth: 72
                        enabled: !root.mainWindow.isPlaying
                        minValue: 1
                        maxValue: 5000000
                        text: String(root.mainWindow.synthMaxVoices)
                        onEditingFinished: {
                            var n = parseInt(text)
                            if (isFinite(n))
                                root.mainWindow.synthMaxVoices = n
                            text = String(root.mainWindow.synthMaxVoices)
                        }
                    }

                    Text { text: "Block"; color: "#6c627d"; font.pixelSize: 8 }
                    ConfigField {
                        id: blockFramesField
                        Layout.preferredWidth: 55
                        enabled: !root.mainWindow.isPlaying
                        minValue: 1
                        maxValue: 65536
                        text: String(root.mainWindow.synthBufferFrames)
                        onEditingFinished: {
                            var n = parseInt(text)
                            if (isFinite(n))
                                root.mainWindow.synthBufferFrames = n
                            text = String(root.mainWindow.synthBufferFrames)
                        }
                    }

                    Text { text: "Bufs"; color: "#6c627d"; font.pixelSize: 8 }
                    ConfigField {
                        id: numBuffersField
                        Layout.preferredWidth: 40
                        enabled: !root.mainWindow.isPlaying
                        minValue: 1
                        maxValue: 128
                        text: String(root.mainWindow.synthNumBuffers)
                        onEditingFinished: {
                            var n = parseInt(text)
                            if (isFinite(n))
                                root.mainWindow.synthNumBuffers = n
                            text = String(root.mainWindow.synthNumBuffers)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text { text: "Prebuf"; color: "#6c627d"; font.pixelSize: 8 }
                    ConfigField {
                        Layout.preferredWidth: 47
                        minValue: 0
                        maxValue: 3600
                        text: String(Math.round(root.mainWindow.synthPrebufferSeconds))
                        onEditingFinished: {
                            var n = parseInt(text)
                            if (isFinite(n))
                                root.mainWindow.synthPrebufferSeconds = n
                            text = String(Math.round(root.mainWindow.synthPrebufferSeconds))
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Rolling audio pre-render seconds. 0 = up to the MIDI duration (memory-capped)."
                    }

                    Text { text: "Vel ≥"; color: "#6c627d"; font.pixelSize: 8 }
                    ConfigField {
                        Layout.preferredWidth: 38
                        minValue: 0
                        maxValue: 127
                        text: String(root.mainWindow.synthVelocityFloor)
                        onEditingFinished: {
                            var n = parseInt(text)
                            if (isFinite(n))
                                root.mainWindow.synthVelocityFloor = n
                            text = String(root.mainWindow.synthVelocityFloor)
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Minimum NoteOn velocity sent to SnappySynth. The live Skipped vel meter rises automatically while audio catches up."
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "effective " + root.mainWindow.skippedVelocity
                        color: root.mainWindow.skippedVelocity > root.mainWindow.synthVelocityFloor ? "#fb7185" : "#70667e"
                        font.pixelSize: 7
                        horizontalAlignment: Text.AlignRight
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    FlatButton {
                        Layout.preferredWidth: 76
                        implicitHeight: 21
                        text: "Stack gain"
                        normalColor: root.mainWindow.synthOverlapGain ? "#2a194b" : "#110e20"
                        borderColor: root.mainWindow.synthOverlapGain ? "#6b48a8" : "#352550"
                        textColor: root.mainWindow.synthOverlapGain ? "#c4b5fd" : "#746a87"
                        onClicked: root.mainWindow.synthOverlapGain = !root.mainWindow.synthOverlapGain
                    }

                    FlatButton {
                        Layout.preferredWidth: 68
                        implicitHeight: 21
                        enabled: !root.mainWindow.isPlaying
                        text: "Soft clip"
                        normalColor: root.mainWindow.synthSoftClip ? "#2a194b" : "#110e20"
                        borderColor: root.mainWindow.synthSoftClip ? "#6b48a8" : "#352550"
                        textColor: root.mainWindow.synthSoftClip ? "#c4b5fd" : "#746a87"
                        onClicked: root.mainWindow.synthSoftClip = !root.mainWindow.synthSoftClip
                    }

                    Item { Layout.fillWidth: true }

                    FlatButton {
                        Layout.preferredWidth: 72
                        implicitHeight: 21
                        text: root.synthAdvancedVisible ? "Advanced ▲" : "Advanced ▼"
                        onClicked: root.synthAdvancedVisible = !root.synthAdvancedVisible
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Text {
                        text: "RATE " + (root.mainWindow.synthSampleRate > 0
                            ? root.formatInteger(root.mainWindow.synthSampleRate) + " Hz"
                            : "—")
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }

                    Text {
                        text: "ACTIVE " + root.formatInteger(root.mainWindow.synthActiveVoices)
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }

                    Text {
                        text: "FREE " + root.formatInteger(root.mainWindow.synthFreeVoices)
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }

                    Text {
                        text: "STEALS " + root.formatInteger(root.mainWindow.synthSteals)
                        color: root.mainWindow.synthSteals > 0 ? "#fb923c" : "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    Text {
                        text: "WORKERS " +
                              (root.mainWindow.synthWorkerCount > 0
                                  ? root.formatInteger(root.mainWindow.synthWorkerCount)
                                  : "—")
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }
                    Text {
                        text: "REGIONS " + root.formatInteger(root.mainWindow.synthRegions)
                        color: "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }
                    Text {
                        text: "UNDERRUNS " + root.formatInteger(root.mainWindow.synthUnderruns)
                        color: root.mainWindow.synthUnderruns > 0 ? "#fb923c" : "#70667e"
                        font.pixelSize: 7
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.synthAdvancedVisible
                    spacing: 5

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#211834"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text { text: "Rate req"; color: "#6c627d"; font.pixelSize: 8 }
                        ConfigField {
                            id: requestedRateField
                            Layout.preferredWidth: 67
                            enabled: !root.mainWindow.synthReady
                            minValue: 0
                            maxValue: 384000
                            text: String(root.mainWindow.synthRequestedSampleRate)
                            onEditingFinished: {
                                var n = parseInt(text)
                                if (isFinite(n))
                                    root.mainWindow.synthRequestedSampleRate = n
                                text = String(root.mainWindow.synthRequestedSampleRate)
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: "0 = browser/device default. Change before the audio backend starts."
                        }

                        Text { text: "Ch"; color: "#6c627d"; font.pixelSize: 8 }
                        FlatButton {
                            Layout.preferredWidth: 39
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: root.mainWindow.synthChannels === 1 ? "1" : "2"
                            onClicked: root.mainWindow.synthChannels =
                                root.mainWindow.synthChannels === 1 ? 2 : 1
                        }

                        Text { text: "Bits"; color: "#6c627d"; font.pixelSize: 8 }
                        FlatButton {
                            Layout.preferredWidth: 42
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: String(root.mainWindow.synthBitsPerSample)
                            onClicked: root.mainWindow.synthBitsPerSample =
                                root.mainWindow.synthBitsPerSample === 16 ? 32 : 16
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text { text: "Min voices"; color: "#6c627d"; font.pixelSize: 8 }
                        ConfigField {
                            id: minVoicesField
                            Layout.preferredWidth: 68
                            enabled: !root.mainWindow.isPlaying
                            minValue: 0
                            maxValue: 5000000
                            text: String(root.mainWindow.synthMinVoices)
                            onEditingFinished: {
                                var n = parseInt(text)
                                if (isFinite(n))
                                    root.mainWindow.synthMinVoices = n
                                text = String(root.mainWindow.synthMinVoices)
                            }
                        }

                        Text { text: "Workers"; color: "#6c627d"; font.pixelSize: 8 }
                        ConfigField {
                            id: workersField
                            Layout.preferredWidth: 43
                            enabled: !root.mainWindow.isPlaying
                            minValue: 0
                            maxValue: 256
                            text: String(root.mainWindow.synthWorkers)
                            onEditingFinished: {
                                var n = parseInt(text)
                                if (isFinite(n))
                                    root.mainWindow.synthWorkers = n
                                text = String(root.mainWindow.synthWorkers)
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: "0 = original auto policy / all eligible logical cores."
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text { text: "Sharding"; color: "#6c627d"; font.pixelSize: 8 }
                        FlatButton {
                            Layout.preferredWidth: 64
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: root.mainWindow.synthNoteSharding === 1 ? "Channel"
                                : root.mainWindow.synthNoteSharding === 2 ? "Hash"
                                : "Auto"
                            onClicked: root.mainWindow.synthNoteSharding =
                                (root.mainWindow.synthNoteSharding + 1) % 3
                        }

                        FlatButton {
                            Layout.preferredWidth: 67
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: "RT priority"
                            normalColor: root.mainWindow.synthRealtimePriority ? "#2a194b" : "#110e20"
                            borderColor: root.mainWindow.synthRealtimePriority ? "#6b48a8" : "#352550"
                            textColor: root.mainWindow.synthRealtimePriority ? "#c4b5fd" : "#746a87"
                            onClicked: root.mainWindow.synthRealtimePriority =
                                !root.mainWindow.synthRealtimePriority
                            ToolTip.visible: hovered
                            ToolTip.text: "Preserves AudioConfig.realtime_priority. Browser thread priority itself is OS/browser managed."
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        FlatButton {
                            Layout.preferredWidth: 78
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: "Steal cache"
                            normalColor: root.mainWindow.synthStealScoreCache ? "#2a194b" : "#110e20"
                            borderColor: root.mainWindow.synthStealScoreCache ? "#6b48a8" : "#352550"
                            textColor: root.mainWindow.synthStealScoreCache ? "#c4b5fd" : "#746a87"
                            onClicked: root.mainWindow.synthStealScoreCache =
                                !root.mainWindow.synthStealScoreCache
                        }

                        FlatButton {
                            Layout.preferredWidth: 83
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: "Fast note-off"
                            normalColor: root.mainWindow.synthFastNoteOff ? "#2a194b" : "#110e20"
                            borderColor: root.mainWindow.synthFastNoteOff ? "#6b48a8" : "#352550"
                            textColor: root.mainWindow.synthFastNoteOff ? "#c4b5fd" : "#746a87"
                            onClicked: root.mainWindow.synthFastNoteOff =
                                !root.mainWindow.synthFastNoteOff
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        FlatButton {
                            Layout.preferredWidth: 84
                            implicitHeight: 21
                            enabled: !root.mainWindow.isPlaying
                            text: "Validate state"
                            normalColor: root.mainWindow.synthValidateState ? "#4a2331" : "#110e20"
                            borderColor: root.mainWindow.synthValidateState ? "#a94d68" : "#352550"
                            textColor: root.mainWindow.synthValidateState ? "#fb7185" : "#746a87"
                            onClicked: root.mainWindow.synthValidateState =
                                !root.mainWindow.synthValidateState
                            ToolTip.visible: hovered
                            ToolTip.text: "Debug consistency checks from SS_VALIDATE_STATE. Slower; leave off for performance."
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: "Audio: AudioWorklet"
                            color: "#4f485b"
                            font.pixelSize: 7
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Source-only desktop options: AudioAPI=WinMM/DirectSound and GPU DirectCompute mixer are not available in browser WASM."
                        color: "#484252"
                        font.pixelSize: 7
                        wrapMode: Text.Wrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Worker → original SnappySynthV2 voice engine → AudioWorklet → system audio"
                    color: "#484252"
                    font.pixelSize: 7
                    elide: Text.ElideRight
                }
            }
        }

        StatsPanel {
            Layout.fillWidth: true
            visible: root.mainWindow.hasMidi
            implicitHeight: root.mainWindow.hasMidi ? 142 : 0
            mainWindow: root.mainWindow
        }

        Item { Layout.preferredHeight: 52 }
    }

    function cyclePostBuffer() {
        var idx = 0
        if (!root.mainWindow.postBufferAuto) {
            var current = Number(root.mainWindow.postBuffer.toFixed(1))
            idx = 1
            for (var i = 1; i < root.postValues.length; ++i) {
                if (Math.abs(root.postValues[i] - current) < 0.01) {
                    idx = i
                    break
                }
            }
            idx = (idx + 1) % root.postValues.length
        } else {
            idx = 1
        }
        if (root.postValues[idx] < 0)
            root.mainWindow.setPostBufferAuto()
        else
            root.mainWindow.postBuffer = root.postValues[idx]
    }

    function groupIntegerString(digits) {
        var out = ""
        for (var i = 0; i < digits.length; ++i) {
            if (i > 0 && ((digits.length - i) % 3) === 0)
                out += ","
            out += digits.charAt(i)
        }
        return out
    }

    // QML/JS may choose scientific notation for large Numbers. Build the
    // decimal representation explicitly so UI counters always show natural
    // digits (for example 44,750,700, never 4.47507E+07).
    function formatInteger(value) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"
        var rounded = Math.round(n)
        var negative = rounded < 0
        var digits = Math.abs(rounded).toFixed(0)
        return (negative ? "-" : "") + groupIntegerString(digits)
    }

    function formatNumber(value, maxDecimals) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"

        var decimals = Math.max(0, Math.min(8, Math.floor(Number(maxDecimals) || 0)))
        var negative = n < 0
        var fixed = Math.abs(n).toFixed(decimals)
        var dot = fixed.indexOf(".")
        var integerPart = dot >= 0 ? fixed.substring(0, dot) : fixed
        var decimalPart = dot >= 0 ? fixed.substring(dot + 1) : ""

        while (decimalPart.length > 0 && decimalPart.charAt(decimalPart.length - 1) === "0")
            decimalPart = decimalPart.substring(0, decimalPart.length - 1)

        return (negative ? "-" : "") +
            groupIntegerString(integerPart) +
            (decimalPart.length > 0 ? "." + decimalPart : "")
    }


    function formatTime(seconds) {
        var s = Math.max(0, Math.floor(seconds))
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        var r = s % 60
        if (h > 0)
            return h + ":" + (m < 10 ? "0" : "") + m + ":" + (r < 10 ? "0" : "") + r
        return m + ":" + (r < 10 ? "0" : "") + r
    }
}
