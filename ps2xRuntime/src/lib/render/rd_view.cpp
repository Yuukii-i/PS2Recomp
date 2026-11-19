// FEAT-RD1: Direct geometry view from the VU1 scene buffer via OpenGL/rlgl.
#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <atomic> // FEAT-RD8
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD1
extern unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa); // FEAT-RD5
extern void ps2xRdTexTickFrame(); // FEAT-RD3

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2]; // FEAT-RD3
};
static_assert(sizeof(RdVert) == 28, "RdVert size"); // FEAT-RD3

struct RdBatch // FEAT-RD2
{
    uint32_t first;
    uint32_t count;
    uint64_t alpha;
    uint64_t test;
    uint64_t frame;
    uint64_t zbuf;
    uint64_t prim;
    uint64_t tex0;  // FEAT-RD3
    uint64_t clamp; // FEAT-RD3
    uint64_t tex1;  // FEAT-RD3
    uint64_t texa;  // FEAT-RD3
    uint32_t kickSerial; // FEAT-RD5
};

struct Rd2DBatch // FEAT-RD8
{
    uint32_t first;
    uint32_t count;
    uint32_t afterSerial;
    uint32_t texSerial;
    uint64_t prim;
    uint64_t tex0;
    uint64_t texa;
    uint64_t clamp;
    uint64_t tex1;
    uint64_t alpha;
    uint64_t test;
    uint64_t frame;
    uint64_t zbuf;
};

