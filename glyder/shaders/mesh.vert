#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec4 iRow0;
layout(location = 4) in vec4 iRow1;
layout(location = 5) in vec4 iRow2;
layout(location = 6) in vec4 iTint;   // team colour (sRGB), a = highlight
layout(location = 7) in vec4 iParams; // x emissive gain / 4, y fade, z contour, w ghost

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColour;
layout(location = 3) flat out vec4 vMaterial; // x code, y highlight, z emissive gain, w contour
layout(location = 4) flat out vec3 vTint;

void main() {
    mat4 model = transpose(mat4(iRow0, iRow1, iRow2, vec4(0, 0, 0, 1)));
    vec4 world = model * vec4(inPos, 1.0);
    vWorld = world.xyz;
    vNormal = normalize(mat3(model) * inNormal.xyz);
    float code = floor(inColour.a * 255.0 / 64.0 + 0.5);
    vec3 tint = srgbToLinear(iTint.rgb);
    vColour = (code == 1.0 || code == 3.0) ? tint : srgbToLinear(inColour.rgb);
    vTint = tint;
    vMaterial = vec4(code, iTint.a, iParams.x * 4.0, iParams.z);
    gl_Position = scene.views[gl_ViewIndex].viewProj * world;
}
