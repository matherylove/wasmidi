import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    signal openRequested()
    clip: true

    property bool dragActive: false
    property int fpsValue: 0
    property real neuralPhase: 0
    property var neuralNodes: []

    Component.onCompleted: {
        var nodes = []
        for (var i = 0; i < 54; ++i) {
            nodes.push({
                x: Math.random(),
                y: Math.random(),
                vx: (Math.random() - 0.5) * 0.00075,
                vy: (Math.random() - 0.5) * 0.00075,
                r: 0.75 + Math.random() * 1.25,
                pulse: Math.random() * Math.PI * 2
            })
        }
        neuralNodes = nodes
        neural.requestPaint()
    }

    Rectangle {
        anchors.fill: parent
        radius: 7
        color: "#050511"
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
            opacity: root.mainWindow.hasMidi ? 0.16 : 0.92

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var nodes = root.neuralNodes
                if (!nodes || nodes.length === 0)
                    return

                var gradient = ctx.createRadialGradient(
                    width * 0.52, height * 0.44, 0,
                    width * 0.52, height * 0.44, Math.max(width, height) * 0.72
                )
                gradient.addColorStop(0, "rgba(70, 42, 120, 0.12)")
                gradient.addColorStop(1, "rgba(5, 5, 17, 0)")
                ctx.fillStyle = gradient
                ctx.fillRect(0, 0, width, height)

                var maxDistance = Math.max(125, Math.min(205, width * 0.15))

                for (var i = 0; i < nodes.length; ++i) {
                    var a = nodes[i]
                    var ax = a.x * width
                    var ay = a.y * height

                    for (var j = i + 1; j < nodes.length; ++j) {
                        var b = nodes[j]
                        var bx = b.x * width
                        var by = b.y * height
                        var dx = ax - bx
                        var dy = ay - by
                        var dist = Math.sqrt(dx * dx + dy * dy)

                        if (dist < maxDistance) {
                            var alpha = (1.0 - dist / maxDistance)
                            alpha = alpha * alpha * 0.42
                            ctx.beginPath()
                            ctx.moveTo(ax, ay)
                            ctx.lineTo(bx, by)
                            ctx.strokeStyle = "rgba(128, 88, 205, " + alpha + ")"
                            ctx.lineWidth = 0.65
                            ctx.stroke()
                        }
                    }
                }

                for (var k = 0; k < nodes.length; ++k) {
                    var n = nodes[k]
                    var nx = n.x * width
                    var ny = n.y * height
                    var glow = 0.68 + Math.sin(root.neuralPhase + n.pulse) * 0.22

                    ctx.beginPath()
                    ctx.arc(nx, ny, n.r + 1.4, 0, Math.PI * 2)
                    ctx.fillStyle = "rgba(115, 78, 185, " + (0.11 * glow) + ")"
                    ctx.fill()

                    ctx.beginPath()
                    ctx.arc(nx, ny, n.r, 0, Math.PI * 2)
                    ctx.fillStyle = "rgba(180, 148, 255, " + (0.72 * glow) + ")"
                    ctx.fill()
                }
            }
        }

        // MPWGL2 playhead: vertical line at 18% of the roll width.
        Rectangle {
            x: Math.round(parent.width * 0.18)
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 2
            color: "#a78bfa"
            opacity: root.mainWindow.hasMidi ? 0.72 : 0
            z: 5
        }

        Column {
            anchors.centerIn: parent
            spacing: 7
            visible: !root.mainWindow.hasMidi && !root.dragActive

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

            Column {
                anchors.centerIn: parent
                spacing: 8
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "♪"
                    color: "#c4b5fd"
                    font.pixelSize: 38
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Drop MIDI to load"
                    color: "#c4b5fd"
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: ".mid / .midi · Format 0 & 1"
                    color: "#8979a7"
                    font.pixelSize: 9
                }
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

    FrameAnimation {
        id: frameAnimation
        running: root.visible

        onTriggered: {
            root.fpsValue =
                smoothFrameTime > 0
                    ? Math.round(1.0 / smoothFrameTime)
                    : 0

            /*
             * QML Canvas is significantly more expensive than the native
             * WebGL roll. While MIDI is playing, animate the neural backdrop
             * every second display frame (~30 Hz); its movement is time-based
             * so it keeps the same speed.
             */
            if (root.mainWindow.isPlaying &&
                (currentFrame & 1))
                return

            var scale =
                frameTime > 0
                    ? frameTime / (1.0 / 60.0)
                    : 1.0

            root.neuralPhase +=
                0.035 * scale

            var next = []

            for (var i = 0;
                 i < root.neuralNodes.length;
                 ++i) {
                var n =
                    root.neuralNodes[i]

                var x =
                    n.x + n.vx * scale

                var y =
                    n.y + n.vy * scale

                if (x <= 0 || x >= 1) {
                    n.vx = -n.vx
                    x = Math.max(
                        0,
                        Math.min(1, x))
                }

                if (y <= 0 || y >= 1) {
                    n.vy = -n.vy
                    y = Math.max(
                        0,
                        Math.min(1, y))
                }

                n.x = x
                n.y = y
                next.push(n)
            }

            root.neuralNodes = next
            neural.requestPaint()
        }
    }
}
