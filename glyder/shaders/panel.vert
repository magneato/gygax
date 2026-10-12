#version 450
#extension GL_EXT_multiview : require
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inLocal;   // xy position from the centre (m), zw half size (m)
layout(location = 2) in vec4 inShape;   // x corner radius, y border width, z softness, w glow
layout(location = 3) in vec4 inFill;
layout(location = 4) in vec4 inBorder;

layout(location = 0) out vec4 vLocal;
layout(location = 1) flat out vec4 vShape;
layout(location = 2) flat out vec4 vFill;
layout(location = 3) flat out vec4 vBorder;

void main() {
    vLocal = inLocal;
    vShape = inShape;
    vFill = vec4(srgbToLinear(inFill.rgb), inFill.a);
    vBorder = vec4(srgbToLinear(inBorder.rgb), inBorder.a);
    gl_Position = scene.views[gl_ViewIndex].viewProj * vec4(inPos, 1.0);
}
