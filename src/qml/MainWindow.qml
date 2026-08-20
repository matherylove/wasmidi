import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Wasmidi

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 800
    title: "WASMIDI Player - " + mainWindow.fileName
    
    MainWindow {
        id: mainWindow
    }
    
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        
        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            color: "#0a0820"
            
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 15
                
                Label {
                    text: "🎹 WASMIDI Player"
                    font.pixelSize: 18
                    font.bold: true
                    color: "#c4b5fd"
                }
                
                Item { Layout.fillWidth: true }
                
                Button {
                    text: "📁 Load MIDI"
                    onClicked: fileDialog.open()
                    
                    background: Rectangle {
                        color: parent.pressed ? "#4c1d95" : "#5b21b6"
                        radius: 6
                    }
                }
            }
        }
        
        // Main content
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            
            // Left panel (controls)
            Rectangle {
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                color: "#0f0b1e"
                
                ScrollView {
                    anchors.fill: parent
                    clip: true
                    
                    Controls {
                        mainWindow: mainWindow
                    }
                }
            }
            
            // Right panel (piano roll + keyboard)
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0
                
                PianoRoll {
                    id: pianoRoll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    mainWindow: mainWindow
                }
                
                Keyboard {
                    id: keyboard
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    mainWindow: mainWindow
                }
            }
        }
    }
    
    FileDialog {
        id: fileDialog
        nameFilters: ["MIDI Files (*.mid *.midi)"]
        onAccepted: {
            var file = Qt.openUrlExternally(selectedFile)
            if (file) {
                var reader = new FileReader()
                reader.onload = function(e) {
                    mainWindow.loadMidiFile(e.target.result)
                }
                reader.readAsArrayBuffer(file)
            }
        }
    }
}