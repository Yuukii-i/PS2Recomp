#version 450
#extension GL_ARB_fragment_shader_interlock : require
// PERF-VK V2: GS pixel pipeline in a fragment shader. Mirrors GSCpuBackend (DrawTriangle / DrawSprite generic path /
// WritePixel / SampleTexture / combineTexture). VRAM is a storage buffer holding the raw 4 MB GS local memory.
layout(pixel_interlock_ordered) in;

layout(std430, binding = 0) coherent buffer Vram { uint vram[]; };
layout(std430, binding = 1) readonly buffer Tables { uint tbl[]; };
layout(std430, binding = 2) readonly buffer Clut { uint clut[]; };
layout(std430, binding = 3) readonly buffer Prims { uint pd[]; };

layout(location = 0) flat in uint vPrim;

const uint STRIDE = 80u;
const uint X0 = 0u, Y0 = 3u, Z0 = 6u, Q0 = 12u, S0 = 15u, T0 = 18u, RGBA0 = 21u, UV0 = 24u, FOG0 = 27u;
const uint FLAGS = 30u, OFX = 31u, OFY = 32u, SCX0 = 33u, SCX1 = 34u, SCY0 = 35u, SCY1 = 36u;
const uint FBP = 37u, FBW = 38u, FPSM = 39u, FBMSK = 40u, ZBP = 41u, ZPSM = 42u;
const uint TEST = 43u, ALPHA = 44u, ALPHAFIX = 45u;
const uint TBP0 = 46u, TBW = 47u, TPSM = 48u, TCC = 49u, TFX = 50u, CPSM = 51u, CSM = 52u, CSA = 53u;
const uint TEXW = 54u, TEXH = 55u, CLAMP_LO = 56u, CLAMP_HI = 57u, TA0 = 58u, AEM = 59u, TA1 = 60u, FOGRGB = 61u;
const uint SPR_UX0 = 66u, SPR_UY0 = 67u, SPR_U0F = 70u, SPR_V0F = 71u, SPR_U1F = 72u, SPR_V1F = 73u;
const uint SPR_W = 74u, SPR_H = 75u, SPR_Z = 76u, FBA = 77u, CLUTSLOT = 78u;

const uint F_SPRITE = 1u, F_IIP = 2u, F_TME = 4u, F_FST = 8u, F_FGE = 16u, F_ABE = 32u, F_PABE = 64u, F_LINEAR = 128u;
const uint F_ZMASK = 256u;

// page-table offsets (in u16 entries) inside the Tables buffer; order fixed by ps2_vk_backend.cpp
const uint T_C32 = 0u, T_Z32 = 65536u, T_C16 = 131072u, T_C16S = 262144u, T_Z16 = 393216u, T_Z16S = 524288u;
const uint T_P8 = 655360u, T_P4 = 917504u;

uint P(uint i) { return pd[vPrim * STRIDE + i]; }
float PF(uint i) { return uintBitsToFloat(P(i)); }
int PI(uint i) { return int(P(i)); }

int clampU8(int v) { return clamp(v, 0, 255); }

uint pixelAddr(uint tableOff, uint pageW, uint pageH, uint pixPerPage, uint base, uint bw, uint x, uint y)
{
    const uint basePage = base / 32u;
    const uint row = (y / pageH) * ((bw * 64u) / pageW);
    const uint col = x / pageW;
    const uint page = basePage + row + col;
    const uint idx = tableOff + (((base % 32u) * pageH + (y % pageH)) * pageW + (x % pageW));
    uint e = tbl[idx >> 1];
    e = ((idx & 1u) != 0u) ? (e >> 16) : (e & 0xFFFFu);
    return page * pixPerPage + e;
}