namespace
{
std::mutex g_rdMutex;
std::vector<RdVert> g_rdPending;
std::vector<RdBatch> g_rdPendingBatches; // FEAT-RD2
uint32_t g_rdPendingSkipped = 0;
uint32_t g_rdPendingMasked = 0; // FEAT-RD2
float g_rdPendingVpScaleZ = 0.0f;  // FEAT-RD11
float g_rdPendingVpOffsetZ = 0.0f; // FEAT-RD11
bool g_rdHasPending = false;
bool g_rdEverPublished = false;

std::vector<RdVert> g_rdPending2D; // FEAT-RD8
std::vector<Rd2DBatch> g_rdPending2DBatches; // FEAT-RD8
uint32_t g_rdPendingRtSkipped = 0; // FEAT-RD8
uint32_t g_rdPendingPointLineSkipped = 0; // FEAT-RD8
bool g_rdHasPending2D = false; // FEAT-RD8
std::atomic<uint32_t> g_rdPresentW{640u}; // FEAT-RD8
std::atomic<uint32_t> g_rdPresentH{448u}; // FEAT-RD8

struct RdGpuState
{
    bool init = false;
    bool everUploaded = false;
    unsigned int vao = 0;
    unsigned int vbo = 0;
    size_t vboCapVerts = 0;
    size_t gpuVertCount = 0;
    unsigned int vao2d = 0; // FEAT-RD8
    unsigned int vbo2d = 0; // FEAT-RD8
    size_t vbo2dCapVerts = 0; // FEAT-RD8
    size_t gpu2dVertCount = 0; // FEAT-RD8
    uint32_t lastSkippedDraws = 0;
    uint32_t lastMaskedDraws = 0; // FEAT-RD2
    uint32_t lastRtSkipped = 0; // FEAT-RD8
    uint32_t lastPointLineSkipped = 0; // FEAT-RD8
    Shader shader{};
    int flipLoc = -1;
    int mode2dLoc = -1; // FEAT-RD8
    int fbSizeLoc = -1; // FEAT-RD8
    int vpZLoc = -1;    // FEAT-RD11
    int atstLoc = -1; // FEAT-RD2
    int arefLoc = -1; // FEAT-RD2
    int samplerLoc = -1; // FEAT-RD3
    int texLoc = -1;     // FEAT-RD3
    int tfxLoc = -1;     // FEAT-RD3
    int tccLoc = -1;     // FEAT-RD3
    float flip[3] = {1.0f, -1.0f, 1.0f}; // FEAT-RD2
    std::vector<RdVert> localScratch;
    std::vector<RdBatch> localBatches; // FEAT-RD2
    std::vector<RdVert> local2dScratch; // FEAT-RD8
    std::vector<Rd2DBatch> local2dBatches; // FEAT-RD8
    uint32_t frames = 0;
    uint64_t totalTris = 0;
    uint64_t totalSkippedDraws = 0;
    uint64_t totalMaskedDraws = 0; // FEAT-RD2
    uint64_t totalUnsupportedBlend = 0; // FEAT-RD2
    uint64_t totalApproxAd = 0; // FEAT-RD7
    uint64_t total2dDraws = 0; // FEAT-RD8
    uint64_t total2dZtested = 0; // FEAT-RD11
    uint64_t total2dTris = 0; // FEAT-RD8
    uint64_t totalRtSkipped = 0; // FEAT-RD8
    uint64_t totalPointLineSkipped = 0; // FEAT-RD8
    float vpScaleZ = 0.0f;  // FEAT-RD11
    float vpOffsetZ = 0.0f; // FEAT-RD11
    int fixLoc = -1; // FEAT-RD7
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
};
RdGpuState g_rdGpu;

// FEAT-RD8: Shared RD7 blend state decoder for 3D and 2D batches.
int computeRdBlend(uint64_t prim, uint64_t alpha, uint64_t &unsupBlend, uint64_t &approxAd, float &fixAlpha)
{
    int blendState = 0;
    fixAlpha = -1.0f;
    if (!((prim >> 6) & 1ull))
        return 0;
    uint32_t a = static_cast<uint32_t>((alpha >> 0) & 3ull);
    uint32_t b = static_cast<uint32_t>((alpha >> 2) & 3ull);
    const uint32_t c = static_cast<uint32_t>((alpha >> 4) & 3ull);
    uint32_t d = static_cast<uint32_t>((alpha >> 6) & 3ull);
    if (a == 3u) a = 2u;
    if (b == 3u) b = 2u;
    if (d == 3u) d = 2u;
    int sfk = -1, dfk = -1, eqk = 0; // eqk 0 add, 1 subtract, 2 reverse subtract
    if (a == b)
    {
        sfk = (d == 0u) ? 1 : 0;
        dfk = (d == 1u) ? 1 : 0;
    }
    else if (a == 0u && b == 1u && d == 1u) { sfk = 2; dfk = 3; }
    else if (a == 0u && b == 2u && d == 1u) { sfk = 2; dfk = 1; }
    else if (a == 2u && b == 0u && d == 1u) { sfk = 2; dfk = 1; eqk = 2; }
    else if (a == 0u && b == 2u && d == 2u) { sfk = 2; dfk = 0; }
    else if (a == 0u && b == 1u && d == 2u) { sfk = 2; dfk = 2; eqk = 1; }
    else if (a == 1u && b == 0u && d == 0u) { sfk = 3; dfk = 2; }
    else if (a == 1u && b == 2u && d == 2u) { sfk = 0; dfk = 2; }
    else if (a == 1u && b == 2u && d == 0u) { sfk = 1; dfk = 2; }
    else if (a == 2u && b == 1u && d == 1u) { sfk = 0; dfk = 3; }
    if (sfk < 0 || c == 3u)
    {
        ++unsupBlend;
        static uint64_t s_unsupSeen[8];
        static int s_unsupN = 0;
        bool seen = false;
        for (int i = 0; i < s_unsupN; ++i)
            if (s_unsupSeen[i] == alpha)
                seen = true;
        if (!seen && s_unsupN < 8)
        {
            s_unsupSeen[s_unsupN++] = alpha;
            ps2xSafeLog("[rd] unsupported ALPHA=0x%016llx\n", static_cast<unsigned long long>(alpha));
        }
    }
    else
    {
        const bool usesC = (sfk >= 2 || dfk >= 2);
        (void)approxAd; // FEAT-RD9: Ad now uses GL DST_ALPHA
        if (usesC && c == 2u)
            fixAlpha = std::min(static_cast<float>((alpha >> 32) & 0xFFull) / 128.0f, 1.0f);
        // FEAT-RD9: kinds 0 ZERO,1 ONE,2 As,3 1-As,4 Ad,5 1-Ad; encoding 1 + sk*128 + dk*16 + eq
        int sk = sfk, dk = dfk;
        if (c == 1u)
        {
            if (sk == 2) sk = 4; else if (sk == 3) sk = 5;
            if (dk == 2) dk = 4; else if (dk == 3) dk = 5;
        }
        blendState = 1 + sk * 128 + dk * 16 + eqk;
    }
    return blendState;
}

// FEAT-RD9: FBMSK (frame>>32) -> GL colour mask; logs first 8 distinct values per class
int g_rdMaskCur = 15; // FEAT-RD9
void applyRdFbmsk(uint64_t frame, bool is2d, uint64_t &partialMask)
{
    static uint32_t s_seen[2][8];
    static int s_n[2] = {0, 0};
    const uint32_t fbmsk = static_cast<uint32_t>(frame >> 32);
    const int cls = is2d ? 1 : 0;
    bool seen = false;
    for (int i = 0; i < s_n[cls]; ++i)
        if (s_seen[cls][i] == fbmsk)
            seen = true;
    if (!seen && s_n[cls] < 8)
    {
        s_seen[cls][s_n[cls]++] = fbmsk;
        ps2xSafeLog("[rd] fbmsk%s=0x%08x (n3d=%d n2d=%d)\n", is2d ? "2d" : "3d", fbmsk, s_n[0], s_n[1]);
    }
    bool ch[4];
    bool partial = false;
    for (int c = 0; c < 4; ++c)
    {
        const uint32_t m = (fbmsk >> (8 * c)) & 0xFFu;
        ch[c] = (m != 0xFFu);
        if (m != 0u && m != 0xFFu)
            partial = true;
    }
    if (partial)
        ++partialMask;
    const int packed = (ch[0] ? 1 : 0) | (ch[1] ? 2 : 0) | (ch[2] ? 4 : 0) | (ch[3] ? 8 : 0);
    if (packed != g_rdMaskCur)
    {
        rlColorMask(ch[0], ch[1], ch[2], ch[3]);
        g_rdMaskCur = packed;
    }
}

void applyRdBlend(int blendState, unsigned int vao) // FEAT-RD8
{
    if (blendState == 0)
        rlDisableColorBlend();
    else
    {
        const int code = blendState - 1;
        const int sk = code / 128, dk = (code / 16) & 7, ek = code & 3; // FEAT-RD9
        auto glf = [](int k) { return k == 0 ? RL_ZERO : k == 1 ? RL_ONE : k == 2 ? RL_SRC_ALPHA : k == 3 ? RL_ONE_MINUS_SRC_ALPHA : k == 4 ? RL_DST_ALPHA : RL_ONE_MINUS_DST_ALPHA; };
        rlEnableColorBlend();
        rlSetBlendFactors(glf(sk), glf(dk), ek == 0 ? RL_FUNC_ADD : ek == 1 ? RL_FUNC_SUBTRACT : RL_FUNC_REVERSE_SUBTRACT);
        rlSetBlendMode(RL_BLEND_CUSTOM);
    }
    rlEnableShader(g_rdGpu.shader.id);
    rlEnableVertexArray(vao);
}

void uploadRdVbo(unsigned int vao, unsigned int &vbo, size_t &capVerts, const std::vector<RdVert> &verts) // FEAT-RD8
{
    const size_t n = verts.size();
    if (n == 0)
        return;
    if (n > capVerts)
    {
        if (vbo != 0)
            rlUnloadVertexBuffer(vbo);
        const size_t newCap = std::max<size_t>(n, capVerts ? capVerts * 2 : 4096);
        vbo = rlLoadVertexBuffer(nullptr, static_cast<int>(newCap * sizeof(RdVert)), true);
        capVerts = newCap;

        rlEnableVertexArray(vao);
        rlEnableVertexBuffer(vbo);
        rlSetVertexAttribute(0, 4, RL_FLOAT, false, sizeof(RdVert), 0);
        rlEnableVertexAttribute(0);
        rlSetVertexAttribute(1, 4, RL_UNSIGNED_BYTE, true, sizeof(RdVert), 16);
        rlEnableVertexAttribute(1);
        rlSetVertexAttribute(2, 2, RL_FLOAT, false, sizeof(RdVert), 20);
        rlEnableVertexAttribute(2);
        rlDisableVertexBuffer();
        rlDisableVertexArray();
    }
    rlUpdateVertexBuffer(vbo, verts.data(), static_cast<int>(n * sizeof(RdVert)), 0);
}
} // namespace

