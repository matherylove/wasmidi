import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "#07071a"
        border.color: "#211a3e"

        PianoRollSurface {
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: parent.height * 0.075
            height: 1
            color: "#a78bfa"
            opacity: root.mainWindow.noteCount > 0 ? 0.35 : 0
        }

        Column {
            anchors.centerIn: parent
            spacing: 7
            visible: root.mainWindow.noteCount === 0
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♪"; color: "#4c4168"; font.pixelSize: 44 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Open a MIDI file to begin"; color: "#7c7395"; font.pixelSize: 14 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Format 0 / 1 · high-density renderer"; color: "#514960"; font.pixelSize: 10 }
        }
    }
}
