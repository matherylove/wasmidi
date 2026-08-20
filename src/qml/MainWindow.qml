import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Wasmidi

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    color: "#07071a"
    title: mainWindow.fileName.length ? "WASMIDI — " + mainWindow.fileName : "WASMIDI"

    property int colorChannel: 0

    PlayerController {
        id: mainWindow
        onLoadFailed: (message) => errorDialog.text = message
    }

    FileDialog {
        id: fileDialog
        title: "Open MIDI"
        nameFilters: ["MIDI files (*.mid *.midi)", "All files (*)"]
        onAccepted: mainWindow.loadMidiUrl(selectedFile)
    }

    ColorDialog {
        id: colorDialog
        title: "Channel color"
        onAccepted: mainWindow.setChannelColor(window.colorChannel, selectedColor)
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
                Layout.preferredHeight: 46
                color: "#0a0820"
                border.color: "#241b4b"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 10

                    Label {
                        text: "WASMIDI"
                        color: "#c4b5fd"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Rectangle {
                        implicitWidth: 47
                        implicitHeight: 20
                        radius: 10
                        color: "#24154d"
                        border.color: "#5633a8"
                        Label { anchors.centerIn: parent; text: "WASM"; color: "#a78bfa"; font.pixelSize: 9; font.bold: true }
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        visible: mainWindow.fileName.length > 0
                        text: mainWindow.fileName
                        color: "#7c7395"
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: 380
                    }
                    Button {
                        text: "Open MIDI"
                        onClicked: fileDialog.open()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 320
                    Layout.minimumWidth: 280
                    Layout.fillHeight: true
                    color: "#0b091a"
                    border.color: "#211a3e"

                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                        Controls {
                            width: Math.max(0, parent.width - 8)
                            mainWindow: mainWindow
                            onOpenMidiRequested: fileDialog.open()
                            onColorRequested: (channel) => {
                                window.colorChannel = channel
                                colorDialog.open()
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
                        anchors.margins: 8
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 28
                            Label { text: "PIANO ROLL"; color: "#746a91"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
                            Item { Layout.fillWidth: true }
                            Label { text: mainWindow.noteCount.toLocaleString() + " notes"; color: "#655d78"; font.pixelSize: 10 }
                            Label { text: mainWindow.activeVoices + " active"; color: "#a78bfa"; font.pixelSize: 10 }
                        }

                        PianoRoll {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            mainWindow: mainWindow
                        }

                        Keyboard {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 122
                            mainWindow: mainWindow
                        }
                    }
                }
            }
        }
    }
}