bool ps2xRdViewOn() // FEAT-RD1
{
    static const bool v = []() {
        const char *e = std::getenv("PS2X_RD_VIEW");
        return e && e[0] == '1';
    }();
    return v;
}

bool ps2xRdTakeoverOn() // FEAT-RD12
{
    static const bool v = []() {
        if (!ps2xRdViewOn())
            return false;
        const char *e = std::getenv("PS2X_RD_TAKEOVER");
        if (e && e[0] == '1')
        {
            ps2xSafeLog("[rd] TAKEOVER on: GS raster + readback skipped\n");
            return true;
        }
        return false;
    }();
    return v;
}

void ps2xRdPublish(const RdVert *v, size_t n, const RdBatch *b, size_t nb, uint32_t skippedDraws, uint32_t maskedDraws, float vpScaleZ, float vpOffsetZ) // FEAT-RD2 // FEAT-RD11
{
    static thread_local std::vector<RdVert> t_staging;
    static thread_local std::vector<RdBatch> t_batchStaging;
    if (v && n > 0)
        t_staging.assign(v, v + n);
    else
        t_staging.clear();
    if (b && nb > 0)
        t_batchStaging.assign(b, b + nb);
    else
        t_batchStaging.clear();

    std::lock_guard<std::mutex> lock(g_rdMutex);
    g_rdPending.swap(t_staging);
    g_rdPendingBatches.swap(t_batchStaging);
    g_rdPendingSkipped = skippedDraws;
    g_rdPendingMasked = maskedDraws;
    if (vpScaleZ != 0.0f) // FEAT-RD11
    {
        g_rdPendingVpScaleZ = vpScaleZ;
        g_rdPendingVpOffsetZ = vpOffsetZ;
    }
    g_rdHasPending = true;
    g_rdEverPublished = true;
}

void ps2xRdPublish2D(const RdVert *v, size_t n, const Rd2DBatch *b, size_t nb, uint32_t rtSkipped, uint32_t pointLineSkipped) // FEAT-RD8
{
    static thread_local std::vector<RdVert> t_staging2d;
    static thread_local std::vector<Rd2DBatch> t_batchStaging2d;
    if (v && n > 0)
        t_staging2d.assign(v, v + n);
    else
        t_staging2d.clear();
    if (b && nb > 0)
        t_batchStaging2d.assign(b, b + nb);
    else
        t_batchStaging2d.clear();

    std::lock_guard<std::mutex> lock(g_rdMutex);
    g_rdPending2D.swap(t_staging2d);
    g_rdPending2DBatches.swap(t_batchStaging2d);
    g_rdPendingRtSkipped = rtSkipped;
    g_rdPendingPointLineSkipped = pointLineSkipped;
    g_rdHasPending2D = true;
    g_rdEverPublished = true;
}