uint readPsm(uint psm, uint base, uint bw, uint x, uint y)
{
    uint a;
    switch (psm)
    {
    case 0u: // CT32
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        return vram[a & 0xFFFFFu];
    case 1u: // CT24
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        return vram[a & 0xFFFFFu] & 0x00FFFFFFu;
    case 48u: // Z32
        a = pixelAddr(T_Z32, 64u, 32u, 2048u, base, bw, x, y);
        return vram[a & 0xFFFFFu];
    case 49u: // Z24
        a = pixelAddr(T_Z32, 64u, 32u, 2048u, base, bw, x, y);
        return vram[a & 0xFFFFFu] & 0x00FFFFFFu;
    case 2u: case 10u: case 50u: case 58u:
    {
        uint off = (psm == 2u) ? T_C16 : (psm == 10u) ? T_C16S : (psm == 50u) ? T_Z16 : T_Z16S;
        a = pixelAddr(off, 64u, 64u, 4096u, base, bw, x, y);
        const uint byteAddr = (a * 2u) & 0x3FFFFEu;
        return (vram[byteAddr >> 2] >> ((byteAddr & 2u) * 8u)) & 0xFFFFu;
    }
    case 19u: // T8
    {
        a = pixelAddr(T_P8, 128u, 64u, 8192u, base, bw, x, y);
        const uint byteAddr = a & 0x3FFFFFu;
        return (vram[byteAddr >> 2] >> ((byteAddr & 3u) * 8u)) & 0xFFu;
    }
    case 20u: // T4
    {
        a = pixelAddr(T_P4, 128u, 128u, 16384u, base, bw, x, y);
        const uint bits = a * 4u;
        const uint byteAddr = (bits >> 3) & 0x3FFFFFu;
        return (vram[byteAddr >> 2] >> ((byteAddr & 3u) * 8u + (bits & 7u))) & 0xFu;
    }
    case 27u: // T8H
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        return (vram[a & 0xFFFFFu] >> 24) & 0xFFu;
    case 36u: // T4HL
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        return (vram[a & 0xFFFFFu] >> 24) & 0xFu;
    case 44u: // T4HH
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        return (vram[a & 0xFFFFFu] >> 28) & 0xFu;
    }
    return 0xFFFF00FFu;
}

void writePsm(uint psm, uint base, uint bw, uint x, uint y, uint value)
{
    uint a;
    switch (psm)
    {
    case 0u:
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        vram[a & 0xFFFFFu] = value;
        return;
    case 48u:
        a = pixelAddr(T_Z32, 64u, 32u, 2048u, base, bw, x, y);
        vram[a & 0xFFFFFu] = value;
        return;
    case 1u:
    {
        a = pixelAddr(T_C32, 64u, 32u, 2048u, base, bw, x, y);
        const uint w = a & 0xFFFFFu;
        vram[w] = (vram[w] & 0xFF000000u) | (value & 0x00FFFFFFu);
        return;
    }
    case 49u:
    {
        a = pixelAddr(T_Z32, 64u, 32u, 2048u, base, bw, x, y);
        const uint w = a & 0xFFFFFu;
        vram[w] = (vram[w] & 0xFF000000u) | (value & 0x00FFFFFFu);
        return;
    }
    case 2u: case 10u: case 50u: case 58u:
    {
        uint off = (psm == 2u) ? T_C16 : (psm == 10u) ? T_C16S : (psm == 50u) ? T_Z16 : T_Z16S;
        a = pixelAddr(off, 64u, 64u, 4096u, base, bw, x, y);
        const uint byteAddr = (a * 2u) & 0x3FFFFEu;
        const uint sh = (byteAddr & 2u) * 8u;
        atomicAnd(vram[byteAddr >> 2], ~(0xFFFFu << sh));
        atomicOr(vram[byteAddr >> 2], (value & 0xFFFFu) << sh);
        return;
    }
    }
}

