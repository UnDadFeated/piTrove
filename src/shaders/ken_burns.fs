#version 300 es
precision mediump float;
precision highp int;

in vec2 vTexCoord;
in vec2 vCenter;
out vec4 fragColor;

uniform sampler2D uTexture;
uniform float uTime;
uniform float uZoomStart;
uniform float uZoomEnd;
uniform float uPanX;
uniform float uPanY;
uniform float uResolutionX;
uniform float uResolutionY;

void main() {
    float t = uTime;
    t = t * t * (3.0 - 2.0 * t);

    float zoom = mix(uZoomStart, uZoomEnd, t);
    float pan_x = mix(0.0, uPanX, t);
    float pan_y = mix(0.0, uPanY, t);

    vec2 uv = vTexCoord;
    uv -= 0.5;
    uv *= zoom;
    uv += vec2(pan_x, pan_y);
    uv += 0.5;

    vec4 color = texture(uTexture, clamp(uv, 0.0, 1.0));

    float dist = length(vTexCoord - 0.5);
    float vignette = 1.0 - dist * 0.3;
    color.rgb *= vignette;

    fragColor = color;
}