void ps2xRdSetPresentSize(uint32_t w, uint32_t h) // FEAT-RD8
{
    if (w > 0u)
        g_rdPresentW.store(w, std::memory_order_relaxed);
    if (h > 0u)
        g_rdPresentH.store(h, std::memory_order_relaxed);
}

void ps2xRdGetPresentSize(uint32_t &w, uint32_t &h) // FEAT-RD12
{
    w = g_rdPresentW.load(std::memory_order_relaxed);
    h = g_rdPresentH.load(std::memory_order_relaxed);
}

bool ps2xRdDraw(float x, float y, float w, float h) // FEAT-RD1
{
    bool hasNew = false;
    bool hasNew2d = false; // FEAT-RD8
    {
        std::lock_guard<std::mutex> lock(g_rdMutex);
        if (!g_rdEverPublished && !g_rdGpu.everUploaded)
        {
            if (ps2xRdTakeoverOn()) // FEAT-RD12: clear to black instead of drawing stale GS texture
                return true;
            return false;
        }
        if (g_rdHasPending)
        {
            g_rdGpu.localScratch.swap(g_rdPending);
            g_rdGpu.localBatches.swap(g_rdPendingBatches); // FEAT-RD2
            g_rdGpu.lastSkippedDraws = g_rdPendingSkipped;
            g_rdGpu.lastMaskedDraws = g_rdPendingMasked; // FEAT-RD2
            g_rdGpu.vpScaleZ = g_rdPendingVpScaleZ;      // FEAT-RD11
            g_rdGpu.vpOffsetZ = g_rdPendingVpOffsetZ;    // FEAT-RD11
            g_rdHasPending = false;
            hasNew = true;
        }
        if (g_rdHasPending2D) // FEAT-RD8
        {
            g_rdGpu.local2dScratch.swap(g_rdPending2D);
            g_rdGpu.local2dBatches.swap(g_rdPending2DBatches);
            g_rdGpu.lastRtSkipped = g_rdPendingRtSkipped;
            g_rdGpu.lastPointLineSkipped = g_rdPendingPointLineSkipped;
            g_rdHasPending2D = false;
            hasNew2d = true;
        }
    }

    if (!g_rdGpu.init)
    {
        g_rdGpu.init = true;
        static const char *kVs =
            "#version 330\n"
            "layout(location = 0) in vec4 aPos;\n"
            "layout(location = 1) in vec4 aCol;\n"
            "layout(location = 2) in vec2 aUv;\n" // FEAT-RD3
            "uniform vec3 uFlip;\n"
            "uniform int uMode2d;\n" // FEAT-RD8
            "uniform vec2 uFbSize;\n" // FEAT-RD8
            "uniform vec2 uVpZ;\n"    // FEAT-RD11
            "out vec4 vCol;\n"
            "out vec2 vUv;\n" // FEAT-RD3
            "void main() {\n"
            "    if (uMode2d != 0) {\n" // FEAT-RD8
            "        float ndcZ = (uMode2d == 2 && abs(uVpZ.x) > 1e-6) ? ((aPos.z - uVpZ.y) / uVpZ.x) * uFlip.z : 0.0;\n" // FEAT-RD11
            "        gl_Position = vec4(aPos.x / uFbSize.x * 2.0 - 1.0, 1.0 - aPos.y / uFbSize.y * 2.0, ndcZ, 1.0);\n"
            "    } else {\n"
            "        gl_Position = vec4(aPos.x * uFlip.x, aPos.y * uFlip.y, aPos.z * uFlip.z, aPos.w);\n"
            "    }\n"
            "    vCol = aCol;\n"
            "    vUv = aUv;\n" // FEAT-RD3
            "}\n";
        static const char *kFs =
            "#version 330\n"
            "in vec4 vCol;\n"
            "in vec2 vUv;\n" // FEAT-RD3
            "uniform sampler2D uSampler;\n" // FEAT-RD3
            "uniform int uTex;\n" // FEAT-RD3
            "uniform int uTfx;\n" // FEAT-RD3
            "uniform int uTcc;\n" // FEAT-RD3
            "uniform int uAtst;\n"
            "uniform int uAref;\n"
            "uniform float uFixAlpha;\n" // FEAT-RD7
            "out vec4 finalColor;\n"
            "void main() {\n"
            "    vec3 rgb = vCol.rgb;\n"
            "    float a_ps2 = vCol.a * 255.0;\n"
            "    if (uTex != 0) {\n" // FEAT-RD3
            "        vec4 t = texture(uSampler, vUv);\n"
            "        float t_a_ps2 = t.a * 255.0;\n"
            "        vec3 vc_rgb = vCol.rgb * 255.0;\n"
            "        float vc_a = vCol.a * 255.0;\n"
            "        if (uTfx == 0) {\n"
            "            rgb = t.rgb * vc_rgb / 128.0;\n"
            "            a_ps2 = (uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;\n"
            "        } else if (uTfx == 1) {\n"
            "            rgb = t.rgb;\n"
            "            a_ps2 = (uTcc != 0) ? t_a_ps2 : vc_a;\n"
            "        } else if (uTfx == 2) {\n"
            "            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);\n"
            "            a_ps2 = (uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;\n"
            "        } else {\n"
            "            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);\n"
            "            a_ps2 = (uTcc != 0) ? t_a_ps2 : vc_a;\n"
            "        }\n"
            "        rgb = clamp(rgb, 0.0, 1.0);\n"
            "    }\n"
            "    if (uAtst != 1) {\n"
            "        int a = int(clamp(a_ps2, 0.0, 255.0) + 0.5);\n"
            "        bool pass = true;\n"
            "        if (uAtst == 0) pass = false;\n"
            "        else if (uAtst == 2) pass = (a < uAref);\n"
            "        else if (uAtst == 3) pass = (a <= uAref);\n"
            "        else if (uAtst == 4) pass = (a == uAref);\n"
            "        else if (uAtst == 5) pass = (a >= uAref);\n"
            "        else if (uAtst == 6) pass = (a > uAref);\n"
            "        else if (uAtst == 7) pass = (a != uAref);\n"
            "        if (!pass) discard;\n"
            "    }\n"
            "    finalColor = vec4(rgb, (uFixAlpha >= 0.0) ? min(uFixAlpha, 1.0) : min(a_ps2 / 128.0, 1.0));\n" // FEAT-RD7
            "}\n";
        g_rdGpu.shader = LoadShaderFromMemory(kVs, kFs);
        g_rdGpu.flipLoc = GetShaderLocation(g_rdGpu.shader, "uFlip");
        g_rdGpu.mode2dLoc = GetShaderLocation(g_rdGpu.shader, "uMode2d"); // FEAT-RD8
        g_rdGpu.fbSizeLoc = GetShaderLocation(g_rdGpu.shader, "uFbSize"); // FEAT-RD8
        g_rdGpu.vpZLoc = GetShaderLocation(g_rdGpu.shader, "uVpZ");       // FEAT-RD11
        g_rdGpu.atstLoc = GetShaderLocation(g_rdGpu.shader, "uAtst"); // FEAT-RD2
        g_rdGpu.arefLoc = GetShaderLocation(g_rdGpu.shader, "uAref"); // FEAT-RD2
        g_rdGpu.fixLoc = GetShaderLocation(g_rdGpu.shader, "uFixAlpha"); // FEAT-RD7
        g_rdGpu.samplerLoc = GetShaderLocation(g_rdGpu.shader, "uSampler"); // FEAT-RD3
        g_rdGpu.texLoc = GetShaderLocation(g_rdGpu.shader, "uTex");         // FEAT-RD3
        g_rdGpu.tfxLoc = GetShaderLocation(g_rdGpu.shader, "uTfx");         // FEAT-RD3
        g_rdGpu.tccLoc = GetShaderLocation(g_rdGpu.shader, "uTcc");         // FEAT-RD3
        if (g_rdGpu.samplerLoc >= 0) // FEAT-RD3
        {
            int samplerUnit = 0;
            SetShaderValue(g_rdGpu.shader, g_rdGpu.samplerLoc, &samplerUnit, SHADER_UNIFORM_INT);
        }
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        g_rdGpu.flip[0] = 1.0f;
        g_rdGpu.flip[1] = (flipBits & 1) ? 1.0f : -1.0f; // FEAT-RD2: y flip default; bit 1 = do NOT flip y
        g_rdGpu.flip[2] = (flipBits & 2) ? -1.0f : 1.0f;
        if (g_rdGpu.flipLoc >= 0)
            SetShaderValue(g_rdGpu.shader, g_rdGpu.flipLoc, g_rdGpu.flip, SHADER_UNIFORM_VEC3);
        g_rdGpu.vao = rlLoadVertexArray();
        g_rdGpu.vao2d = rlLoadVertexArray(); // FEAT-RD8
    }

    if (hasNew)
    {
        g_rdGpu.everUploaded = true;
        g_rdGpu.gpuVertCount = g_rdGpu.localScratch.size();
        uploadRdVbo(g_rdGpu.vao, g_rdGpu.vbo, g_rdGpu.vboCapVerts, g_rdGpu.localScratch);
    }
    if (hasNew2d) // FEAT-RD8
    {
        g_rdGpu.everUploaded = true;
        g_rdGpu.gpu2dVertCount = g_rdGpu.local2dScratch.size();
        uploadRdVbo(g_rdGpu.vao2d, g_rdGpu.vbo2d, g_rdGpu.vbo2dCapVerts, g_rdGpu.local2dScratch);
    }

    rlDrawRenderBatchActive();
    ps2xRdTexTickFrame(); // FEAT-RD3

    const int rw = GetRenderWidth();
    const int rh = GetRenderHeight();
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const float scaleX = (sw > 0) ? (static_cast<float>(rw) / static_cast<float>(sw)) : 1.0f;
    const float scaleY = (sh > 0) ? (static_cast<float>(rh) / static_cast<float>(sh)) : 1.0f;
    const int vx = static_cast<int>(x * scaleX);
    const int vy = static_cast<int>((static_cast<float>(sh) - y - h) * scaleY);
    const int vw = static_cast<int>(w * scaleX);
    const int vh = static_cast<int>(h * scaleY);

    rlViewport(vx, vy, vw, vh);
    { // FEAT-RD9: clear box alpha to 0 (colour untouched via mask)
        rlEnableScissorTest();
        rlScissor(vx, vy, vw, vh);
        rlColorMask(false, false, false, true);
        rlClearColor(0, 0, 0, 0);
        rlClearScreenBuffers();
        rlColorMask(true, true, true, true);
        rlDisableScissorTest();
    }
    rlEnableDepthTest();
    rlDisableBackfaceCulling();

    uint64_t unsupBlend = 0; // FEAT-RD2
    uint64_t approxAd = 0;   // FEAT-RD7
    uint64_t partialMask = 0; // FEAT-RD9
    static const bool s_draw2d = []{ const char *e = std::getenv("PS2X_RD_2D"); return !(e && e[0] == '0'); }(); // FEAT-RD9
    static const bool s_2dDepth = []{ const char *e = std::getenv("PS2X_RD_2DZ"); return !(e && e[0] == '0'); }(); // FEAT-RD11
    uint64_t draws2dThisFrame = 0; // FEAT-RD8
    uint64_t ztested2dThisFrame = 0; // FEAT-RD11
    if ((g_rdGpu.gpuVertCount > 0 || g_rdGpu.gpu2dVertCount > 0) && g_rdGpu.vao != 0 && g_rdGpu.shader.id != 0) // FEAT-RD8
    {
        rlEnableShader(g_rdGpu.shader.id);
        int mode2d = 0; // FEAT-RD8
        if (g_rdGpu.mode2dLoc >= 0)
            rlSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
        const float fbSize[2] = {
            static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed)),
            static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed))
        };
        if (g_rdGpu.fbSizeLoc >= 0)
            rlSetUniform(g_rdGpu.fbSizeLoc, fbSize, RL_SHADER_UNIFORM_VEC2, 1);
        const float vpZ[2] = { g_rdGpu.vpScaleZ, g_rdGpu.vpOffsetZ }; // FEAT-RD11
        if (g_rdGpu.vpZLoc >= 0)
            rlSetUniform(g_rdGpu.vpZLoc, vpZ, RL_SHADER_UNIFORM_VEC2, 1);
        rlEnableVertexArray(g_rdGpu.vao);
        int prevBlend = -1; // FEAT-RD2b
        size_t i2d = 0; // FEAT-RD8
        const size_t n2d = (s_draw2d && g_rdGpu.gpu2dVertCount > 0 && g_rdGpu.vao2d != 0) ? g_rdGpu.local2dBatches.size() : 0;

        auto draw2dUntil = [&](uint32_t maxSerial, bool drawAll) // FEAT-RD8
        {
            const uint32_t maxS = maxSerial & 0x3FFFFFFFu; // FEAT-RD10
            if (i2d >= n2d || (!drawAll && (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) >= maxS))
                return;
            mode2d = 1;
            if (g_rdGpu.mode2dLoc >= 0)
                rlSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            rlDisableDepthTest();
            rlDisableDepthMask();
            rlEnableVertexArray(g_rdGpu.vao2d);

            while (i2d < n2d && (drawAll || (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) < maxS))
            {
                const Rd2DBatch &b0 = g_rdGpu.local2dBatches[i2d];
                if (b0.count == 0u)
                {
                    ++i2d;
                    continue;
                }
                const uint32_t ate = static_cast<uint32_t>(b0.test & 1ull);
                const uint32_t atst = ate ? static_cast<uint32_t>((b0.test >> 1) & 7ull) : 1u;
                if (atst == 0u)
                {
                    ++i2d;
                    continue;
                }
                const uint32_t zte = static_cast<uint32_t>((b0.test >> 16) & 1ull); // FEAT-RD11
                const uint32_t ztst = static_cast<uint32_t>((b0.test >> 17) & 3ull);
                if (zte && ztst == 0u)
                {
                    ++i2d;
                    continue;
                }
                const bool use2dZ = s_2dDepth && zte && (ztst == 2u || ztst == 3u) && (g_rdGpu.vpScaleZ != 0.0f);
                const int wantMode2d = use2dZ ? 2 : 1;
                if (wantMode2d != mode2d)
                {
                    mode2d = wantMode2d;
                    if (g_rdGpu.mode2dLoc >= 0)
                        rlSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
                }
                if (use2dZ)
                {
                    rlEnableDepthTest();
                    if ((b0.zbuf >> 32) & 1ull)
                        rlDisableDepthMask();
                    else
                        rlEnableDepthMask();
                }
                else
                {
                    rlDisableDepthTest();
                    rlDisableDepthMask();
                }
                applyRdFbmsk(b0.frame, true, partialMask); // FEAT-RD9
                float fixAlpha = -1.0f;
                const int blendState = computeRdBlend(b0.prim, b0.alpha, unsupBlend, approxAd, fixAlpha);
                if (blendState != prevBlend)
                {
                    applyRdBlend(blendState, g_rdGpu.vao2d);
                    prevBlend = blendState;
                }
                int atstVal = static_cast<int>(atst);
                int arefVal = static_cast<int>((b0.test >> 4) & 0xFFull);
                if (g_rdGpu.atstLoc >= 0)
                    rlSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.arefLoc >= 0)
                    rlSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.fixLoc >= 0)
                    rlSetUniform(g_rdGpu.fixLoc, &fixAlpha, RL_SHADER_UNIFORM_FLOAT, 1);

                int texVal = 0, tfxVal = 0, tccVal = 0;
                unsigned int texId = 0u;
                if ((b0.prim >> 4) & 1ull)
                {
                    texId = ps2xRdGetTextureForBatch(b0.texSerial, b0.tex0, b0.texa);
                    if (texId != 0u)
                    {
                        const uint32_t wms = static_cast<uint32_t>(b0.clamp & 3ull);
                        const uint32_t wmt = static_cast<uint32_t>((b0.clamp >> 2) & 3ull);
                        rlTextureParameters(texId, RL_TEXTURE_WRAP_S, (wms == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP);
                        rlTextureParameters(texId, RL_TEXTURE_WRAP_T, (wmt == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP);
                        rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, (((b0.tex1 >> 5) & 1ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR);
                        rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, (((b0.tex1 >> 6) & 7ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR);
                        rlActiveTextureSlot(0);
                        rlEnableTexture(texId);
                        texVal = 1;
                        tfxVal = static_cast<int>((b0.tex0 >> 35) & 3ull);
                        tccVal = static_cast<int>((b0.tex0 >> 34) & 1ull);
                    }
                }
                if (g_rdGpu.texLoc >= 0)
                    rlSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.tfxLoc >= 0)
                    rlSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.tccLoc >= 0)
                    rlSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);

                uint32_t totalCount = b0.count;
                size_t j = i2d + 1;
                while (j < n2d && (drawAll || (g_rdGpu.local2dBatches[j].afterSerial & 0x3FFFFFFFu) < maxS)) // FEAT-RD10
                {
                    const Rd2DBatch &bn = g_rdGpu.local2dBatches[j];
                    if (bn.first != b0.first + totalCount ||
                        bn.prim != b0.prim || bn.tex0 != b0.tex0 || bn.texa != b0.texa ||
                        bn.clamp != b0.clamp || bn.tex1 != b0.tex1 || bn.alpha != b0.alpha ||
                        bn.test != b0.test || bn.frame != b0.frame || bn.zbuf != b0.zbuf)
                        break;
                    if (texVal != 0 && ps2xRdGetTextureForBatch(bn.texSerial, bn.tex0, bn.texa) != texId)
                        break;
                    totalCount += bn.count;
                    ++j;
                }
                rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
                ++draws2dThisFrame;
                if (use2dZ) // FEAT-RD11
                    ++ztested2dThisFrame;
                i2d = j;
            }

            mode2d = 0;
            if (g_rdGpu.mode2dLoc >= 0)
                rlSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            rlEnableVertexArray(g_rdGpu.vao);
        };

        for (const RdBatch &batch : g_rdGpu.localBatches) // FEAT-RD2
        {
            draw2dUntil(batch.kickSerial, false); // FEAT-RD8
            if (batch.count == 0u)
                continue;
            const uint64_t test = batch.test;
            const uint32_t ate = static_cast<uint32_t>(test & 1ull);
            const uint32_t atst = ate ? static_cast<uint32_t>((test >> 1) & 7ull) : 1u;
            if (atst == 0u)
                continue;
            const uint32_t zte = static_cast<uint32_t>((test >> 16) & 1ull);
            const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
            if (zte && ztst == 0u)
                continue;
            if (!zte || ztst == 1u)
                rlDisableDepthTest();
            else
                rlEnableDepthTest();
            if ((batch.zbuf >> 32) & 1ull)
                rlDisableDepthMask();
            else
                rlEnableDepthMask();

            // FEAT-RD7: blendState 0 = off, else 1 + sfk*64 + dfk*16 + eqk (factor kinds: 0 ZERO, 1 ONE, 2 C, 3 1-C)
            applyRdFbmsk(batch.frame, false, partialMask); // FEAT-RD9
            float fixAlpha = -1.0f;
            const int blendState = computeRdBlend(batch.prim, batch.alpha, unsupBlend, approxAd, fixAlpha); // FEAT-RD8
            if (blendState != prevBlend) // FEAT-RD2b: blend calls flush raylib batch and unbind shader/VAO
            {
                applyRdBlend(blendState, g_rdGpu.vao); // FEAT-RD8
                prevBlend = blendState;
            }
            int atstVal = static_cast<int>(atst);
            int arefVal = static_cast<int>((test >> 4) & 0xFFull);
            if (g_rdGpu.atstLoc >= 0)
                rlSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.arefLoc >= 0)
                rlSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.fixLoc >= 0) // FEAT-RD7
                rlSetUniform(g_rdGpu.fixLoc, &fixAlpha, RL_SHADER_UNIFORM_FLOAT, 1);

            int texVal = 0; // FEAT-RD3
            int tfxVal = 0;
            int tccVal = 0;
            if ((batch.prim >> 4) & 1ull)
            {
                const unsigned int texId = ps2xRdGetTextureForBatch(batch.kickSerial, batch.tex0, batch.texa); // FEAT-RD5
                if (texId != 0u)
                {
                    const uint32_t wms = static_cast<uint32_t>(batch.clamp & 3ull);
                    const uint32_t wmt = static_cast<uint32_t>((batch.clamp >> 2) & 3ull);
                    const int wrapS = (wms == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP;
                    const int wrapT = (wmt == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP;
                    const int magFilter = (((batch.tex1 >> 5) & 1ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR;
                    const int minFilter = (((batch.tex1 >> 6) & 7ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR;
                    rlTextureParameters(texId, RL_TEXTURE_WRAP_S, wrapS);
                    rlTextureParameters(texId, RL_TEXTURE_WRAP_T, wrapT);
                    rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, magFilter);
                    rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, minFilter);
                    rlActiveTextureSlot(0);
                    rlEnableTexture(texId);
                    texVal = 1;
                    tfxVal = static_cast<int>((batch.tex0 >> 35) & 3ull);
                    tccVal = static_cast<int>((batch.tex0 >> 34) & 1ull);
                }
            }
            if (g_rdGpu.texLoc >= 0)
                rlSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tfxLoc >= 0)
                rlSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tccLoc >= 0)
                rlSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);

            rlDrawVertexArray(static_cast<int>(batch.first), static_cast<int>(batch.count));
        }
        draw2dUntil(0u, true); // FEAT-RD8
        rlActiveTextureSlot(0); // FEAT-RD3
        rlDisableTexture();     // FEAT-RD3
        rlDisableVertexArray();
        rlDisableShader();
    }

    rlColorMask(true, true, true, true); // FEAT-RD9
    g_rdMaskCur = 15;
    (void)partialMask;
    rlEnableDepthMask();
    rlEnableColorBlend(); // FEAT-RD2b
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    rlViewport(0, 0, rw, rh);

    ++g_rdGpu.frames;
    g_rdGpu.totalTris += static_cast<uint64_t>(g_rdGpu.gpuVertCount / 3u);
    g_rdGpu.totalSkippedDraws += static_cast<uint64_t>(g_rdGpu.lastSkippedDraws);
    g_rdGpu.totalMaskedDraws += static_cast<uint64_t>(g_rdGpu.lastMaskedDraws); // FEAT-RD2
    g_rdGpu.totalUnsupportedBlend += unsupBlend; // FEAT-RD2
    g_rdGpu.totalApproxAd += approxAd; // FEAT-RD7
    g_rdGpu.total2dDraws += draws2dThisFrame; // FEAT-RD8
    g_rdGpu.total2dZtested += ztested2dThisFrame; // FEAT-RD11
    g_rdGpu.total2dTris += static_cast<uint64_t>(g_rdGpu.gpu2dVertCount / 3u); // FEAT-RD8
    g_rdGpu.totalRtSkipped += static_cast<uint64_t>(g_rdGpu.lastRtSkipped); // FEAT-RD8
    g_rdGpu.totalPointLineSkipped += static_cast<uint64_t>(g_rdGpu.lastPointLineSkipped); // FEAT-RD8
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rdGpu.lastLog).count() >= 5000)
    {
        g_rdGpu.lastLog = now;
        const double fr = g_rdGpu.frames ? static_cast<double>(g_rdGpu.frames) : 1.0;
        ps2xSafeLog("[rd] frames=%u tris/frame=%.0f skippedDraws/frame=%.1f maskedDraws/frame=%.1f unsupBlend/frame=%.1f approxAd/frame=%.1f\n",
                    g_rdGpu.frames,
                    static_cast<double>(g_rdGpu.totalTris) / fr,
                    static_cast<double>(g_rdGpu.totalSkippedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalMaskedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalUnsupportedBlend) / fr,
                    static_cast<double>(g_rdGpu.totalApproxAd) / fr); // FEAT-RD7
        ps2xSafeLog("[rd2d] draws/frame=%.1f tris/frame=%.1f rtSkipped=%.1f pointLineSkipped=%.1f\n",
                    static_cast<double>(g_rdGpu.total2dDraws) / fr,
                    static_cast<double>(g_rdGpu.total2dTris) / fr,
                    static_cast<double>(g_rdGpu.totalRtSkipped) / fr,
                    static_cast<double>(g_rdGpu.totalPointLineSkipped) / fr); // FEAT-RD8
        ps2xSafeLog("[rd2d] ztested/frame=%.1f vpZ=%f,%f\n",
                    static_cast<double>(g_rdGpu.total2dZtested) / fr,
                    g_rdGpu.vpScaleZ,
                    g_rdGpu.vpOffsetZ); // FEAT-RD11
    }
    return true;
}
