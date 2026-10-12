#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColour;

float hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main() {
    ViewData vd = scene.views[gl_ViewIndex];
    vec4 near = vd.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(near.xyz / near.w - vd.eye.xyz);
    vec3 colour = skyColour(dir);

    // Stars: one candidate per cell of a fine direction grid, a soft point inside it.
    float elevation = dot(dir, scene.worldUp.xyz);
    if (scene.skyZenith.w > 0.0 && elevation > -0.02) {
        const float cells = 220.0;
        vec3 g = dir * cells;
        vec3 cell = floor(g);
        float h = hash(cell);
        if (h > 0.985) {
            vec3 centre = cell + 0.5 + (vec3(hash(cell + 1.3), hash(cell + 2.7), hash(cell + 4.1)) - 0.5) * 0.6;
            float d = length(g - centre);
            float mag = pow((h - 0.985) / 0.015, 3.0);
            float star = exp(-d * d * 18.0) * mag;
            colour += vec3(0.85, 0.9, 1.0) * star * scene.skyZenith.w * smoothstep(-0.02, 0.25, elevation);
        }
    }
    // Moon or sun: a disc with a halo.
    float c = dot(dir, normalize(scene.moon.xyz));
    float r = scene.moon.w;
    if (r > 0.0) {
        float a = acos(clamp(c, -1.0, 1.0));
        float disc = 1.0 - smoothstep(r * 0.92, r, a);
        colour += scene.sunColor.rgb * (disc * 6.0 + exp(-a / (r * 4.0)) * 0.25);
    }
    outColour = vec4(colour, 1.0);
}
