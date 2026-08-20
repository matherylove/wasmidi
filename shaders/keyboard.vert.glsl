#version 300 es
precision highp float;

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec4 keyData;  // x, y, w, h

uniform vec2 uResolution;

out vec4 vColor;
out float vIsBlack;

void main() {
    float x = keyData.x;
    float y = keyData.y;
    float w = keyData.z;
    float h = keyData.w;
    
    vec2 pos = vec2(
        x + vertexPosition.x * w,
        y + vertexPosition.y * h
    );
    
    pos = pos * 2.0 - 1.0;
    
    gl_Position = vec4(pos, 0.0, 1.0);
    
    vIsBlack = keyData.z < 0.02 ? 1.0 : 0.0;
    vColor = vec4(1.0, 1.0, 1.0, 1.0);
}