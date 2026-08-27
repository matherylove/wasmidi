#version 300 es
precision highp float;

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec4 noteData;  // start, end, pitch, channel

uniform float uCurrentTime;
uniform float uWindowSeconds;
uniform float uPostBuffer;
uniform vec2 uResolution;

out vec4 vColor;
out float vActive;

void main() {
    float noteStart = noteData.x;
    float noteEnd = noteData.y;
    float pitch = noteData.z;
    float channel = noteData.w;
    
    float relativeStart = noteStart - uCurrentTime;
    float relativeEnd = noteEnd - uCurrentTime;
    
    float xMin = (relativeStart - uPostBuffer) / uWindowSeconds;
    float xMax = (relativeEnd - uPostBuffer) / uWindowSeconds;
    
    float x = mix(xMin, xMax, vertexPosition.x);
    float y = pitch / 127.0;
    
    vec2 pos = vec2(
        x * 2.0 - 1.0,
        y * 2.0 - 1.0
    );
    
    gl_Position = vec4(pos, 0.0, 1.0);
    
    vActive = float(relativeStart < 0.0 && relativeEnd > 0.0);
    vColor = vec4(1.0, 1.0, 1.0, 1.0);
}