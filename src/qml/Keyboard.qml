import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    clip: true

    Rectangle {
        anchors.fill: parent
        color: "#07071a"
        radius: 6
        border.color: "#211a3e"
        KeyboardSurface {
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
        }
    }
}
