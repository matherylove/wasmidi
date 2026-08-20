import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var mainWindow
    implicitHeight: content.implicitHeight
    signal openMidiRequested()
    signal colorRequested(int channel)

    ColumnLayout {
        id: content
        width: root.width
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            Label { text: "PLAYER"; color: "#766c91"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
            Item { Layout.fillWidth: true }
            Button { text: "Open"; onClicked: root.openMidiRequested() }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            radius: 7
            color: "#110d27"
            border.color: "#271d4a"
            RowLayout {
                anchors.fill: parent
                anchors.margins: 7
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label { text: root.mainWindow.fileName.length ? root.mainWindow.fileName : "No MIDI loaded"; color: "#c4b5fd"; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label { text: root.mainWindow.noteCount.toLocaleString() + " notes · " + root.mainWindow.trackCount + " tracks"; color: "#625977"; font.pixelSize: 9 }
                }
                Label { text: "F" + root.mainWindow.midiFormat; color: "#a78bfa"; font.pixelSize: 9; font.bold: true }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5
            Button {
                Layout.preferredWidth: 42
                Layout.preferredHeight: 34
                enabled: root.mainWindow.noteCount > 0
                text: root.mainWindow.isPlaying ? "Ⅱ" : "▶"
                onClicked: root.mainWindow.isPlaying ? root.mainWindow.pause() : root.mainWindow.play()
            }
            Button { Layout.preferredWidth: 34; Layout.preferredHeight: 34; enabled: root.mainWindow.noteCount > 0; text: "■"; onClicked: root.mainWindow.stop() }
            Slider {
                id: timeSlider
                Layout.fillWidth: true
                from: 0
                to: Math.max(0.001, root.mainWindow.duration)
                value: root.mainWindow.currentTime
                enabled: root.mainWindow.noteCount > 0
                onMoved: root.mainWindow.seek(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: formatTime(root.mainWindow.currentTime); color: "#706781"; font.pixelSize: 9; font.family: "monospace" }
            Item { Layout.fillWidth: true }
            Label { text: formatTime(root.mainWindow.duration); color: "#706781"; font.pixelSize: 9; font.family: "monospace" }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Window"; color: "#756c87"; font.pixelSize: 10; Layout.preferredWidth: 54 }
            Slider { Layout.fillWidth: true; from: 0.1; to: 60; value: root.mainWindow.noteSpeed; onMoved: root.mainWindow.noteSpeed = value }
            Label { text: root.mainWindow.noteSpeed.toFixed(1) + "s"; color: "#a78bfa"; font.pixelSize: 9; Layout.preferredWidth: 34 }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Post"; color: "#756c87"; font.pixelSize: 10; Layout.preferredWidth: 54 }
            ComboBox {
                Layout.fillWidth: true
                model: ["0.0 s", "0.1 s", "0.5 s", "1.0 s", "2.0 s"]
                onActivated: {
                    const values = [0.0, 0.1, 0.5, 1.0, 2.0]
                    root.mainWindow.postBuffer = values[currentIndex]
                }
            }
        }

        StatsPanel { Layout.fillWidth: true; mainWindow: root.mainWindow }

        Label { text: "COLORS"; color: "#766c91"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }

        RowLayout {
            Layout.fillWidth: true
            Button {
                Layout.fillWidth: true
                text: root.mainWindow.perTrackColors ? "Per Track" : "Per Channel"
                onClicked: root.mainWindow.perTrackColors = !root.mainWindow.perTrackColors
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 4
            rowSpacing: 4
            Repeater {
                model: 16
                delegate: Button {
                    required property int index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 25
                    text: "CH " + (index + 1)
                    font.pixelSize: 8
                    onClicked: root.colorRequested(index)
                }
            }
        }

        Label { text: "MIDI OUTPUT"; color: "#766c91"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }

        RowLayout {
            Layout.fillWidth: true
            Button {
                Layout.fillWidth: true
                checkable: true
                checked: root.mainWindow.outputMode === "embedded"
                text: "Embedded Synth"
                onClicked: root.mainWindow.outputMode = "embedded"
            }
            Button {
                Layout.fillWidth: true
                checkable: true
                checked: root.mainWindow.outputMode === "native"
                text: "Web MIDI"
                onClicked: root.mainWindow.outputMode = "native"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.mainWindow.outputMode === "embedded" ? 150 : 92
            radius: 8
            color: "#0e0b20"
            border.color: "#251b48"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 5

                Label {
                    text: root.mainWindow.outputMode === "embedded" ? "SnappySynth / SF2" : "Browser MIDI device"
                    color: "#c4b5fd"
                    font.pixelSize: 10
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.mainWindow.outputMode === "embedded"
                          ? "Audio engine bridge is the next port stage; the UI/state is now native QML."
                          : "Web MIDI device enumeration and sending will be provided by the minimal browser bridge."
                    color: "#696078"
                    font.pixelSize: 9
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "Volume"; color: "#756c87"; font.pixelSize: 9; Layout.preferredWidth: 48 }
                    Slider { Layout.fillWidth: true; from: 0; to: 100; value: root.mainWindow.volume; onMoved: root.mainWindow.volume = value }
                    Label { text: root.mainWindow.volume + "%"; color: "#a78bfa"; font.pixelSize: 9; Layout.preferredWidth: 30 }
                }

                RowLayout {
                    visible: root.mainWindow.outputMode === "embedded"
                    Layout.fillWidth: true
                    Label { text: "Voices"; color: "#756c87"; font.pixelSize: 9 }
                    Item { Layout.fillWidth: true }
                    Label { text: "256 / layer"; color: "#8a7da0"; font.pixelSize: 9 }
                }

                RowLayout {
                    visible: root.mainWindow.outputMode === "embedded"
                    Layout.fillWidth: true
                    Label { text: "Workers"; color: "#756c87"; font.pixelSize: 9 }
                    Item { Layout.fillWidth: true }
                    Label { text: "2"; color: "#8a7da0"; font.pixelSize: 9 }
                }
            }
        }

        Item { Layout.preferredHeight: 8 }
    }

    function formatTime(seconds) {
        var s = Math.max(0, Math.floor(seconds))
        var m = Math.floor(s / 60)
        var r = s % 60
        return m + ":" + (r < 10 ? "0" : "") + r
    }
}
