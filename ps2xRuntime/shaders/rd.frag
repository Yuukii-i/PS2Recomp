#version 450

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUv;

layout(set = 0, binding = 0) uniform sampler2D uSampler;
layout(set = 0, binding = 1) uniform sampler2D uPalTex;

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

layout(location = 0) out vec4 finalColor;

void main() {
    vec3 rgb = vCol.rgb;
    float a_ps2 = vCol.a * 255.0;
    if (pc.uTex != 0) {
        vec4 t = texture(uSampler, vUv);
        if (pc.uPalMode == 1) {
            float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));
            if (pc.uPalFallback != 0)
                t.rgb = vec3(0.25, 0.25, 0.45) * l + vec3(0.031, 0.031, 0.055); // FIX-FXC5
            else
                t = texture(uPalTex, vec2((floor(l * 255.0) + 0.5) / 256.0, 0.5));
        }
        if (pc.uPalMode != 0)
            t.a = 128.0 / 255.0;
        float t_a_ps2 = t.a * 255.0;
        vec3 vc_rgb = vCol.rgb * 255.0;
        float vc_a = vCol.a * 255.0;
        if (pc.uTfx == 0) {
            rgb = t.rgb * vc_rgb / 128.0;
            a_ps2 = (pc.uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;
        } else if (pc.uTfx == 1) {
            rgb = t.rgb;
            a_ps2 = (pc.uTcc != 0) ? t_a_ps2 : vc_a;
        } else if (pc.uTfx == 2) {
            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);
            a_ps2 = (pc.uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;
        } else {
            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);
            a_ps2 = (pc.uTcc != 0) ? t_a_ps2 : vc_a;
        }
        rgb = clamp(rgb, 0.0, 1.0);
    }
    if (pc.uAtst != 1) {
        int a = int(clamp(a_ps2, 0.0, 255.0) + 0.5);
        bool pass = true;
        if (pc.uAtst == 0) pass = false;
        else if (pc.uAtst == 2) pass = (a < pc.uAref);
        else if (pc.uAtst == 3) pass = (a <= pc.uAref);
        else if (pc.uAtst == 4) pass = (a == pc.uAref);
        else if (pc.uAtst == 5) pass = (a >= pc.uAref);
        else if (pc.uAtst == 6) pass = (a > pc.uAref);
        else if (pc.uAtst == 7) pass = (a != pc.uAref);
        if (pc.uAtInvert != 0) pass = !pass;
        if (!pass) discard;
    }
    finalColor = vec4(rgb, (pc.uFixAlpha >= 0.0) ? min(pc.uFixAlpha, 1.0) : min(a_ps2 / 128.0, 1.0));
}
