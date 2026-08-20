import QtQuick
import Wasmidi

Item {
    id: root
    required property var mainWindow
    signal openRequested()
    clip: true

    property bool dragActive: false
    property int fpsValue: 0
    property real hue: 230
    property var nodes: []

    function hsl2rgb(h,s,l) {
        s/=100; l/=100
        function k(n){ return (n+h/30)%12 }
        var a=s*Math.min(l,1-l)
        function f(n){ return l-a*Math.max(-1,Math.min(k(n)-3,Math.min(9-k(n),1))) }
        return [Math.round(f(0)*255),Math.round(f(8)*255),Math.round(f(4)*255)]
    }
    function rgba(c,a){ return "rgba("+c[0]+","+c[1]+","+c[2]+","+a+")" }

    Component.onCompleted: {
        var a=[]
        for(var i=0;i<95;i++){
            var ang=Math.random()*Math.PI*2
            a.push({x:Math.random(),y:Math.random(),r:.7+Math.random()*1.2,p:Math.random()*Math.PI*2,ps:.006+Math.random()*.014,sa:Math.random()*Math.PI*2,ss:.003+Math.random()*.004})
        }
        nodes=a
    }

    Rectangle {
        anchors.fill: parent
        radius: 7
        color: "#050511"
        border.color: "#151127"

        Canvas {
            id: neural
            anchors.fill: parent
            z: 0
            onPaint: {
                var ctx=getContext("2d"),W=width,H=height
                if(W<2||H<2)return
                var act=Math.min(1,root.mainWindow.neuralActivity||0)
                var diff=(root.mainWindow.dominantHue||230)-root.hue
                if(diff>180)diff-=360;if(diff<-180)diff+=360
                root.hue=(root.hue+diff*.02+360)%360

                var c1=root.hsl2rgb(root.hue,52,3+act*4)
                var c2=root.hsl2rgb((root.hue+55)%360,48,5+act*5)
                var g=ctx.createLinearGradient(0,0,W,H)
                g.addColorStop(0,root.rgba(c1,1));g.addColorStop(1,root.rgba(c2,1))
                ctx.fillStyle=g;ctx.fillRect(0,0,W,H)

                var blobs=[{x:.15,y:.2,r:.3,o:0},{x:.82,y:.75,r:.28,o:50}]
                for(var bi=0;bi<blobs.length;bi++){
                    var b=blobs[bi],bc=root.hsl2rgb((root.hue+b.o)%360,65,28+act*14)
                    var rg=ctx.createRadialGradient(W*b.x,H*b.y,0,W*b.x,H*b.y,W*b.r)
                    rg.addColorStop(0,root.rgba(bc,.09+act*.07));rg.addColorStop(1,"rgba(0,0,0,0)")
                    ctx.fillStyle=rg;ctx.fillRect(0,0,W,H)
                }

                var line=root.hsl2rgb((root.hue+25)%360,70,65)
                for(var i=0;i<nodes.length;i++)for(var j=i+1;j<nodes.length;j++){
                    var dx=(nodes[i].x-nodes[j].x)*W,dy=(nodes[i].y-nodes[j].y)*H,d=Math.sqrt(dx*dx+dy*dy)
                    if(d<130){
                        var al=(1-d/130)*(.28+act*.22)
                        ctx.beginPath();ctx.moveTo(nodes[i].x*W,nodes[i].y*H);ctx.lineTo(nodes[j].x*W,nodes[j].y*H)
                        ctx.strokeStyle=root.rgba(line,al);ctx.lineWidth=(1-d/130)*.9;ctx.stroke()
                    }
                }

                var nc=root.hsl2rgb((root.hue+35)%360,78,68),spd=.28+act*2.2
                for(var n=0;n<nodes.length;n++){
                    var v=nodes[n],x=v.x*W,y=v.y*H,pr=v.r*(.8+.2*Math.sin(v.p))*(1+act*.5)
                    var cr=ctx.createRadialGradient(x,y,0,x,y,pr+1.2)
                    cr.addColorStop(0,root.rgba(nc,.95));cr.addColorStop(1,"rgba(0,0,0,0)")
                    ctx.beginPath();ctx.arc(x,y,pr,0,Math.PI*2);ctx.fillStyle=cr;ctx.fill()
                    v.sa+=v.ss*(1+act*6);v.x+=Math.cos(v.sa)*spd/Math.max(1,W);v.y+=Math.sin(v.sa)*spd/Math.max(1,H);v.p+=v.ps
                    if(v.x<-.04)v.x=1.04;if(v.x>1.04)v.x=-.04;if(v.y<-.06)v.y=1.06;if(v.y>1.06)v.y=-.06
                }
            }
        }

        PianoRollSurface {
            id: surface
            anchors.fill: parent
            anchors.margins: 1
            controller: root.mainWindow
            z: 1
        }

        Column {
            anchors.centerIn: parent
            spacing: 7
            visible: !root.mainWindow.hasMidi && !root.dragActive
            z: 4
            Text { anchors.horizontalCenter: parent.horizontalCenter; text:"♬"; color:"#4e386d"; font.pixelSize:34 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text:"Drop a MIDI file here or click to open"; color:"#706182"; font.pixelSize:11 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text:"Format 0 / 1 · Black MIDI ready"; color:"#443a52"; font.pixelSize:8 }
        }

        Rectangle {
            anchors.fill: parent
            visible: root.dragActive
            color: "#241044dd"; radius:7; border.color:"#9f7aea"; border.width:2; z:10
            Text { anchors.centerIn: parent; text:"Drop MIDI to load"; color:"#c4b5fd"; font.pixelSize:15; font.bold:true }
        }

        Text {
            anchors.left: parent.left; anchors.bottom: parent.bottom
            anchors.leftMargin:7; anchors.bottomMargin:5
            text: root.fpsValue+" FPS"; color:"#76668e"; font.pixelSize:7; font.bold:true; z:6
        }

        MouseArea { anchors.fill: parent; enabled: !root.mainWindow.hasMidi; cursorShape: Qt.PointingHandCursor; onClicked: root.openRequested() }
        DropArea {
            anchors.fill: parent
            onEntered: (drag)=>{root.dragActive=true;drag.acceptProposedAction()}
            onExited: root.dragActive=false
            onDropped: (drop)=>{
                root.dragActive=false
                if(drop.urls&&drop.urls.length>0)root.mainWindow.loadMidiUrl(drop.urls[0]);else root.openRequested()
                drop.acceptProposedAction()
            }
        }
    }

    FrameAnimation {
        running: root.visible
        onTriggered: {
            root.fpsValue=smoothFrameTime>0?Math.round(1/smoothFrameTime):0
            neural.requestPaint()
        }
    }
}
