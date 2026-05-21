#version 300 es
precision mediump float;
precision highp int;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTexture;
uniform float uTime;
uniform vec3 uBiasLight;
uniform float uBiasWidth;
uniform float uVignette;
uniform float uScanlineIntensity;
uniform vec2 uResolution;

void main() {
    vec4 color = texture(uTexture, vTexCoord);

    float dist_left = vTexCoord.x;
    float dist_right = 1.0 - vTexCoord.x;
    float dist_top = vTexCoord.y;
    float dist_bottom = 1.0 - vTexCoord.y;
    float edge_dist = min(min(dist_left, dist_right), min(dist_top, dist_bottom));
    float glow = smoothstep(uBiasWidth, 0.0, edge_dist);
    color.rgb += uBiasLight * glow * 0.15;

    float corner_dist = length(vTexCoord - 0.5);
    float vignette_factor = 1.0 - corner_dist * uVignette;
    color.rgb *= vignette_factor;

    float scanline = sin(vTexCoord.y * uResolution.y * 0.5) * 0.5 + 0.5;
    color.rgb *= 1.0 - uScanlineIntensity * (1.0 - scanline) * 0.05;

    fragColor = color;
}
