import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    clip: true

    Rectangle {
        anchors.fill: parent
        color: "#080817"
        radius: 5
        border.color: "#18142a"
        border.width: 1

        KeyboardSurface {
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
        }

        Repeater {
            model: 11
            delegate: Text {
                required property int index
                property int pitch: index * 12
                property int whiteBefore: Math.floor(pitch / 12) * 7
                x: (whiteBefore / 75.0) * parent.width + 4
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                text: "C" + (index - 1)
                color: "#39405a"
                font.pixelSize: 7
                font.bold: true
            }
        }
    }
}
