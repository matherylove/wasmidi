import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root
    property var mainWindow
    
    property alias controlsColumn: controlsColumn
    
    ColumnLayout {
        id: controlsColumn
        width: parent.width
        spacing: 12
        
        // File info
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            color: "transparent"
            border.color: "#5b21b6"
            border.width: 1
            radius: 8
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 5
                
                Label {
                    text: "📄 File Info"
                    font.bold: true
                    color: "#a78bfa"
                }
                
                Label {
                    text: mainWindow.fileName || "No file loaded"
                    font.pixelSize: 12
                    color: "#c4b5fd"
                    elide: Text.ElideMiddle
                }
                
                Label {
                    text: "Notes: " + mainWindow.noteCount + " | Tracks: " + mainWindow.trackCount
                    font.pixelSize: 11
                    color: "#94a3b8"
                }
            }
        }
        
        // Transport controls
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            
            Button {
                text: mainWindow.isPlaying ? "⏸" : "▶"
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                
                background: Rectangle {
                    color: parent.pressed ? "#4c1d95" : "#6d28d9"
                    radius: 8
                }
                
                onClicked: {
                    if (mainWindow.isPlaying) {
                        mainWindow.pause()
                    } else {
                        mainWindow.play()
                    }
                }
            }
            
            Button {
                text: "⏹"
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                
                background: Rectangle {
                    color: parent.pressed ? "#4c1d95" : "#5b21b6"
                    radius: 8
                }
                
                onClicked: mainWindow.stop()
            }
            
            Slider {
                id: timeSlider
                Layout.fillWidth: true
                from: 0
                to: mainWindow.duration
                value: mainWindow.currentTime
                
                background: Rectangle {
                    color: "#1e1b4b"
                    radius: 2
                    height: 4
                    
                    Rectangle {
                        width: timeSlider.visualPosition * parent.width
                        height: parent.height
                        color: "#a78bfa"
                        radius: 2
                    }
                }
                
                handle: Rectangle {
                    x: timeSlider.leftPadding + timeSlider.visualPosition * 
                       (timeSlider.availableWidth - width)
                    y: timeSlider.topPadding + timeSlider.availableHeight / 2 - height / 2
                    implicitWidth: 12
                    implicitHeight: 12
                    radius: 6
                    color: timeSlider.pressed ? "#c4b5fd" : "#a78bfa"
                }
                
                onMoved: {
                    mainWindow.seek(value)
                }
            }
        }
        
        // Time display
        RowLayout {
            Layout.fillWidth: true
            spacing: 5
            
            Label {
                text: formatTime(mainWindow.currentTime)
                font.pixelSize: 11
                font.family: "monospace"
                color: "#94a3b8"
            }
            
            Item { Layout.fillWidth: true }
            
            Label {
                text: formatTime(mainWindow.duration)
                font.pixelSize: 11
                font.family: "monospace"
                color: "#94a3b8"
            }
        }
        
        // Note speed
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            
            Label {
                text: "Note Speed"
                font.pixelSize: 12
                color: "#94a3b8"
            }
            
            Slider {
                id: speedSlider
                Layout.fillWidth: true
                from: 0.1
                to: 60
                stepSize: 0.1
                value: mainWindow.noteSpeed
                
                background: Rectangle {
                    color: "#1e1b4b"
                    radius: 2
                    height: 3
                }
                
                handle: Rectangle {
                    x: speedSlider.leftPadding + speedSlider.visualPosition * 
                       (speedSlider.availableWidth - width)
                    y: speedSlider.topPadding + speedSlider.availableHeight / 2 - height / 2
                    implicitWidth: 11
                    implicitHeight: 11
                    radius: 6
                    color: "#a78bfa"
                }
                
                onMoved: {
                    mainWindow.setNoteSpeed(value)
                }
            }
            
            Label {
                text: speedSlider.value.toFixed(1) + "s"
                font.pixelSize: 12
                color: "#a78bfa"
                Layout.preferredWidth: 40
            }
        }
        
        // Post-buffer
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            
            Label {
                text: "Post-buffer"
                font.pixelSize: 12
                color: "#94a3b8"
            }
            
            ComboBox {
                Layout.fillWidth: true
                model: ["auto", "0.1s", "0.5s", "1.0s", "2.0s"]
                currentIndex: 0
                
                background: Rectangle {
                    color: "#1e1b4b"
                    radius: 6
                    border.color: "#5b21b6"
                    border.width: 1
                }
                
                contentItem: Label {
                    text: parent.text
                    color: "#a78bfa"
                    font.pixelSize: 12
                }
                
                popup: Popup {
                    width: parent.width
                    contentItem: ListView {
                        model: parent.popup.parent.model
                        clip: true
                        implicitHeight: contentHeight + 20
                        delegate: ItemDelegate {
                            width: parent.width
                            contentItem: Label {
                                text: modelData
                                color: "#c4b5fd"
                            }
                            background: Rectangle {
                                color: parent.down ? "#4c1d95" : "transparent"
                            }
                        }
                    }
                }
            }
        }
        
        // Live stats
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            color: "transparent"
            border.color: "#5b21b6"
            border.width: 1
            radius: 8
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                
                Label {
                    text: "📊 Live Stats"
                    font.bold: true
                    color: "#a78bfa"
                }
                
                Grid {
                    Layout.fillWidth: true
                    columns: 3
                    rowSpacing: 5
                    columnSpacing: 5
                    
                    Label {
                        text: "Active"
                        font.pixelSize: 10
                        color: "#94a3b8"
                    }
                    Label {
                        text: "NPS"
                        font.pixelSize: 10
                        color: "#94a3b8"
                    }
                    Label {
                        text: "BPM"
                        font.pixelSize: 10
                        color: "#94a3b8"
                    }
                    
                    Label {
                        text: mainWindow.activeVoices.toString()
                        font.pixelSize: 16
                        font.bold: true
                        color: "#c4b5fd"
                    }
                    Label {
                        text: mainWindow.nps.toString()
                        font.pixelSize: 16
                        font.bold: true
                        color: "#c4b5fd"
                    }
                    Label {
                        text: mainWindow.bpm.toFixed(0)
                        font.pixelSize: 16
                        font.bold: true
                        color: "#c4b5fd"
                    }
                }
            }
        }
        
        // Volume
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            
            Label {
                text: "🔊"
                font.pixelSize: 16
            }
            
            Slider {
                id: volumeSlider
                Layout.fillWidth: true
                from: 0
                to: 100
                value: 80
                
                background: Rectangle {
                    color: "#1e1b4b"
                    radius: 2
                    height: 3
                }
                
                handle: Rectangle {
                    x: volumeSlider.leftPadding + volumeSlider.visualPosition * 
                       (volumeSlider.availableWidth - width)
                    y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                    implicitWidth: 10
                    implicitHeight: 10
                    radius: 5
                    color: "#a78bfa"
                }
                
                onMoved: {
                    mainWindow.setVolume(value)
                }
            }
        }
        
        // Colors mode
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            
            Label {
                text: "Colors"
                font.pixelSize: 12
                color: "#94a3b8"
            }
            
            Button {
                text: mainWindow.perTrackColors ? "Per Track" : "Per Channel"
                Layout.fillWidth: true
                
                background: Rectangle {
                    color: parent.pressed ? "#4c1d95" : "#5b21b6"
                    radius: 6
                }
                
                onClicked: {
                    mainWindow.setPerTrackColors(!mainWindow.perTrackColors)
                }
            }
        }
    }
    
    function formatTime(seconds) {
        var m = Math.floor(seconds / 60)
        var s = Math.floor(seconds % 60)
        return m + ":" + (s < 10 ? "0" : "") + s
    }
}