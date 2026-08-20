import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    signal openRequested()
    clip: true

    property bool dragActive: false
    property int frameCounter: 0
    property int fpsValue: 0
    property var neuralNodes: []

    Component.onCompleted: {
        var nodes = []
        for (var i = 0; i < 30; ++i) {
            nodes.push({
                x: Math.random(),
                y: Math.random(),
                vx: (Math.random() - 0.5) * 0.00035,
                vy: (Math.random() - 0.5) * 0.00035
            })
        }
        neuralNodes = nodes
    }

    Rectangle {
        anchors.fill: parent
        radius: 7
        color: "#07071a"
        border.color: "#151127"
        border.width: 1

        PianoRollSurface {
            id: surface
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
        }

        Canvas {
            id: neural
            anchors.fill: parent
            visible: !root.mainWindow.hasMidi
            opacity: 0.55
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var nodes = root.neuralNodes
                if (!nodes || nodes.length === 0)
                    return

                for (var i = 0; i < nodes.length; ++i) {
                    var a = nodes[i]
                    var ax = a.x * width
                    var ay = a.y * height
                    ctx.fillStyle = "#8564d0"
                    ctx.globalAlpha = 0.65
                    ctx.fillRect(ax, ay, 1.3, 1.3)
                    for (var j = i + 1; j < nodes.length; ++j) {
                        var b = nodes[j]
                        var bx = b.x * width
                        var by = b.y * height
                        var dx = ax - bx
                        var dy = ay - by
                        var dist = Math.sqrt(dx * dx + dy * dy)
                        if (dist < 145) {
                            ctx.beginPath()
                            ctx.moveTo(ax, ay)
                            ctx.lineTo(bx, by)
                            ctx.strokeStyle = "#604492"
                            ctx.globalAlpha = (1.0 - dist / 145.0) * 0.28
                            ctx.lineWidth = 0.55
                            ctx.stroke()
                        }
                    }
                }
                ctx.globalAlpha = 1.0
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: parent.height * 0.075
            height: 1
            color: "#a78bfa"
            opacity: root.mainWindow.hasMidi ? 0.32 : 0
        }

        Column {
            anchors.centerIn: parent
            spacing: 6
            visible: !root.mainWindow.hasMidi && !root.dragActive
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♬"; color: "#2c2343"; font.pixelSize: 31 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop a MIDI file here or click to open"; color: "#554c69"; font.pixelSize: 10 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Format 0 / 1 · Black MIDI ready"; color: "#332d40"; font.pixelSize: 8 }
        }

        Rectangle {
            anchors.fill: parent
            visible: root.dragActive
            color: "#241044cc"
            radius: 7
            border.color: "#9f7aea"
            border.width: 2
            z: 10
            Column {
                anchors.centerIn: parent
                spacing: 8
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "♪"; color: "#c4b5fd"; font.pixelSize: 38 }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop MIDI to load"; color: "#c4b5fd"; font.pixelSize: 15; font.bold: true }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: ".mid / .midi · Format 0 & 1"; color: "#8979a7"; font.pixelSize: 9 }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 7
            anchors.bottomMargin: 5
            text: root.fpsValue + " FPS"
            color: "#5c5271"
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
            onEntered: (drag) => { root.dragActive = true; drag.acceptProposedAction() }
            onExited: root.dragActive = false
            onDropped: (drop) => {
                root.dragActive = false
                if (drop.urls && drop.urls.length > 0)
                    root.mainWindow.loadMidiUrl(drop.urls[0])
                else
                    root.openRequested()
                drop.acceptProposedAction()
            }
        }
    }

    Timer {
        interval: 16
        running: root.visible
        repeat: true
        onTriggered: {
            root.frameCounter++
            if (!root.mainWindow.hasMidi) {
                var next = []
                for (var i = 0; i < root.neuralNodes.length; ++i) {
                    var n = root.neuralNodes[i]
                    var x = n.x + n.vx
                    var y = n.y + n.vy
                    if (x < 0 || x > 1) n.vx = -n.vx
                    if (y < 0 || y > 1) n.vy = -n.vy
                    n.x = Math.max(0, Math.min(1, x))
                    n.y = Math.max(0, Math.min(1, y))
                    next.push(n)
                }
                root.neuralNodes = next
                neural.requestPaint()
            }
        }
    }

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: {
            root.fpsValue = root.frameCounter
            root.frameCounter = 0
        }
    }
}
