import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Wasmidi

ApplicationWindow {
    id: window
    visible: true
    width: 1440
    height: 900
    minimumWidth: 840
    minimumHeight: 560
    color: "#07071a"
    title: mainWindow.fileName.length ? "WASMIDI — " + mainWindow.fileName : "WASMIDI"

    property int colorChannel: 0
    property bool sidebarCollapsed: false
    property var presetColors: [
        "#818cf8", "#a78bfa", "#c084fc", "#e879f9",
        "#f472b6", "#fb7185", "#fb923c", "#facc15",
        "#a3e635", "#4ade80", "#34d399", "#2dd4bf",
        "#22d3ee", "#38bdf8", "#60a5fa", "#8b5cf6"
    ]

    PlayerController {
        id: mainWindow
        onLoadFailed: (message) => {
            errorDialog.text = message
            errorDialog.open()
        }
    }


    ColorDialog {
        id: colorDialog
        title: "Custom channel color"

        // Qt Quick Dialogs updates selectedColor while the browser color picker
        // is being manipulated. Push every preview change directly into both GPU
        // renderers instead of waiting for Accept.
        onSelectedColorChanged: {
            if (visible)
                mainWindow.setChannelColor(window.colorChannel, selectedColor)
        }

        onAccepted: {
            mainWindow.setChannelColor(window.colorChannel, selectedColor)
            colorPopup.close()
        }
    }

    MessageDialog {
        id: errorDialog
        title: "WASMIDI"
    }

    Rectangle {
        anchors.fill: parent
        color: "#07071a"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                color: "#09071d"
                border.color: "#21173b"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Math.max(14, (parent.width - 1140) / 2)
                    anchors.rightMargin: Math.max(14, (parent.width - 1140) / 2)
                    spacing: 8

                    RowLayout {
                        spacing: 7
                        Rectangle {
                            implicitWidth: 22
                            implicitHeight: 22
                            radius: 5
                            color: "#17102e"
                            border.color: "#6d4dd0"
                            Text {
                                anchors.centerIn: parent
                                text: "◇"
                                color: "#a78bfa"
                                font.pixelSize: 15
                                font.bold: true
                            }
                        }
                        Text {
                            text: "Dekxtopia"
                            color: "#c4b5fd"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: "Home"
                        color: "#756b8d"
                        font.pixelSize: 11
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Qt.openUrlExternally("https://matherylove.github.io/") }
                    }

                    Rectangle {
                        implicitWidth: 146
                        implicitHeight: 25
                        radius: 6
                        color: "#1a1038"
                        border.color: "#2b1b55"
                        Row {
                            anchors.centerIn: parent
                            spacing: 7
                            Text { text: "MIDI Player WebGL2"; color: "#c4b5fd"; font.pixelSize: 10; font.bold: true }
                            Rectangle {
                                width: 29; height: 14; radius: 7
                                color: "#4b287c"
                                Text { anchors.centerIn: parent; text: "NEW"; color: "#c4b5fd"; font.pixelSize: 7; font.bold: true }
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        id: sidebar
                        Layout.preferredWidth: window.sidebarCollapsed ? 0 : 320
                        Layout.minimumWidth: window.sidebarCollapsed ? 0 : 280
                        Layout.maximumWidth: window.sidebarCollapsed ? 0 : 360
                        Layout.fillHeight: true
                        color: "#090817"
                        border.color: "#1d1731"
                        clip: true


                        ScrollView {
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 9
                            anchors.topMargin: 7
                            anchors.bottomMargin: 7
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                            ScrollBar.vertical.policy: ScrollBar.AsNeeded

                            Controls {
                                width: Math.max(0, parent.width - 8)
                                mainWindow: mainWindow
                                onOpenMidiRequested: mainWindow.openMidiPicker()
                                onColorRequested: (channel) => {
                                    window.colorChannel = channel
                                    colorPopup.open()
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#07071a"

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            anchors.topMargin: 6
                            anchors.bottomMargin: 6
                            spacing: 5

                            PianoRoll {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                mainWindow: mainWindow
                                onOpenRequested: mainWindow.openMidiPicker()
                            }

                            Keyboard {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(105, Math.min(145, window.height * 0.16))
                                mainWindow: mainWindow
                            }
                        }
                    }
                }

                Rectangle {
                    id: sidebarFab
                    width: 46
                    height: 46
                    radius: 23
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 18
                    anchors.bottomMargin: 18
                    color: window.sidebarCollapsed ? "#251548" : "#151526"
                    border.color: window.sidebarCollapsed ? "#7c4dff" : "#3c3b55"
                    border.width: 1
                    z: 20

                    Column {
                        anchors.centerIn: parent
                        spacing: 4
                        Repeater {
                            model: 3
                            Rectangle {
                                width: index === 1 ? 15 : 22
                                height: 2
                                radius: 1
                                color: "#c8c6d3"
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: window.sidebarCollapsed = !window.sidebarCollapsed
                    }
                }
            }
        }
    }

    Rectangle {
        id: midiLoadingOverlay
        anchors.fill: parent
        visible: mainWindow.midiLoading
        z: 1000
        color: "#e607071a"

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }

        Rectangle {
            width: Math.min(520, parent.width - 48)
            height: 196
            anchors.centerIn: parent
            radius: 16
            color: "#0d0b1e"
            border.color: "#4c347d"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 13

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Rectangle {
                        width: 38
                        height: 38
                        radius: 9
                        color: "#1b1237"
                        border.color: "#6846ad"

                        Text {
                            anchors.centerIn: parent
                            text: "◇"
                            color: "#c4b5fd"
                            font.pixelSize: 23
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: "Loading MIDI"
                            color: "#e9e5ff"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Text {
                            Layout.fillWidth: true
                            text: mainWindow.midiLoadingStage.length
                                  ? mainWindow.midiLoadingStage
                                  : "Preparing parser"
                            color: "#9d94b5"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    Text {
                        text: mainWindow.midiLoadingProgress + "%"
                        color: "#c4b5fd"
                        font.pixelSize: 19
                        font.bold: true
                    }
                }

                Item { Layout.preferredHeight: 2 }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 14
                    radius: 7
                    color: "#17122a"
                    border.color: "#2e2349"
                    clip: true

                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, mainWindow.midiLoadingProgress / 100.0))
                        height: parent.height
                        radius: parent.radius
                        color: "#7c5ce0"

                        Behavior on width {
                            NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Parsing runs in a background worker"
                        color: "#766d89"
                        font.pixelSize: 10
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "UI stays responsive"
                        color: "#766d89"
                        font.pixelSize: 10
                    }
                }
            }
        }
    }

    Popup {
        id: colorPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 250
        height: 195
        modal: false
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 10
            color: "#0d0b1e"
            border.color: "#38275e"
            border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 9

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Ch " + (window.colorChannel + 1) + " — Color"
                    color: "#c4b5fd"
                    font.pixelSize: 11
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Text { text: "✕"; color: "#766d89"; font.pixelSize: 13; MouseArea { anchors.fill: parent; onClicked: colorPopup.close() } }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                rowSpacing: 6
                columnSpacing: 6
                Repeater {
                    model: window.presetColors
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 27
                        radius: 5
                        color: modelData
                        border.color: "#ffffff55"
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                mainWindow.setChannelColor(window.colorChannel, parent.color)
                                colorPopup.close()
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 31
                radius: 6
                color: "#17122a"
                border.color: "#33264e"
                Text { anchors.centerIn: parent; text: "Custom color…"; color: "#a78bfa"; font.pixelSize: 10; font.bold: true }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        colorDialog.selectedColor = mainWindow.channelColorList[window.colorChannel]
                        colorDialog.open()
                    }
                }
            }
        }
    }
}
