#version 450
#extension GL_EXT_multiview : require
layout(location = 0) in vec4 vColour;
layout(location = 1) in float vAcross;
layout(location = 2) in vec3 vWorld;
layout(location = 3) in float vCover;
layout(location = 0) out vec4 outColour;
void main() {
    // Crisp strokes for text and HUD, anti-aliased across the stroke.
    // Coverage from the distance to the stroke's edge in pixels: exact for thin and thick strokes.
    float halfWidthPx = 1.0 / max(fwidth(vAcross), 1e-4);
    float a = clamp((1.0 - abs(vAcross)) * halfWidthPx + 0.5, 0.0, 1.0);
    // Thin strokes keep most of their weight: text must stay legible at any distance.
    outColour = vec4(vColour.rgb, vColour.a * a * mix(0.55, 1.0, vCover));
}
