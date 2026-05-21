#version 300 es
precision mediump float;
precision highp int;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTexture;
uniform float uPixelSize;
uniform vec2 uResolution;

void main() {
    float pixel_size = uPixelSize / uResolution.x;
    vec2 pixelated_uv = floor(vTexCoord / pixel_size) * pixel_size + pixel_size * 0.5;
    vec4 color = texture(uTexture, clamp(pixelated_uv, 0.0, 1.0));
    fragColor = color;
}
