#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColour;
layout(location = 3) flat in vec4 vMaterial;
layout(location = 4) flat in vec3 vTint;
layout(location = 0) out vec4 outColour;

void main() {
    vec3 eye = scene.views[gl_ViewIndex].eye.xyz;
    vec3 n = normalize(vNormal);
    vec3 v = normalize(eye - vWorld);
    float code = vMaterial.x;
    vec3 colour;
    if (code >= 2.0) {
        // Emitters: self-lit, a little hotter at grazing angles so lenses and exhausts glint.
        colour = vColour * (vMaterial.z * (1.0 + 0.6 * pow(1.0 - max(dot(n, v), 0.0), 2.0)));
    } else {
        vec3 l = normalize(scene.sunDir.xyz);
        float ndl = max(dot(n, l), 0.0);
        vec3 hemi = mix(scene.ambientGround.rgb, scene.ambientSky.rgb, dot(n, scene.worldUp.xyz) * 0.5 + 0.5);
        float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * scene.sunColor.w;
        colour = vColour * (hemi + scene.sunColor.rgb * ndl * scene.sunDir.w) + scene.skyHorizon.rgb * rim;
        // Contour lines on terrain: thin, anti-aliased, every `spacing` metres of world height.
        if (vMaterial.w > 0.5) {
            float h = (dot(vWorld, scene.worldUp.xyz) - scene.contour.z) / (scene.contour.w * scene.contour.x);
            float d = abs(fract(h - 0.5) - 0.5) / max(fwidth(h), 1e-5);
            float line = 1.0 - clamp(d - 0.25, 0.0, 1.0);
            float major = 1.0 - clamp(abs(fract(h * 0.2 - 0.5) - 0.5) / max(fwidth(h * 0.2), 1e-5) - 0.4, 0.0, 1.0);
            colour += scene.contourColor.rgb * scene.contour.y * (line * 0.55 + major * 0.9);
        }
    }
    // Selection: a breathing fresnel halo in the team colour.
    if (vMaterial.y > 0.0) {
        float pulse = 0.65 + 0.35 * sin(scene.groundColor.w * 6.0);
        colour += vTint * vMaterial.y * pulse * (0.25 + 1.5 * pow(1.0 - max(dot(n, v), 0.0), 2.0));
    }
    colour = applyFog(colour, vWorld, eye) * scene.ambientSky.w;
    outColour = vec4(colour, 1.0);
}
