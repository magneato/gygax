#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec4 inP0; // xyz start, w width (render metres)
layout(location = 1) in vec4 inP1; // xyz end, w intensity
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec2 inCorner; // x end (0 or 1), y side (-1 or 1)

layout(location = 0) out vec4 vColour;
layout(location = 1) out float vAcross;
layout(location = 2) out vec3 vWorld;
layout(location = 3) out float vCover;

void main() {
    vec3 eye = scene.views[gl_ViewIndex].eye.xyz;
    vec3 p = mix(inP0.xyz, inP1.xyz, inCorner.x);
    vec3 dir = inP1.xyz - inP0.xyz;
    float len = length(dir);
    dir = len > 1e-6 ? dir / len : vec3(1, 0, 0);
    vec3 toEye = eye - p;
    float dist = length(toEye);
    vec3 side = cross(dir, toEye);
    side = length(side) > 1e-6 ? normalize(side) : vec3(0, 1, 0);
    // Never thinner than about two pixels on a Quest panel; a widened line carries the same light
    // (glow) or the same coverage (strokes) as the thin one it stands for.
    float width = inP0.w;
    float minWidth = dist * 0.0022;
    vCover = min(1.0, width / max(minWidth, 1e-6));
    float intensity = inP1.w;
    width = max(width, minWidth);
    p += side * (width * 0.5 * inCorner.y) + dir * (width * 0.5 * (inCorner.x * 2.0 - 1.0));
    vColour = vec4(srgbToLinear(inColour.rgb) * intensity, inColour.a);
    vAcross = inCorner.y;
    vWorld = p;
    gl_Position = scene.views[gl_ViewIndex].viewProj * vec4(p, 1.0);
}
