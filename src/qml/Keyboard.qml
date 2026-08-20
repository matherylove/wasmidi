import QtQuick

Item {
    id: root
    property var mainWindow
    
    Rectangle {
        anchors.fill: parent
        color: "#07071a"
        
        Canvas {
            id: keyboardCanvas
            anchors.fill: parent
            antialiasing: true
            
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                
                // Dibujar teclas
                drawKeyboard(ctx)
            }
            
            function drawKeyboard(ctx) {
                const whiteKeyWidth = width / 52
                const blackKeyWidth = whiteKeyWidth * 0.58
                const blackKeyHeight = height * 0.62
                
                const isBlack = [0,1,0,1,0,0,1,0,1,0,1,0]
                let whiteIndex = 0
                
                for (let octave = 0; octave < 10; ++octave) {
                    for (let i = 0; i < 12; ++i) {
                        const note = octave * 12 + i
                        const isBlackKey = isBlack[i] !== 0
                        
                        if (!isBlackKey) {
                            const x = whiteIndex * whiteKeyWidth
                            
                            // Tecla blanca
                            ctx.fillStyle = "#e2e8f0"
                            ctx.fillRect(x, 0, whiteKeyWidth - 1, height)
                            
                            ++whiteIndex
                        } else {
                            const x = (whiteIndex - 1) * whiteKeyWidth + 
                                    whiteKeyWidth * 0.5 - blackKeyWidth * 0.5
                            
                            // Tecla negra
                            ctx.fillStyle = "#1e293b"
                            ctx.fillRect(x, 0, blackKeyWidth, blackKeyHeight)
                        }
                    }
                }
            }
        }
    }
}