#version 450

layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec2 aUv;
layout(location = 3) in float aFog; // FIX-E1c

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
    vec4 uFog; // FIX-E1c
} pc;

layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUv;
layout(location = 2) out float vFog; // FIX-E1c
layout(location = 3) flat out int vAtInvert; // FIX-FG8

// FIX-M12: Direct 3D path receives clip-space (x,y,z,w); VU1 converts to
// prim-space pixels, ftoi4-packs (saturating at +-2^31), and GIF keeps low
// 16 bits as unsigned 12.4 fixed point in [0, 4096).
float wrapGsCoord(float num, float w, float prim) {
    if (w == 0.0 || isnan(prim) || isinf(prim)) {
        if (num == 0.0 && w == 0.0) {
            prim = 0.0;
        } else {
            uint signXor = (floatBitsToUint(num) ^ floatBitsToUint(w)) & 0x80000000u;
            prim = (signXor != 0u) ? -3.4e38 : 3.4e38;
        }
    }
    float v16 = prim * 16.0;
    if (v16 >= 2147483648.0) return 4095.9375;
    if (v16 <= -2147483648.0) return 0.0;
    return prim - 4096.0 * floor(prim / 4096.0);
}

void main() {
    if (pc.uMode2d != 0) {
        float ndcZ = (pc.uMode2d == 2 && abs(pc.uVpZ.x) > 1e-6)
                         ? clamp(((aPos.z - pc.uVpZ.y) / pc.uVpZ.x) * pc.uFlip.z, -1.0, 1.0)
                         : 0.0;
        gl_Position = vec4(aPos.x / pc.uFbSize.x * 2.0 - 1.0,
                           1.0 - aPos.y / pc.uFbSize.y * 2.0,
                           ndcZ, 1.0);
    } else if (pc.uVpB.w > 0.5) {
        float numX = aPos.x * pc.uVpA.x + aPos.w * pc.uVpA.z;
        float numY = aPos.y * pc.uVpA.y + aPos.w * pc.uVpA.w;
        float primX = numX / aPos.w;
        float primY = numY / aPos.w;
        float px = numX - aPos.w * pc.uVpB.x * (1.0 / 16.0);
        float py = numY - aPos.w * pc.uVpB.y * (1.0 / 16.0);
        if (aPos.w == 0.0 || isnan(primX) || isinf(primX) || primX < 0.0 || primX >= 4096.0
                          || isnan(primY) || isinf(primY) || primY < 0.0 || primY >= 4096.0) { // FIX-M12
            px = wrapGsCoord(numX, aPos.w, primX) * aPos.w - aPos.w * pc.uVpB.x * (1.0 / 16.0);
            py = wrapGsCoord(numY, aPos.w, primY) * aPos.w - aPos.w * pc.uVpB.y * (1.0 / 16.0);
        }
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
    vFog = aFog; // FIX-E1c
    vAtInvert = int(gl_InstanceIndex); // FIX-FG8
}
