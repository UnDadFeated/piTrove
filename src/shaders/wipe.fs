#version 300 es
precision mediump float;
precision highp int;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTexture;
uniform float uProgress;
uniform vec2 uDirection;

void main() {
    float wipe = dot(vTexCoord, uDirection);
    float edge = uProgress;
    float alpha = smoothstep(edge - 0.05, edge + 0.05, wipe);
    vec4 color = texture(uTexture, vTexCoord);
    fragColor = vec4(color.rgb, color.a * alpha);
}
