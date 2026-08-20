#version 300 es
precision mediump float;

in vec4 vColor;
in float vIsBlack;

uniform float uHue;
uniform vec3 uActiveColor;
uniform float uIsActive;

out vec4 fragColor;

vec3 hsl2rgb(vec3 c) {
    vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z + c.y * (rgb - 0.5) * (1.0 - abs(2.0 * c.z - 1.0));
}

void main() {
    float lightness = vIsBlack > 0.5 ? 0.15 : 0.5;
    float saturation = 0.3;
    
    vec3 baseColor = hsl2rgb(vec3(uHue / 360.0, saturation, lightness));
    
    if (uIsActive > 0.5) {
        baseColor = mix(baseColor, uActiveColor, 0.7);
    }
    
    fragColor = vec4(baseColor, 1.0);
}