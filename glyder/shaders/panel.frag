#version 450
#extension GL_EXT_multiview : require
layout(location = 0) in vec4 vLocal;
layout(location = 1) flat in vec4 vShape;
layout(location = 2) flat in vec4 vFill;
layout(location = 3) flat in vec4 vBorder;
layout(location = 0) out vec4 outColour;

void main() {
    if (vShape.w < 0.0) {
        // A vignette: dark outside a soft circle of radius shape.x.
        float d = length(vLocal.xy) - vShape.x;
        outColour = vec4(vFill.rgb, vFill.a * smoothstep(0.0, -vShape.w, d));
        return;
    }
    // Signed distance to a rounded rectangle, in metres.
    vec2 q = abs(vLocal.xy) - vLocal.zw + vShape.x;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - vShape.x;
    float aa = max(fwidth(d), 1e-5) + vShape.z;
    float inside = 1.0 - smoothstep(-aa, 0.0, d);
    float border = vShape.y > 0.0 ? (1.0 - smoothstep(vShape.y - aa, vShape.y, -d)) * inside : 0.0;
    float glow = vShape.w > 0.0 ? exp(-max(d, 0.0) / vShape.w) * (1.0 - inside) * 0.6 : 0.0;
    vec3 rgb = mix(vFill.rgb, vBorder.rgb, border);
    float a = mix(vFill.a, vBorder.a, border) * inside + vBorder.a * glow;
    outColour = vec4(inside > 0.0 ? rgb : vBorder.rgb, a);
}
