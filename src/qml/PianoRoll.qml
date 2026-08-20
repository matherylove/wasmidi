import QtQuick
import QtQuick.Scene3D

Item {
    id: root
    property var mainWindow
    
    Rectangle {
        anchors.fill: parent
        color: "#07071a"
        
        // Piano roll canvas (WebGL)
        Canvas {
            id: pianoRollCanvas
            anchors.fill: parent
            antialiasing: false
            
            property var gl
            property var program
            property var vao
            property var vbo
            
            onAvailableChanged: {
                if (available) {
                    initializeGL()
                }
            }
            
            function initializeGL() {
                gl = getContext("webgl2", {
                    alpha: false,
                    antialias: false,
                    powerPreference: "high-performance"
                })
                
                if (!gl) {
                    console.log("WebGL2 not available")
                    return
                }
                
                // Inicializar shaders, buffers, etc.
            }
            
            onPaint: {
                if (!gl) return
                
                gl.viewport(0, 0, width, height)
                gl.clearColor(0.027, 0.027, 0.102, 1.0)
                gl.clear(gl.COLOR_BUFFER_BIT)
                
                // Renderizar notas
                renderNotes()
            }
            
            function renderNotes() {
                // Lógica de renderizado WebGL
            }
        }
        
        // Overlay de carga
        Rectangle {
            visible: !mainWindow || mainWindow.noteCount === 0
            anchors.fill: parent
            color: "#07071a"
            opacity: 0.9
            
            Column {
                anchors.centerIn: parent
                spacing: 15
                
                Text {
                    text: "🎵"
                    font.pixelSize: 48
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                
                Text {
                    text: "Drop a MIDI file here or click to open"
                    font.pixelSize: 16
                    color: "#c4b5fd"
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                
                Text {
                    text: ".mid / .midi · Format 0 & 1 · Black MIDI ready"
                    font.pixelSize: 12
                    color: "#94a3b8"
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }
}