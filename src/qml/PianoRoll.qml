import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    signal openRequested()
    clip: true

    property bool dragActive: false
    property int fpsValue: 0

    Rectangle {
        anchors.fill: parent
        radius: 7
        color: "#050511"
        border.color: "#151127"

        PianoRollSurface {
            id: surface
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
            z: 1
        }

        Column {
            anchors.centerIn: parent
            spacing: 7
            visible: !root.mainWindow.hasMidi &&
                     !root.dragActive
            z: 4

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "♬"
                color: "#4e386d"
                font.pixelSize: 34
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Drop a MIDI file here or click to open"
                color: "#706182"
                font.pixelSize: 11
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Format 0 / 1 · Black MIDI ready"
                color: "#443a52"
                font.pixelSize: 8
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: root.dragActive
            color: "#241044dd"
            radius: 7
            border.color: "#9f7aea"
            border.width: 2
            z: 10

            Text {
                anchors.centerIn: parent
                text: "Drop MIDI to load"
                color: "#c4b5fd"
                font.pixelSize: 15
                font.bold: true
            }
        }

        Text {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 7
            anchors.bottomMargin: 5
            text: root.fpsValue + " FPS"
            color: "#76668e"
            font.pixelSize: 7
            font.bold: true
            z: 6
        }

        MouseArea {
            anchors.fill: parent
            enabled: !root.mainWindow.hasMidi
            cursorShape: Qt.PointingHandCursor
            onClicked: root.openRequested()
        }

        DropArea {
            anchors.fill: parent

            onEntered: (drag) => {
                root.dragActive = true
                drag.acceptProposedAction()
            }

            onExited:
                root.dragActive = false

            onDropped: (drop) => {
                root.dragActive = false

                if (drop.urls &&
                    drop.urls.length > 0) {
                    root.mainWindow.loadMidiUrl(
                        drop.urls[0])
                } else {
                    root.openRequested()
                }

                drop.acceptProposedAction()
            }
        }
    }

    // Samples the completed C++/WebGL render rate; this timer does not drive
    // frames. Renderer::update() is now the only animation source.
    Timer {
        interval: 500
        running: root.visible
        repeat: true
        onTriggered:
            root.fpsValue =
                surface.renderFps
    }
}
