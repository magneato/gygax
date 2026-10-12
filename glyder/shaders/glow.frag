#version 450
#extension GL_EXT_multiview : require
layout(location = 0) in vec4 vColour;
layout(location = 1) in float vAcross;
layout(location = 2) in vec3 vWorld;
layout(location = 3) in float vCover;
layout(location = 0) out vec4 outColour;
void main() {
    // A hot core and a soft halo: lasers, tracers, sound rings. Blended additively.
    float a = exp(-vAcross * vAcross * 4.0) * 0.7 + exp(-vAcross * vAcross * 40.0) * 0.6;
    outColour = vec4(vColour.rgb * a * vColour.a * vCover, 0.0);
}
