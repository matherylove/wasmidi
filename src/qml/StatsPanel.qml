import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var mainWindow
    implicitHeight: 148
    color: "#0e0b20"
    radius: 8
    border.color: "#251b48"

    GridLayout {
        anchors.fill: parent
        anchors.margins: 9
        columns: 3
        rowSpacing: 5
        columnSpacing: 8

        Repeater {
            model: [
                ["Notes", root.mainWindow.noteCount.toLocaleString()],
                ["Tracks", root.mainWindow.trackCount.toString()],
                ["Duration", formatTime(root.mainWindow.duration)],
                ["Format", root.mainWindow.midiFormat.toString()],
                ["PPQ", root.mainWindow.ppq.toString()],
                ["Tempo", root.mainWindow.bpm.toFixed(1) + " BPM"],
                ["Tempo chg", root.mainWindow.tempoChangeCount.toString()],
                ["CC events", root.mainWindow.controlEventCount.toLocaleString()],
                ["Peak NPS", root.mainWindow.peakNps.toLocaleString()],
                ["Peak poly", root.mainWindow.peakPolyphony.toLocaleString()],
                ["Live NPS", root.mainWindow.nps.toLocaleString()],
                ["Active", root.mainWindow.activeVoices.toLocaleString()]
            ]

            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: "#110e26"
                radius: 5
                Column {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.topMargin: 3
                    spacing: 0
                    Text { text: modelData[0].toUpperCase(); color: "#554c69"; font.pixelSize: 7; font.bold: true }
                    Text { text: modelData[1]; color: "#c4b5fd"; font.pixelSize: 11; font.bold: true }
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