uint rgba5551To8888(uint c)
{
    const uint r = ((c >> 0) & 0x1Fu) << 3;
    const uint g = ((c >> 5) & 0x1Fu) << 3;
    const uint b = ((c >> 10) & 0x1Fu) << 3;
    const uint a = ((c >> 15) & 1u) << 7;
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint rgba8888To5551(uint c)
{
    const uint r = ((c >> 0) & 0xFFu) >> 3;
    const uint g = ((c >> 8) & 0xFFu) >> 3;
    const uint b = ((c >> 16) & 0xFFu) >> 3;
    const uint a = ((c >> 24) & 0xFFu) >> 7;
    return r | (g << 5) | (b << 10) | (a << 15);
}

uint applyTexa(uint psm, uint texel)
{
    if (psm == 0u)
        return texel;
    const uint r = texel & 0xFFu, g = (texel >> 8) & 0xFFu, b = (texel >> 16) & 0xFFu;
    const bool rgbZero = (r == 0u && g == 0u && b == 0u);
    uint a = (texel >> 24) & 0xFFu;
    const bool aem = P(AEM) != 0u;
    if (psm == 1u)
        a = (aem && rgbZero) ? 0u : P(TA0);
    else if (psm == 2u || psm == 10u)
    {
        if ((a & 0x80u) != 0u)
            a = P(TA1);
        else
            a = (aem && rgbZero) ? 0u : P(TA0);
    }
    return (texel & 0x00FFFFFFu) | (a << 24);
}

uint clutRead(uint i) { return (clut[P(CLUTSLOT) * 256u + (i >> 1)] >> ((i & 1u) * 16u)) & 0xFFFFu; }

uint lookupClut(uint index, uint cpsm, uint csa, uint sourcePsm)
{
    const bool sixteen = (cpsm == 2u || cpsm == 10u);
    const uint csaMask = sixteen ? 0x1Fu : 0x0Fu;
    const uint clutBase = (csa & csaMask) << 4;
    const bool four = (sourcePsm == 20u || sourcePsm == 36u || sourcePsm == 44u);
    const uint srcIndex = four ? (index & 0x0Fu) : index;
    const uint ci = (clutBase + srcIndex) & (sixteen ? 0x1FFu : 0x0FFu);
    if (cpsm == 0u)
        return applyTexa(cpsm, clutRead(ci) | (clutRead(ci + 256u) << 16));
    if (cpsm == 1u)
        return applyTexa(cpsm, (clutRead(ci) | (clutRead(ci + 256u) << 16)) & 0x00FFFFFFu);
    if (cpsm == 2u || cpsm == 10u)
        return applyTexa(cpsm, rgba5551To8888(clutRead(ci)));
    return 0xFFFF00FFu;
}

int wrapCoord(int coord, int size, uint mode, uint regMin, uint regMax)
{
    switch (mode & 3u)
    {
    case 0u: return int(uint(coord) & uint(size - 1));
    case 1u: return clamp(coord, 0, size - 1);
    case 2u: return min(max(coord, int(regMin)), int(regMax));
    default: return int((uint(coord) & regMin) | regMax);
    }
}

uint samplePoint(int su, int sv)
{
    const int texW = PI(TEXW), texH = PI(TEXH);
    const uint clampLo = P(CLAMP_LO), clampHi = P(CLAMP_HI);
    const uint wrapU = clampLo & 3u, wrapV = (clampLo >> 2) & 3u;
    const uint minU = (clampLo >> 4) & 0x3FFu;
    const uint maxU = ((clampLo >> 14) & 0x3FFu);
    const uint minV = ((clampLo >> 24) | (clampHi << 8)) & 0x3FFu;
    const uint maxV = (clampHi >> 2) & 0x3FFu;
    su = wrapCoord(su, texW, wrapU, minU, maxU);
    sv = wrapCoord(sv, texH, wrapV, minV, maxV);
    const uint psm = P(TPSM);
    const uint raw = readPsm(psm, P(TBP0), P(TBW), uint(su), uint(sv));
    switch (psm)
    {
    case 0u: case 1u:
        return applyTexa(psm, raw);
    case 48u: case 49u:
        return raw; // CPU applyTexa is the identity for Z32/Z24
    case 2u: case 10u:
        return applyTexa(psm, rgba5551To8888(raw));
    case 50u: case 58u:
        return rgba5551To8888(raw); // CPU applyTexa is the identity for Z16/Z16S
    case 19u: case 27u: case 20u: case 36u: case 44u:
        return lookupClut(raw & 0xFFu, P(CPSM), P(CSA), psm);
    }
    return 0xFFFF00FFu;
}

int roundClamped(float x)
{
    const int i = int(x);
    return i + (((x - float(i)) >= 0.5) ? 1 : 0);
}

int lerpChannel(int c00, int c10, int c01, int c11, float fx, float fy)
{
    const float top = float(c00) + (float(c10) - float(c00)) * fx;
    const float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    return clampU8(roundClamped(top + (bottom - top) * fy));
}

float fabsQ(float q) { return (abs(q) > 1.0e-8) ? q : 1.0; }

uint sampleTexture(float s, float t, float q, uint u, uint v, bool fst, bool linear)
{
    const int texW = PI(TEXW), texH = PI(TEXH);
    float texUf, texVf;
    if (fst)
    {
        texUf = float(u) / 16.0;
        texVf = float(v) / 16.0;
    }
    else
    {
        const float invQ = 1.0 / fabsQ(q);
        texUf = s * invQ * float(texW);
        texVf = t * invQ * float(texH);
    }
    if (!linear)
        return samplePoint(int(texUf), int(texVf));
    const float sU = texUf - 0.5, sV = texVf - 0.5;
    const int u0 = int(floor(sU)), v0 = int(floor(sV));
    const float fx = sU - float(u0), fy = sV - float(v0);
    const uint c00 = samplePoint(u0, v0), c10 = samplePoint(u0 + 1, v0);
    const uint c01 = samplePoint(u0, v0 + 1), c11 = samplePoint(u0 + 1, v0 + 1);
    uint out_ = 0u;
    for (uint ch = 0u; ch < 4u; ++ch)
    {
        const uint sh = ch * 8u;
        const int c = lerpChannel(int((c00 >> sh) & 0xFFu), int((c10 >> sh) & 0xFFu),
                                  int((c01 >> sh) & 0xFFu), int((c11 >> sh) & 0xFFu), fx, fy);
        out_ |= uint(c) << sh;
    }
    return out_;
}

ivec4 combineTexture(ivec4 vtx, ivec4 tex)
{
    const bool hasA = P(TCC) != 0u;
    ivec4 o = ivec4(tex.rgb, hasA ? tex.a : vtx.a);
    switch (P(TFX))
    {
    case 0u:
        o.rgb = ivec3(clampU8((tex.r * vtx.r) >> 7), clampU8((tex.g * vtx.g) >> 7), clampU8((tex.b * vtx.b) >> 7));
        o.a = hasA ? clampU8((tex.a * vtx.a) >> 7) : vtx.a;
        break;
    case 1u:
        break;
    case 2u:
        o.rgb = ivec3(clampU8(((tex.r * vtx.r) >> 7) + vtx.a), clampU8(((tex.g * vtx.g) >> 7) + vtx.a),
                      clampU8(((tex.b * vtx.b) >> 7) + vtx.a));
        o.a = hasA ? clampU8(tex.a + vtx.a) : vtx.a;
        break;
    case 3u:
        o.rgb = ivec3(clampU8(((tex.r * vtx.r) >> 7) + vtx.a), clampU8(((tex.g * vtx.g) >> 7) + vtx.a),
                      clampU8(((tex.b * vtx.b) >> 7) + vtx.a));
        o.a = hasA ? tex.a : vtx.a;
        break;
    default:
        break;
    }
    return o;
}

void rmw(int x, int y, int r, int g, int b, int a, uint zv, uint flags, uint test, uint fpsm, uint fbp, uint fbw, uint zbp, uint zpsm,
         bool writeRgb, bool writeAlpha, bool writeDepth, bool writesFb, uint zmethod, bool preserveDestAlpha, bool is16)
{
    uint rawFb = 0u;
    uint fbrgba = 0u;
    if (writesFb || (((test >> 14) & 1u) != 0u))
    {
        rawFb = readPsm(fpsm, fbp, fbw, uint(x), uint(y));
        fbrgba = rawFb;
        if (is16)
            fbrgba = rgba5551To8888(fbrgba);
        else if (fpsm == 1u)
            fbrgba |= 0x80000000u;
    }

    // destination alpha test
    if (((test >> 14) & 1u) != 0u)
    {
        const bool datm = ((test >> 15) & 1u) != 0u;
        bool dpass = true;
        if (fpsm == 0u)
            dpass = (((rawFb >> 31) & 1u) != 0u) == datm;
        else if (is16)
            dpass = (((rawFb >> 15) & 1u) != 0u) == datm;
        if (!dpass)
        {
            return;
        }
    }

    bool zpass = false;
    uint storedZ = 0u;
    if (zmethod == 1u)
        zpass = true;
    else if (zmethod >= 2u)
    {
        storedZ = readPsm(zpsm, zbp, fbw, uint(x), uint(y));
        zpass = (zmethod == 2u) ? (zv >= storedZ) : (zv > storedZ);
    }
    if (!zpass)
    {
        return;
    }

    if (writesFb)
    {
        const int srcR = r, srcG = g, srcB = b;
        if ((flags & F_ABE) != 0u)
        {
            const int dr = int(fbrgba & 0xFFu), dg = int((fbrgba >> 8) & 0xFFu), db = int((fbrgba >> 16) & 0xFFu), da = int((fbrgba >> 24) & 0xFFu);
            if (!((flags & F_PABE) != 0u && (a & 0x80) == 0))
            {
                const uint alphaReg = P(ALPHA);
                const uint asel = alphaReg & 3u, bsel = (alphaReg >> 2) & 3u, csel = (alphaReg >> 4) & 3u, dsel = (alphaReg >> 6) & 3u;
                const int fix = int(P(ALPHAFIX) & 0xFFu);
                const int cAlpha = (csel == 0u) ? a : (csel == 1u) ? da : fix;
                int cs[3] = int[3](r, g, b);
                int cd[3] = int[3](dr, dg, db);
                int o[3];
                for (int i = 0; i < 3; ++i)
                {
                    const int A = (asel == 0u) ? cs[i] : (asel == 1u) ? cd[i] : 0;
                    const int B = (bsel == 0u) ? cs[i] : (bsel == 1u) ? cd[i] : 0;
                    const int D = (dsel == 0u) ? cs[i] : (dsel == 1u) ? cd[i] : 0;
                    o[i] = clampU8((((A - B) * cAlpha) >> 7) + D);
                }
                r = o[0]; g = o[1]; b = o[2];
            }
            else
            {
                r = srcR; g = srcG; b = srcB;
            }
        }
        if (writeAlpha && ((P(FBA) & 1u) != 0u) && fpsm != 1u)
            a = a | 0x80;

        uint pixel = uint(r) | (uint(g) << 8) | (uint(b) << 16) | (uint(a) << 24);
        const uint fbmsk = P(FBMSK);
        if (fbmsk != 0u)
            pixel = (pixel & ~fbmsk) | (fbrgba & fbmsk);
        if (preserveDestAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        if (is16)
            pixel = rgba8888To5551(pixel);
        writePsm(fpsm, fbp, fbw, uint(x), uint(y), pixel);
    }

    if (writeDepth && (flags & F_ZMASK) == 0u)
        writePsm(zpsm, zbp, fbw, uint(x), uint(y), zv);

}

struct PixelIn
{
    int x, y, r, g, b, a;
    uint zv, flags, test, fpsm, fbp, fbw, zbp, zpsm;
    bool writeRgb, writeAlpha, writeDepth, writesFb;
    uint zmethod;
    bool preserveDestAlpha, is16;
};

bool shade(out PixelIn pin)
{
    const int x = int(gl_FragCoord.x);
    const int y = int(gl_FragCoord.y);
    const uint flags = P(FLAGS);
    const bool sprite = (flags & F_SPRITE) != 0u;
    const bool tme = (flags & F_TME) != 0u;
    const bool fst = (flags & F_FST) != 0u;
    const bool linear = (flags & F_LINEAR) != 0u;

    ivec4 col;
    uint zv;
    int fogv;

    if (sprite)
    {
        const uint rgba = pd[vPrim * STRIDE + RGBA0 + 1u];
        col = ivec4(int(rgba & 0xFFu), int((rgba >> 8) & 0xFFu), int((rgba >> 16) & 0xFFu), int((rgba >> 24) & 0xFFu));
        zv = P(SPR_Z);
        fogv = int(pd[vPrim * STRIDE + FOG0 + 1u]);
        if (tme)
        {
            const float spriteW = PF(SPR_W), spriteH = PF(SPR_H);
            const float u0f = PF(SPR_U0F), v0f = PF(SPR_V0F), u1f = PF(SPR_U1F), v1f = PF(SPR_V1F);
            const float ty = (float(y - PI(SPR_UY0)) + 0.5) / spriteH;
            const float tx = (float(x - PI(SPR_UX0)) + 0.5) / spriteW;
            const float texVf = v0f + (v1f - v0f) * ty;
            const float texUf = u0f + (u1f - u0f) * tx;
            uint texel;
            if (fst)
            {
                const int fixedU = int((texUf * 16.0) + 0.5);
                const int fixedV = int((texVf * 16.0) + 0.5);
                texel = sampleTexture(0.0, 0.0, 1.0, uint(clamp(fixedU, 0, 0xFFFF)), uint(clamp(fixedV, 0, 0xFFFF)), true, linear);
            }
            else
            {
                texel = sampleTexture(texUf / float(PI(TEXW)), texVf / float(PI(TEXH)), 1.0, 0u, 0u, false, linear);
            }
            const ivec4 tx4 = ivec4(int(texel & 0xFFu), int((texel >> 8) & 0xFFu), int((texel >> 16) & 0xFFu), int((texel >> 24) & 0xFFu));
            col = combineTexture(col, tx4);
        }
    }
    else
    {
        const float ofx = float(PI(OFX)), ofy = float(PI(OFY));
        const float fx0 = PF(X0 + 0u) - ofx, fx1 = PF(X0 + 1u) - ofx, fx2 = PF(X0 + 2u) - ofx;
        const float fy0 = PF(Y0 + 0u) - ofy, fy1 = PF(Y0 + 1u) - ofy, fy2 = PF(Y0 + 2u) - ofy;
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        const float winding = (denom < 0.0) ? -1.0 : 1.0;
        const float invAbsDenom = 1.0 / abs(denom);
        const float px = float(x) + 0.5, py = float(y) + 0.5;
        const float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
        const float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
        const float w2 = 1.0 - w0 - w1;
        const float eps = 1.0e-4;
        if (w0 < -eps || w1 < -eps || w2 < -eps)
            return false;

        const double z0 = packDouble2x32(uvec2(P(Z0 + 0u), P(Z0 + 1u)));
        const double z1 = packDouble2x32(uvec2(P(Z0 + 2u), P(Z0 + 3u)));
        const double z2 = packDouble2x32(uvec2(P(Z0 + 4u), P(Z0 + 5u)));
        const double zd = z0 + (z1 - z0) * double(w1) + (z2 - z0) * double(w2);
        const double zr = zd + 0.5;
        zv = (zr <= 0.0) ? 0u : uint(zr);

        uint rgbaV[3];
        rgbaV[0] = P(RGBA0 + 0u); rgbaV[1] = P(RGBA0 + 1u); rgbaV[2] = P(RGBA0 + 2u);
        if ((flags & F_IIP) != 0u)
        {
            for (uint c = 0u; c < 4u; ++c)
            {
                const uint sh = c * 8u;
                const float f = float((rgbaV[0] >> sh) & 0xFFu) * w0 + float((rgbaV[1] >> sh) & 0xFFu) * w1 + float((rgbaV[2] >> sh) & 0xFFu) * w2;
                col[c] = clampU8(int(f));
            }
        }
        else
        {
            const uint r2 = rgbaV[2];
            col = ivec4(int(r2 & 0xFFu), int((r2 >> 8) & 0xFFu), int((r2 >> 16) & 0xFFu), int((r2 >> 24) & 0xFFu));
        }

        if (tme)
        {
            float is = 0.0, it = 0.0, iq = 1.0;
            uint iu = 0u, iv = 0u;
            if (fst)
            {
                const uint uv0 = P(UV0 + 0u), uv1 = P(UV0 + 1u), uv2 = P(UV0 + 2u);
                const float fu = float(uv0 & 0xFFFFu) * w0 + float(uv1 & 0xFFFFu) * w1 + float(uv2 & 0xFFFFu) * w2;
                const float fv = float(uv0 >> 16) * w0 + float(uv1 >> 16) * w1 + float(uv2 >> 16) * w2;
                iu = uint(int(fu)) & 0xFFFFu;
                iv = uint(int(fv)) & 0xFFFFu;
            }
            else
            {
                is = PF(S0 + 0u) * w0 + PF(S0 + 1u) * w1 + PF(S0 + 2u) * w2;
                it = PF(T0 + 0u) * w0 + PF(T0 + 1u) * w1 + PF(T0 + 2u) * w2;
                iq = PF(Q0 + 0u) * w0 + PF(Q0 + 1u) * w1 + PF(Q0 + 2u) * w2;
            }
            const uint texel = sampleTexture(is, it, iq, iu, iv, fst, linear);
            const ivec4 tx4 = ivec4(int(texel & 0xFFu), int((texel >> 8) & 0xFFu), int((texel >> 16) & 0xFFu), int((texel >> 24) & 0xFFu));
            col = combineTexture(col, tx4);
        }
        const float ff = float(P(FOG0 + 0u)) * w0 + float(P(FOG0 + 1u)) * w1 + float(P(FOG0 + 2u)) * w2;
        fogv = clampU8(int(ff));
    }

    // ---- WritePixel ----
    const uint fpsm = P(FPSM);
    const uint fbp = P(FBP), fbw = max(P(FBW), 1u);
    const uint zbp = P(ZBP), zpsm = P(ZPSM);
    const uint test = P(TEST);
    int r = col.r, g = col.g, b = col.b, a = col.a;

    if ((flags & F_FGE) != 0u)
    {
        const uint fogRgb = P(FOGRGB);
        const uint inv = 255u - uint(fogv);
        const uint fr = uint(fogv);
        r = int(((fr * uint(r)) >> 8) + ((inv * (fogRgb & 0xFFu)) >> 8));
        g = int(((fr * uint(g)) >> 8) + ((inv * ((fogRgb >> 8) & 0xFFu)) >> 8));
        b = int(((fr * uint(b)) >> 8) + ((inv * ((fogRgb >> 16) & 0xFFu)) >> 8));
    }

    // alpha test classification
    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if ((test & 1u) != 0u)
    {
        const uint atst = (test >> 1) & 7u;
        const int aref = int((test >> 4) & 0xFFu);
        bool pass;
        switch (atst)
        {
        case 0u: pass = false; break;
        case 1u: pass = true; break;
        case 2u: pass = a < aref; break;
        case 3u: pass = a <= aref; break;
        case 4u: pass = a == aref; break;
        case 5u: pass = a >= aref; break;
        case 6u: pass = a > aref; break;
        default: pass = a != aref; break;
        }
        if (!pass)
        {
            switch ((test >> 12) & 3u)
            {
            case 1u: writeRgb = true; writeAlpha = true; writeDepth = false; break;
            case 2u: writeRgb = false; writeAlpha = false; writeDepth = true; break;
            case 3u:
                if (fpsm == 0u) { writeRgb = true; writeAlpha = false; writeDepth = false; }
                else { writeRgb = true; writeAlpha = true; writeDepth = false; }
                break;
            default: writeRgb = false; writeAlpha = false; writeDepth = false; break;
            }
        }
    }
    const bool writesFb = writeRgb || writeAlpha;
    if (!writesFb && !writeDepth)
        return false;

    const uint zmethod = (test >> 17) & 3u;
    const bool preserveDestAlpha = writeRgb && !writeAlpha && fpsm == 0u;
    const bool is16 = (fpsm == 2u || fpsm == 10u);

    pin = PixelIn(x, y, r, g, b, a, zv, flags, test, fpsm, fbp, fbw, zbp, zpsm, writeRgb, writeAlpha, writeDepth, writesFb, zmethod, preserveDestAlpha, is16);
    return true;
}

void main()
{
    PixelIn p;
    const bool ok = shade(p);
    beginInvocationInterlockARB();
    if (ok)
    {
        rmw(p.x, p.y, p.r, p.g, p.b, p.a, p.zv, p.flags, p.test, p.fpsm, p.fbp, p.fbw, p.zbp, p.zpsm, p.writeRgb, p.writeAlpha, p.writeDepth, p.writesFb, p.zmethod, p.preserveDestAlpha, p.is16);
    }
    endInvocationInterlockARB();
}
