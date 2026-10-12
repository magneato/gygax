#version 450
#extension GL_EXT_multiview : require

// Vision pre-compensation. The eye adds a predictable elliptical blur at the headset's focal
// distance (glyder/rx.hpp); this boosts exactly the detail that blur removes, along its two axes,
// so what lands on the retina is closer to the scene. Only the central field gets it: that is
// where you look and where your acuity is.

struct PostEye {
    vec4 axes;    // major axis direction (x, y in pixels), sigma major, sigma minor (pixels)
    vec4 gains;   // gain major, gain minor, foveal radius (tangent), enabled
    vec4 frustum; // tan left, tan width, tan up, tan height
    vec4 texel;   // 1/width, 1/height
};
layout(std140, set = 0, binding = 0) uniform Post { PostEye eye[2]; } post;
layout(set = 0, binding = 1) uniform sampler2DArray scene;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColour;

vec3 tap(vec2 uv) { return texture(scene, vec3(uv, float(gl_ViewIndex))).rgb; }

// A 9-tap Gaussian along one direction, sampling between texels.
vec3 blurAlong(vec2 uv, vec2 stepUv, float sigma) {
    if (sigma < 0.3) return tap(uv);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    float spacing = sigma * 0.5;
    for (int k = -4; k <= 4; ++k) {
        float x = float(k) * spacing;
        float w = exp(-x * x / (2.0 * sigma * sigma));
        acc += tap(uv + stepUv * x) * w;
        wsum += w;
    }
    return acc / wsum;
}

void main() {
    PostEye e = post.eye[gl_ViewIndex];
    vec3 c = tap(vUv);
    if (e.gains.w < 0.5) {
        outColour = vec4(c, 1.0);
        return;
    }
    // Where this pixel looks, as a tangent from the eye's optical axis.
    vec2 t = vec2(e.frustum.x + vUv.x * e.frustum.y, e.frustum.z - vUv.y * e.frustum.w);
    float weight = 1.0 - smoothstep(e.gains.z * 0.75, e.gains.z, length(t));
    if (weight <= 0.0) {
        outColour = vec4(c, 1.0);
        return;
    }
    vec2 major = e.axes.xy * e.texel.xy;
    vec2 minor = vec2(-e.axes.y, e.axes.x) * e.texel.xy;
    vec3 bMajor = blurAlong(vUv, major, e.axes.z);
    vec3 bMinor = blurAlong(vUv, minor, e.axes.w);
    vec3 sharp = c + e.gains.x * (c - bMajor) + e.gains.y * (c - bMinor);
    // Keep the tonal range: compress overshoot softly instead of clipping it.
    sharp = mix(sharp, clamp(sharp, 0.0, 1.0), 0.85);
    outColour = vec4(mix(c, clamp(sharp, 0.0, 1.0), weight), 1.0);
}
