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
            anchors.topMargin: 4
            spacing: 0
            Text { text: parent.parent.label; color: "#5d5470"; font.pixelSize: 7; font.bold: true; font.letterSpacing: 0.7 }
            Text { text: parent.parent.value; color: "#c4b5fd"; font.pixelSize: 14; font.bold: true }
        }
    }

    component MiniChart: Rectangle {
        id: chartBox
        property string title: "NPS"
        property real value: 0
        property color accent: "#8b6ce8"
        property var samples: []
        implicitHeight: 56
        radius: 6
        color: "#0b0a18"
        border.color: "#211a35"
        border.width: 1

        Timer {
            interval: 250
            running: true
            repeat: true
            onTriggered: {
                var copy = chartBox.samples.slice(Math.max(0, chartBox.samples.length - 54))
                copy.push(Number(chartBox.value))
                chartBox.samples = copy
                spark.requestPaint()
            }
        }

        Canvas {
            id: spark
            anchors.fill: parent
            anchors.margins: 4
            opacity: 0.6
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var values = chartBox.samples
                if (!values || values.length < 2)
                    return
                var maxValue = 1
                for (var i = 0; i < values.length; ++i)
                    maxValue = Math.max(maxValue, values[i])
                ctx.beginPath()
                ctx.lineWidth = 1
                ctx.strokeStyle = chartBox.accent
                for (var j = 0; j < values.length; ++j) {
                    var px = (j / Math.max(1, values.length - 1)) * width
                    var py = height - 5 - (values[j] / maxValue) * (height - 13)
                    if (j === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                }
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
            text: chartBox.value
            color: chartBox.title === "Skipped vel" ? "#f07188" : "#c4b5fd"
            font.pixelSize: 9
            font.bold: true
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
                Text { text: root.mainWindow.noteCount.toLocaleString() + " notes"; color: "#645a79"; font.pixelSize: 8 }
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
            LiveCard { label: "ACTIVE"; value: root.mainWindow.activeVoices.toLocaleString() }
            LiveCard { label: "NPS"; value: root.mainWindow.nps.toLocaleString() }
            LiveCard { label: "BPM"; value: root.mainWindow.hasMidi ? root.mainWindow.bpm.toFixed(0) : "—" }
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

        MiniChart {
            Layout.fillWidth: true
            visible: root.mainWindow.hasMidi
            implicitHeight: root.mainWindow.hasMidi ? 48 : 0
            title: "NPS Timeline"
            value: root.mainWindow.nps
            accent: "#7457c7"
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5
            MiniChart { Layout.fillWidth: true; title: "NPS"; value: root.mainWindow.nps; accent: "#8b6ce8" }
            MiniChart { Layout.fillWidth: true; title: "Polyphony"; value: root.mainWindow.activeVoices; accent: "#8063dd" }
            MiniChart { Layout.fillWidth: true; title: "BPM"; value: root.mainWindow.hasMidi ? Math.round(root.mainWindow.bpm) : 0; accent: "#9a78e9" }
            MiniChart { Layout.fillWidth: true; title: "CC/s"; value: root.mainWindow.ccPerSecond; accent: "#765cc9" }
            MiniChart { Layout.fillWidth: true; title: "Skipped vel"; value: root.mainWindow.skippedVelocity; accent: "#b85770" }
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel { text: "MIDI I/O" }
            Item { Layout.fillWidth: true }
            spacing: 3
            FlatButton {
                Layout.preferredWidth: 54; implicitHeight: 21; text: "MIDI Out"
                normalColor: root.mainWindow.outputMode === "native" ? "#2a194b" : "#110e20"
                textColor: root.mainWindow.outputMode === "native" ? "#c4b5fd" : "#746a87"
                onClicked: root.mainWindow.outputMode = "native"
            }
            FlatButton {
                Layout.preferredWidth: 44; implicitHeight: 21; text: "MIDI In"
                normalColor: root.mainWindow.outputMode === "input" ? "#2a194b" : "#110e20"
                textColor: root.mainWindow.outputMode === "input" ? "#c4b5fd" : "#746a87"
                onClicked: root.mainWindow.outputMode = "input"
            }
            FlatButton {
                Layout.preferredWidth: 29; implicitHeight: 21; text: "Off"
                normalColor: root.mainWindow.outputMode === "off" ? "#2a194b" : "#110e20"
                textColor: root.mainWindow.outputMode === "off" ? "#c4b5fd" : "#746a87"
                onClicked: root.mainWindow.outputMode = "off"
            }
            FlatButton {
                Layout.preferredWidth: 79; implicitHeight: 21; text: "Embedded"
                normalColor: root.mainWindow.outputMode === "embedded" ? "#2a194b" : "#110e20"
                textColor: root.mainWindow.outputMode === "embedded" ? "#c4b5fd" : "#746a87"
                onClicked: root.mainWindow.outputMode = "embedded"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.mainWindow.outputMode === "embedded" ? 112 : 66
            radius: 6
            color: "#0c0a1a"
            border.color: "#211834"
            border.width: 1

            Loader {
                anchors.fill: parent
                anchors.margins: 7
                sourceComponent: root.mainWindow.outputMode === "native" ? nativePanel
                               : root.mainWindow.outputMode === "input" ? inputPanel
                               : root.mainWindow.outputMode === "embedded" ? embeddedPanel
                               : offPanel
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

    Component {
        id: nativePanel
        ColumnLayout {
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    Layout.preferredWidth: 62
                    Layout.preferredHeight: 21
                    radius: 4
                    color: "#171222"
                    border.color: "#382c4d"
                    Text { anchors.centerIn: parent; text: "Not started"; color: "#82788e"; font.pixelSize: 8; font.bold: true }
                }
                FlatButton { Layout.preferredWidth: 76; implicitHeight: 21; text: "Request Access"; enabled: false }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 21
                    radius: 4
                    color: "#0b0918"
                    border.color: "#2c2341"
                    Text { anchors.centerIn: parent; text: "— select output —"; color: "#686075"; font.pixelSize: 8 }
                }
                FlatButton { Layout.preferredWidth: 25; implicitHeight: 21; text: "↺"; enabled: false }
                FlatButton { Layout.preferredWidth: 45; implicitHeight: 21; text: "All Off"; enabled: false }
            }
            Text { text: "Web MIDI bridge will be connected in the I/O port stage."; color: "#514a5f"; font.pixelSize: 7; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }

    Component {
        id: inputPanel
        ColumnLayout {
            spacing: 5
            Text { text: "MIDI IN"; color: "#a78bfa"; font.pixelSize: 9; font.bold: true }
            Text { text: "Input device enumeration, thru routing and live visualization are reserved for the browser MIDI bridge stage."; color: "#5e566d"; font.pixelSize: 8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }

    Component {
        id: offPanel
        Item {
            Text { anchors.centerIn: parent; text: "MIDI output disabled — visualization only."; color: "#655d72"; font.pixelSize: 8 }
        }
    }

    Component {
        id: embeddedPanel
        ColumnLayout {
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                Text { text: "SnappySynth / SF2"; color: "#c4b5fd"; font.pixelSize: 9; font.bold: true }
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredWidth: 54; Layout.preferredHeight: 19; radius: 4
                    color: "#171222"; border.color: "#382c4d"
                    Text { anchors.centerIn: parent; text: "No SF2"; color: "#81758f"; font.pixelSize: 7; font.bold: true }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "Volume"; color: "#6c627d"; font.pixelSize: 8; Layout.preferredWidth: 42 }
                PurpleSlider { Layout.fillWidth: true; from: 0; to: 100; value: root.mainWindow.volume; onMoved: root.mainWindow.volume = Math.round(value) }
                Text { text: root.mainWindow.volume + "%"; color: "#a78bfa"; font.pixelSize: 8; Layout.preferredWidth: 26 }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "Voices"; color: "#6c627d"; font.pixelSize: 8 }
                Item { Layout.fillWidth: true }
                Text { text: "256 / layer"; color: "#81768f"; font.pixelSize: 8 }
                Text { text: "Layers"; color: "#6c627d"; font.pixelSize: 8 }
                Text { text: "2"; color: "#81768f"; font.pixelSize: 8 }
            }
            Text { text: "SF2 engine controls are ready for the native synth port."; color: "#514a5f"; font.pixelSize: 7; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
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
