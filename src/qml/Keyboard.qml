import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    clip: true

    Rectangle {
        anchors.fill: parent
        color: "#070716"
        radius: 5
        border.color: "#171328"
        border.width: 1

        KeyboardSurface {
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
        }

        // Octave labels aligned to the C white keys, matching the legacy
        // horizontal keysCanvas labels.
        Repeater {
            model: 11

            delegate: Text {
                required property int index

                property int whiteBeforeC: index * 7

                x: (whiteBeforeC / 75.0) * parent.width + 3
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4

                text: "C" + (index - 1)
                color: "#34405f"
                font.pixelSize: 7
                font.bold: true
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: "#30274a"
            opacity: 0.65
        }
    }
}
