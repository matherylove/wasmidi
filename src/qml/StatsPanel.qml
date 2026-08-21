import QtQuick
import QtQuick.Layouts

Item {
    id: root
    required property var mainWindow
    implicitHeight: 142

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Text {
            text: "FILE INFO"
            color: "#6f6685"
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.1
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 3
            rowSpacing: 4
            columnSpacing: 4

            Repeater {
                model: [
                    ["Notes", root.mainWindow.noteCount.toLocaleString()],
                    ["Tracks", root.mainWindow.trackCount.toString()],
                    ["Dur", formatTime(root.mainWindow.duration)],
                    ["BPM", root.mainWindow.bpm.toFixed(0)],
                    ["Chs", root.mainWindow.activeChannelCount.toString()],
                    ["Fmt", root.mainWindow.midiFormat.toString()],
                    ["PPQ", root.mainWindow.ppq.toString()],
                    ["Tmp Chg", root.mainWindow.tempoChangeCount.toString()],
                    ["Pk NPS", root.mainWindow.peakNps.toLocaleString()],
                    ["Pk Poly", root.mainWindow.peakPolyphony.toLocaleString()],
                    ["CC Evts", root.mainWindow.controlEventCount.toLocaleString()],
                    ["Pitch", root.mainWindow.pitchRange]
                ]

                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 4
                    color: "#0d0b1c"
                    border.color: "#1d172e"
                    border.width: 1
                    Column {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.topMargin: 3
                        spacing: 0
                        Text { text: modelData[0].toUpperCase(); color: "#554d66"; font.pixelSize: 6; font.bold: true; font.letterSpacing: 0.4 }
                        Text {
                            text: modelData[1]
                            color: "#bdb0d5"
                            font.pixelSize: 9
                            minimumPixelSize: 6
                            fontSizeMode: Text.HorizontalFit
                            font.bold: true
                            wrapMode: Text.NoWrap
                            width: parent.width - 8
                        }
                    }
                }
            }
        }
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
