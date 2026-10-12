// Shared by every glyder shader. Render frame: x right, y up, -z forward; metres (scaled in god view).

struct ViewData {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 eye; // xyz eye position, w unused
};

layout(std140, set = 0, binding = 0) uniform Scene {
    ViewData views[2];
    vec4 sunDir;      // xyz towards the key light, w intensity
    vec4 sunColor;    // rgb, w rim strength
    vec4 skyZenith;   // rgb, w star brightness
    vec4 skyHorizon;  // rgb, w fog density per render metre
    vec4 groundColor; // rgb ambient from below, w time in seconds
    vec4 moon;        // xyz direction of the disc, w angular radius (radians)
    vec4 contour;     // x spacing (world m), y strength, z ground height (render y), w render metres per world metre
    vec4 contourColor; // rgb, w unused
    vec4 ambientSky;    // rgb light from above, w exposure
    vec4 ambientGround; // rgb light from below, w unused
    vec4 worldUp;       // xyz the world's up in the render frame (it rolls when the world does)
} scene;

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 skyColour(vec3 dir) {
    float up = dot(dir, scene.worldUp.xyz);
    float h = clamp(up, 0.0, 1.0);
    vec3 sky = mix(scene.skyHorizon.rgb, scene.skyZenith.rgb, pow(h, 0.45));
    // Below the horizon fade to the ground colour, softly, so a flat world has no hard seam.
    vec3 below = mix(scene.skyHorizon.rgb, scene.groundColor.rgb, clamp(-up * 4.0, 0.0, 1.0));
    return up >= 0.0 ? sky : below;
}

vec3 applyFog(vec3 colour, vec3 worldPos, vec3 eye) {
    vec3 d = worldPos - eye;
    float dist = length(d);
    float f = 1.0 - exp(-dist * scene.skyHorizon.w);
    return mix(colour, skyColour(d / max(dist, 1e-4)) * 0.9, clamp(f, 0.0, 1.0));
}
