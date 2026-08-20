#version 300 es
precision mediump float;

in vec4 vColor;
in float vActive;

uniform vec3 uNoteColor;
uniform float uActiveMultiplier;

out vec4 fragColor;

void main() {
    float multiplier = vActive > 0.5 ? uActiveMultiplier : 1.0;
    vec3 color = uNoteColor * multiplier;
    fragColor = vec4(color, 0.9);
}