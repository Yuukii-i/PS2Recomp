#version 450

layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec2 aUv;

layout(push_constant) uniform PC {
    vec4 uFlip;
    vec4 uVpA;
    vec4 uVpB;
    vec2 uFbSize;
    vec2 uVpZ;
    float uZBias;
    float uFixAlpha;
    int uMode2d;
    int uAtst;
    int uAref;
    int uAtInvert;
    int uTex;
    int uTfx;
    int uTcc;
    int uPalMode;
    int uPalFallback;
    int _pad0;
} pc;

layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUv;

void main() {
    if (pc.uMode2d != 0) {
        float ndcZ = (pc.uMode2d == 2 && abs(pc.uVpZ.x) > 1e-6)
                         ? clamp(((aPos.z - pc.uVpZ.y) / pc.uVpZ.x) * pc.uFlip.z, -1.0, 1.0)
                         : 0.0;
        gl_Position = vec4(aPos.x / pc.uFbSize.x * 2.0 - 1.0,
                           1.0 - aPos.y / pc.uFbSize.y * 2.0,
                           ndcZ, 1.0);
    } else if (pc.uVpB.w > 0.5) {
        float px = aPos.x * pc.uVpA.x + aPos.w * pc.uVpA.z - aPos.w * pc.uVpB.x * (1.0 / 16.0);
        float py = aPos.y * pc.uVpA.y + aPos.w * pc.uVpA.w - aPos.w * pc.uVpB.y * (1.0 / 16.0);
        float nx = (2.0 * px / pc.uFbSize.x - aPos.w) / pc.uVpB.z;
        float ny = aPos.w - 2.0 * py / pc.uFbSize.y;
        gl_Position = vec4(nx * pc.uFlip.x, ny * (-pc.uFlip.y), aPos.z * pc.uFlip.z, aPos.w);
    } else {
        gl_Position = vec4(aPos.x * pc.uFlip.x, aPos.y * pc.uFlip.y, aPos.z * pc.uFlip.z, aPos.w);
    }
    if (pc.uMode2d == 0 && pc.uZBias != 0.0) {
        gl_Position.z -= pc.uZBias * gl_Position.w;
    }
    gl_Position.y = -gl_Position.y;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    vCol = aCol;
    vUv = aUv;
}
