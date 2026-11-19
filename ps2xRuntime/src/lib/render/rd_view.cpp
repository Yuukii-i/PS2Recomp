// FEAT-RD1: Direct geometry view from the VU1 scene buffer via OpenGL/rlgl.
#include <unordered_map>
#include <cstring>
#include "raylib.h"
#include "rlgl.h"
#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall glDepthFunc(unsigned int func); // FEAT-RD20 (opengl32 export; rlgl.h does not expose glad here)
#endif
static bool g_rdAlwaysFunc = false; // FEAT-RD20

#include <algorithm>
#include <atomic> // FEAT-RD8
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio> // PROBE-RD24
#include <string> // PROBE-RD24
#include <map> // PROBE-RD26
#include <algorithm> // PROBE-RD26
#include <mutex>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD1
extern unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa); // FEAT-RD5
extern bool ps2xRdIsDisplayFbp(uint32_t fbp); // FEAT-RD19
extern bool ps2xRdHasDisplayFbp(); // FEAT-RD19
extern bool ps2xRdLookupState(uint32_t serial, uint64_t &prim, uint64_t &alpha, uint64_t &test, uint64_t &frame,
                              uint64_t &zbuf, uint64_t &tex0, uint64_t &tex1, uint64_t &clamp, uint64_t &texa, uint64_t &fba); // FEAT-RD17
extern bool ps2xRdLookupXyoff(uint32_t serial, uint64_t &xyoff); // FEAT-RD21
extern bool ps2xRdLookupScissor(uint32_t serial, uint64_t &scis); // FEAT-RD22
extern bool ps2xRdIsMainPass(uint32_t fbp, uint64_t xyoffset); // FEAT-RD23
extern uint64_t ps2xRdMainXyoff(); // FEAT-RD23
extern bool ps2xRdIsFbFeedback(uint32_t serial); // FEAT-RD28b
static uint32_t s_fbFeedbackSkipped = 0u; // FEAT-RD28b
extern std::atomic<uint32_t> g_rdStateChangesInKick; // FEAT-RD17
extern std::atomic<uint32_t> g_rdMergedSpan;        // FEAT-RD17
extern void ps2xRdTexTickFrame(); // FEAT-RD3

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2]; // FEAT-RD3
};
static_assert(sizeof(RdVert) == 28, "RdVert size"); // FEAT-RD3

struct RdBatch // FEAT-RD2 // FEAT-RD14
{
    uint32_t first;
    uint32_t count;
    uint32_t baseVertex; // FEAT-RD14
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
    float vpS[2]; // FEAT-RD21
    float vpO[2]; // FEAT-RD21
    uint32_t camHash; // PROBE-RD26
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
    uint64_t scissor; // FEAT-RD22
};

struct Rd3DFrame // FEAT-RD13 // FEAT-RD14
{
    std::vector<RdVert> verts;
    std::vector<uint32_t> indices; // FEAT-RD14
    std::vector<RdBatch> batches;
    uint64_t seq = 0; // FEAT-RD14
    uint32_t skipped = 0;
    uint32_t masked = 0;
    float vpScaleZ = 0.0f;
    float vpOffsetZ = 0.0f;
    uint32_t sMin = 0;
    uint32_t sMax = 0;
};

struct Rd2DFrame // FEAT-RD13
{
    std::vector<RdVert> verts;
    std::vector<Rd2DBatch> batches;
    uint32_t rtSkipped = 0;
    uint32_t pointLineSkipped = 0;
    uint32_t gMin = 0;
    uint32_t gMax = 0;
    bool valid = false;
};

namespace
{
std::mutex g_rdMutex;
Rd3DFrame g_rd3dRing[4]; // FEAT-RD13
size_t g_rd3dWriteIdx = 0; // FEAT-RD13
size_t g_rd3dCount = 0; // FEAT-RD13
uint64_t g_rd3dSeq = 0; // FEAT-RD13
float g_rdLastVpScaleZ = 0.0f;  // FEAT-RD11
float g_rdLastVpOffsetZ = 0.0f; // FEAT-RD11
bool g_rdEverPublished = false;

Rd2DFrame g_rd2dFrames[2]; // FEAT-RD13: [0]=newest, [1]=previous
uint64_t g_rd2dSeq = 0; // FEAT-RD13
std::atomic<uint32_t> g_rdPresentW{640u}; // FEAT-RD8
std::atomic<uint32_t> g_rdPresentH{448u}; // FEAT-RD8

struct RdGpuSlot // FEAT-RD14
{
    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int ebo = 0;
    size_t vboCapVerts = 0;
    size_t eboCapIndices = 0;
    size_t vertCount = 0;
    size_t indexCount = 0;
    uint64_t seq = 0;
    uint32_t skipped = 0;
    uint32_t masked = 0;
    float vpScaleZ = 0.0f;
    float vpOffsetZ = 0.0f;
    uint32_t sMin = 0;
    uint32_t sMax = 0;
    uint32_t curBaseVertex = 0;
    std::vector<RdBatch> batches;
};

struct RdDrawBatch // FEAT-RD14
{
    RdBatch batch;
    uint32_t slotIdx = 0;
};

struct RdGpuState
{
    bool init = false;
    bool everUploaded = false;
    RdGpuSlot slots[4]; // FEAT-RD14
    size_t activeOrder[4] = {0, 1, 2, 3}; // FEAT-RD14
    std::vector<RdVert> pendingVerts[4]; // FEAT-RD14
    std::vector<uint32_t> pendingIndices[4]; // FEAT-RD14
    std::vector<uint16_t> idx16Scratch; // FEAT-RD14
    std::vector<RdBatch> splitBatchScratch; // FEAT-RD14
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
    int vpALoc = -1;    // FEAT-RD21
    int vpBLoc = -1;    // FEAT-RD21
    int atstLoc = -1; // FEAT-RD2
    int arefLoc = -1; // FEAT-RD2
    int samplerLoc = -1; // FEAT-RD3
    int texLoc = -1;     // FEAT-RD3
    int tfxLoc = -1;     // FEAT-RD3
    int tccLoc = -1;     // FEAT-RD3
    float flip[3] = {1.0f, -1.0f, 1.0f}; // FEAT-RD2
    std::vector<RdDrawBatch> drawBatches; // FEAT-RD14
    std::vector<RdVert> local2dScratch; // FEAT-RD8
    std::vector<Rd2DBatch> local2dBatches; // FEAT-RD8
    size_t ring3dCount = 0; // FEAT-RD13
    uint64_t last3dSeq = 0; // FEAT-RD13
    Rd2DFrame frames2d[2]; // FEAT-RD13
    uint64_t last2dSeq = 0; // FEAT-RD13
    uint32_t activeGMin = 0; // FEAT-RD13
    bool curAligned = false; // FEAT-RD13
    bool curFallbackPrev = false; // FEAT-RD13
    bool curSpanned2 = false; // FEAT-RD13
    uint32_t alignedFrames = 0; // FEAT-RD13
    uint32_t fallbackPrevFrames = 0; // FEAT-RD13
    uint32_t spanned2Frames = 0; // FEAT-RD13
    uint32_t gsStateCount = 0;        // FEAT-RD17
    uint32_t rtSkipped3d = 0;         // FEAT-RD19
    uint32_t parsedFallbackCount = 0; // FEAT-RD17
    uint32_t frames = 0;
    uint64_t frameVerts = 0;   // FEAT-RD14
    uint64_t frameIndices = 0; // FEAT-RD14
    uint64_t frameTris = 0;    // FEAT-RD14
    uint64_t totalVerts = 0;   // FEAT-RD14
    uint64_t totalIndices = 0; // FEAT-RD14
    uint64_t totalTris = 0;
    uint64_t totalSkippedDraws = 0;
    uint64_t totalMaskedDraws = 0; // FEAT-RD2
    uint64_t totalUnsupportedBlend = 0; // FEAT-RD2
    uint64_t totalApproxAd = 0; // FEAT-RD7
    uint64_t total2dDraws = 0; // FEAT-RD8
    uint64_t total2dZtested = 0; // FEAT-RD11
    uint64_t total2dZclears = 0; // FEAT-RD20
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

// FEAT-RD25: DATE approximation. States: sk=6 (DATM0: SRC 1-Ad, DST Ad), sk=7 (DATM1: SRC Ad, DST 1-Ad)
uint64_t g_rdDateCount = 0, g_rdDateAbeCount = 0; // FEAT-RD25
int applyRdDate(uint64_t test, uint64_t prim, int blendState) // FEAT-RD25
{
    if (!((test >> 14) & 1ull))
        return blendState;
    ++g_rdDateCount;
    if ((prim >> 6) & 1ull)
        ++g_rdDateAbeCount;
    return 1 + (((test >> 15) & 1ull) ? 7 : 6) * 128;
}

// FEAT-RD15: GL state cache (redundant call elimination)
struct RdGlCache
{
    std::unordered_map<unsigned int, uint32_t> texParams;
    unsigned int bound = 0u;
    int depthTest = -1, depthMask = -1;
    uint64_t calls = 0, totalCalls = 0;
    uint32_t uni[64][2];
    bool uniValid[64];
    uint32_t frame = 0;
    void reset()
    {
        bound = 0u; depthTest = -1; depthMask = -1;
        std::memset(uniValid, 0, sizeof(uniValid));
        if ((++frame % 600u) == 0u)
            texParams.clear();
    }
} g_rdc; // FEAT-RD15

void rdSetUniform(int loc, const void *v, int type, int count) // FEAT-RD15
{
    uint32_t w[2] = {0u, 0u};
    std::memcpy(w, v, (type == RL_SHADER_UNIFORM_VEC2) ? 8 : 4);
    if (loc >= 0 && loc < 64)
    {
        if (g_rdc.uniValid[loc] && g_rdc.uni[loc][0] == w[0] && g_rdc.uni[loc][1] == w[1])
            return;
        g_rdc.uni[loc][0] = w[0]; g_rdc.uni[loc][1] = w[1]; g_rdc.uniValid[loc] = true;
    }
    ++g_rdc.calls;
    rlSetUniform(loc, v, type, count);
}
void rdDepthTest(bool on) // FEAT-RD15
{
    if (g_rdc.depthTest == (on ? 1 : 0)) return;
    g_rdc.depthTest = on ? 1 : 0; ++g_rdc.calls;
    if (on) rlEnableDepthTest(); else rlDisableDepthTest();
}
void rdDepthMask(bool on) // FEAT-RD15
{
    if (g_rdc.depthMask == (on ? 1 : 0)) return;
    g_rdc.depthMask = on ? 1 : 0; ++g_rdc.calls;
    if (on) rlEnableDepthMask(); else rlDisableDepthMask();
}
void rdBindTex(unsigned int texId, uint32_t params) // FEAT-RD15
{
    uint32_t &cached = g_rdc.texParams[texId];
    const uint32_t want = params | 0x80000000u;
    if (cached != want)
    {
        const uint32_t old = cached;
        const bool fresh = !(old & 0x80000000u);
        const int wrapS = (params & 1u) ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT;
        const int wrapT = (params & 2u) ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT;
        const int mag = (params & 4u) ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
        const int mn = (params & 8u) ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
        if (fresh || ((old ^ want) & 1u)) { rlTextureParameters(texId, RL_TEXTURE_WRAP_S, wrapS); ++g_rdc.calls; }
        if (fresh || ((old ^ want) & 2u)) { rlTextureParameters(texId, RL_TEXTURE_WRAP_T, wrapT); ++g_rdc.calls; }
        if (fresh || ((old ^ want) & 4u)) { rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, mag); ++g_rdc.calls; }
        if (fresh || ((old ^ want) & 8u)) { rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, mn); ++g_rdc.calls; }
        cached = want;
    }
    if (g_rdc.bound != texId)
    {
        rlActiveTextureSlot(0);
        rlEnableTexture(texId);
        g_rdc.bound = texId;
        g_rdc.calls += 2;
    }
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
        ++g_rdc.calls; // FEAT-RD15
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
        if (sk >= 6) // FEAT-RD25: DATE approximation
            rlSetBlendFactors(sk == 6 ? RL_ONE_MINUS_DST_ALPHA : RL_DST_ALPHA, sk == 6 ? RL_DST_ALPHA : RL_ONE_MINUS_DST_ALPHA, RL_FUNC_ADD);
        else
        rlSetBlendFactors(glf(sk), glf(dk), ek == 0 ? RL_FUNC_ADD : ek == 1 ? RL_FUNC_SUBTRACT : RL_FUNC_REVERSE_SUBTRACT);
        rlSetBlendMode(RL_BLEND_CUSTOM);
    }
    rlEnableShader(g_rdGpu.shader.id);
    rlEnableVertexArray(vao);
    g_rdc.bound = 0u; // FEAT-RD15: raylib flush may rebind default texture
    g_rdc.calls += 4;
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

void uploadRdSlot(RdGpuSlot &slot, const std::vector<RdVert> &verts, const std::vector<uint32_t> &indices, std::vector<RdBatch> &batches) // FEAT-RD14
{
    const size_t nv = verts.size();
    const size_t ni = indices.size();
    if (nv == 0 || ni == 0)
    {
        slot.vertCount = 0;
        slot.indexCount = 0;
        return;
    }

    auto &idx16 = g_rdGpu.idx16Scratch;
    if (nv <= 65536u)
    {
        idx16.resize(ni);
        const uint32_t *src = indices.data();
        uint16_t *dst = idx16.data();
        for (size_t i = 0; i < ni; ++i)
            dst[i] = static_cast<uint16_t>(src[i]);
        for (RdBatch &b : batches) // FEAT-RD14b
            b.baseVertex = 0u;
    }
    else
    {
        idx16.clear();
        idx16.reserve(ni);
        auto &split = g_rdGpu.splitBatchScratch;
        split.clear();
        split.reserve(batches.size() + 8u);
        for (const RdBatch &b : batches)
        {
            if (b.count < 3u)
                continue;
            const uint32_t end = b.first + b.count;
            uint32_t chunkStartTri = b.first;
            uint32_t minV = indices[b.first];
            uint32_t maxV = indices[b.first];
            for (uint32_t t = b.first; t + 2u < end; t += 3u)
            {
                const uint32_t i0 = indices[t];
                const uint32_t i1 = indices[t + 1u];
                const uint32_t i2 = indices[t + 2u];
                const uint32_t tMin = std::min(i0, std::min(i1, i2));
                const uint32_t tMax = std::max(i0, std::max(i1, i2));
                const uint32_t nextMin = std::min(minV, tMin);
                const uint32_t nextMax = std::max(maxV, tMax);
                if (nextMax - nextMin >= 65536u && t > chunkStartTri)
                {
                    RdBatch nb = b;
                    nb.first = static_cast<uint32_t>(idx16.size());
                    nb.count = t - chunkStartTri;
                    nb.baseVertex = minV;
                    for (uint32_t k = chunkStartTri; k < t; ++k)
                        idx16.push_back(static_cast<uint16_t>(indices[k] - minV));
                    split.push_back(nb);
                    chunkStartTri = t;
                    minV = tMin;
                    maxV = tMax;
                }
                else
                {
                    minV = nextMin;
                    maxV = nextMax;
                }
            }
            if (chunkStartTri < end)
            {
                RdBatch nb = b;
                nb.first = static_cast<uint32_t>(idx16.size());
                nb.count = end - chunkStartTri;
                nb.baseVertex = minV;
                for (uint32_t k = chunkStartTri; k < end; ++k)
                    idx16.push_back(static_cast<uint16_t>(indices[k] - minV));
                split.push_back(nb);
            }
        }
        batches.swap(split);
    }

    const size_t nIdx = idx16.size();
    bool rebindVao = false;
    if (nv > slot.vboCapVerts)
    {
        if (slot.vbo != 0)
            rlUnloadVertexBuffer(slot.vbo);
        const size_t newCap = std::max<size_t>(nv, slot.vboCapVerts ? slot.vboCapVerts * 2 : 4096);
        slot.vbo = rlLoadVertexBuffer(nullptr, static_cast<int>(newCap * sizeof(RdVert)), true);
        slot.vboCapVerts = newCap;
        rebindVao = true;
    }
    if (nIdx > slot.eboCapIndices)
    {
        if (slot.ebo != 0)
            rlUnloadVertexBuffer(slot.ebo);
        const size_t newCap = std::max<size_t>(nIdx, slot.eboCapIndices ? slot.eboCapIndices * 2 : 8192);
        slot.ebo = rlLoadVertexBufferElement(nullptr, static_cast<int>(newCap * sizeof(uint16_t)), true);
        slot.eboCapIndices = newCap;
        rebindVao = true;
    }
    if (rebindVao)
    {
        rlEnableVertexArray(slot.vao);
        rlEnableVertexBuffer(slot.vbo);
        rlSetVertexAttribute(0, 4, RL_FLOAT, false, sizeof(RdVert), 0);
        rlEnableVertexAttribute(0);
        rlSetVertexAttribute(1, 4, RL_UNSIGNED_BYTE, true, sizeof(RdVert), 16);
        rlEnableVertexAttribute(1);
        rlSetVertexAttribute(2, 2, RL_FLOAT, false, sizeof(RdVert), 20);
        rlEnableVertexAttribute(2);
        rlEnableVertexBufferElement(slot.ebo);
        rlDisableVertexArray();
        rlDisableVertexBuffer();
        rlDisableVertexBufferElement();
        slot.curBaseVertex = 0u; // FEAT-RD14b: attributes re-pointed at offset 0
    }
    rlUpdateVertexBuffer(slot.vbo, verts.data(), static_cast<int>(nv * sizeof(RdVert)), 0);
    if (nIdx > 0)
        rlUpdateVertexBufferElements(slot.ebo, idx16.data(), static_cast<int>(nIdx * sizeof(uint16_t)), 0);
    slot.vertCount = nv;
    slot.indexCount = nIdx;
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

float g_rdAspectK = 1.0f; // FEAT-RD16: published each frame by present loop when PS2X_ASPECT=fill & takeover

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

void ps2xRdPublishLean(std::vector<RdVert> &verts, std::vector<uint32_t> &indices, std::vector<RdBatch> &batches, uint32_t skippedDraws, uint32_t maskedDraws, float vpScaleZ, float vpOffsetZ) // FEAT-RD14
{
    uint32_t sMin = 0u; // FEAT-RD13
    uint32_t sMax = 0u; // FEAT-RD13
    for (const RdBatch &batch : batches)
    {
        const uint32_t ms = batch.kickSerial & 0x3FFFFFFFu;
        if (ms != 0u)
        {
            if (sMin == 0u || ms < sMin)
                sMin = ms;
            if (ms > sMax)
                sMax = ms;
        }
    }

    std::lock_guard<std::mutex> lock(g_rdMutex);
    if (vpScaleZ != 0.0f) // FEAT-RD11
    {
        g_rdLastVpScaleZ = vpScaleZ;
        g_rdLastVpOffsetZ = vpOffsetZ;
    }
    Rd3DFrame &slot = g_rd3dRing[g_rd3dWriteIdx]; // FEAT-RD13
    slot.verts.swap(verts);
    slot.indices.swap(indices);
    slot.batches.swap(batches);
    slot.skipped = skippedDraws;
    slot.masked = maskedDraws;
    slot.vpScaleZ = g_rdLastVpScaleZ;
    slot.vpOffsetZ = g_rdLastVpOffsetZ;
    slot.sMin = sMin;
    slot.sMax = sMax;
    slot.seq = ++g_rd3dSeq;
    g_rd3dWriteIdx = (g_rd3dWriteIdx + 1u) % 4u;
    if (g_rd3dCount < 4u)
        ++g_rd3dCount;
    g_rdEverPublished = true;
}

void ps2xRdPublish(const RdVert *v, size_t n, const RdBatch *b, size_t nb, uint32_t skippedDraws, uint32_t maskedDraws, float vpScaleZ, float vpOffsetZ) // FEAT-RD2 // FEAT-RD11
{
    static thread_local std::vector<RdVert> t_staging;
    static thread_local std::vector<uint32_t> t_idxStaging; // FEAT-RD14
    static thread_local std::vector<RdBatch> t_batchStaging;
    if (v && n > 0)
    {
        t_staging.assign(v, v + n);
        t_idxStaging.resize(n);
        for (size_t i = 0; i < n; ++i)
            t_idxStaging[i] = static_cast<uint32_t>(i);
    }
    else
    {
        t_staging.clear();
        t_idxStaging.clear();
    }
    if (b && nb > 0)
        t_batchStaging.assign(b, b + nb);
    else
        t_batchStaging.clear();
    ps2xRdPublishLean(t_staging, t_idxStaging, t_batchStaging, skippedDraws, maskedDraws, vpScaleZ, vpOffsetZ);
}

void ps2xRdPublish2D(const RdVert *v, size_t n, const Rd2DBatch *b, size_t nb, uint32_t rtSkipped, uint32_t pointLineSkipped, uint32_t gMin, uint32_t gMax) // FEAT-RD8 // FEAT-RD13
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
    std::swap(g_rd2dFrames[1], g_rd2dFrames[0]); // FEAT-RD13: previous <- newest
    g_rd2dFrames[0].verts.swap(t_staging2d);
    g_rd2dFrames[0].batches.swap(t_batchStaging2d);
    g_rd2dFrames[0].rtSkipped = rtSkipped;
    g_rd2dFrames[0].pointLineSkipped = pointLineSkipped;
    g_rd2dFrames[0].gMin = gMin;
    g_rd2dFrames[0].gMax = gMax;
    g_rd2dFrames[0].valid = true;
    ++g_rd2dSeq;
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

static const bool g_rdCamOn = []{ const char *e = std::getenv("PS2X_RD_CAMLOG"); return e && e[0] == '1'; }(); // PROBE-RD26
struct RdCamTl // PROBE-RD26
{
    std::vector<std::string> lines;
    std::map<uint32_t, uint32_t> hist;
    bool open = false;
    uint32_t hash = 0u, s0 = 0u, s1 = 0u, batches = 0u, tris = 0u, small2d = 0u, pendSmall = 0u;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    void begin() { lines.clear(); hist.clear(); open = false; pendSmall = 0u; }
    void push(const char *b) { if (lines.size() < 56u) lines.emplace_back(b); }
    void flush()
    {
        if (!open) return;
        char b[256];
        std::snprintf(b, sizeof(b), "[rdcam] 3D cam=%08x serial=%08x..%08x batches=%u tris=%u bbox=%d,%d-%d,%d small2d=%u\n",
                      hash, s0, s1, batches, tris, (int)bx0, (int)by0, (int)bx1, (int)by1, small2d);
        push(b);
        open = false;
    }
    void add3d(uint32_t h, uint32_t serial, uint32_t t, float x0, float y0, float x1, float y1, bool hasBox)
    {
        ++hist[h];
        if (open && hash != h) flush();
        if (!open)
        {
            open = true; hash = h; s0 = serial; batches = 0u; tris = 0u; small2d = pendSmall; pendSmall = 0u;
            bx0 = 1e30f; by0 = 1e30f; bx1 = -1e30f; by1 = -1e30f;
        }
        s1 = serial; ++batches; tris += t;
        if (hasBox) { bx0 = std::min(bx0, x0); by0 = std::min(by0, y0); bx1 = std::max(bx1, x1); by1 = std::max(by1, y1); }
    }
    void add2d(bool big, uint32_t after, uint32_t t, float x0, float y0, float x1, float y1, uint64_t zbuf, uint64_t test, uint64_t frame, uint64_t alpha, uint64_t prim)
    {
        if (!big)
        {
            if (open) ++small2d; else ++pendSmall;
            return;
        }
        flush();
        char b[320];
        std::snprintf(b, sizeof(b), "[rdcam] 2D after=%08x tris=%u bbox=%d,%d-%d,%d zmsk=%u zte=%u ztst=%u fbmsk=%08x alpha=%llx prim=%llx\n",
                      after, t, (int)x0, (int)y0, (int)x1, (int)y1, (unsigned)((zbuf >> 32) & 1ull), (unsigned)((test >> 16) & 1ull),
                      (unsigned)((test >> 17) & 3ull), (unsigned)(frame >> 32), (unsigned long long)alpha, (unsigned long long)prim);
        push(b);
    }
    void finish()
    {
        flush();
        for (const std::string &l : lines) ps2xSafeLog("%s", l.c_str());
        std::vector<std::pair<uint32_t, uint32_t>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        char b[256];
        int n = std::snprintf(b, sizeof(b), "[rdcam] hist cams=%u:", (unsigned)v.size());
        for (size_t i = 0; i < v.size() && i < 4u; ++i)
            n += std::snprintf(b + n, sizeof(b) - n, " %08x=%u", v[i].first, v[i].second);
        ps2xSafeLog("%s\n", b);
    }
};
static RdCamTl g_camTl; // PROBE-RD26
static const bool g_rd24On = []{ const char *e = std::getenv("PS2X_RD_BIGLOG"); return e && e[0] == '1'; }(); // PROBE-RD24
static std::vector<RdVert> g_rd24Verts[4]; // PROBE-RD24
static std::vector<uint16_t> g_rd24Idx[4]; // PROBE-RD24

bool ps2xRdDraw(float x, float y, float w, float h) // FEAT-RD1
{
    bool hasNew2d = false; // FEAT-RD8
    bool needRealign = false; // FEAT-RD13
    bool pendingUpload[4] = {}; // FEAT-RD14
    {
        std::lock_guard<std::mutex> lock(g_rdMutex);
        if (!g_rdEverPublished && !g_rdGpu.everUploaded)
        {
            if (ps2xRdTakeoverOn()) // FEAT-RD12: clear to black instead of drawing stale GS texture
                return true;
            return false;
        }
        if (g_rd3dSeq != g_rdGpu.last3dSeq) // FEAT-RD13 // FEAT-RD14
        {
            g_rdGpu.ring3dCount = g_rd3dCount;
            const size_t oldest = (g_rd3dWriteIdx + 4u - g_rd3dCount) % 4u;
            for (size_t i = 0; i < g_rd3dCount; ++i)
            {
                const size_t idx = (oldest + i) % 4u;
                g_rdGpu.activeOrder[i] = idx;
                Rd3DFrame &rf = g_rd3dRing[idx];
                RdGpuSlot &gs = g_rdGpu.slots[idx];
                if (rf.seq != 0u && rf.seq != gs.seq)
                {
                    g_rdGpu.pendingVerts[idx].swap(rf.verts);
                    g_rdGpu.pendingIndices[idx].swap(rf.indices);
                    gs.batches = rf.batches;
                    gs.seq = rf.seq;
                    gs.skipped = rf.skipped;
                    gs.masked = rf.masked;
                    gs.vpScaleZ = rf.vpScaleZ;
                    gs.vpOffsetZ = rf.vpOffsetZ;
                    gs.sMin = rf.sMin;
                    gs.sMax = rf.sMax;
                    pendingUpload[idx] = true;
                }
            }
            g_rdGpu.last3dSeq = g_rd3dSeq;
            needRealign = true;
        }
        if (g_rd2dSeq != g_rdGpu.last2dSeq) // FEAT-RD13
        {
            g_rdGpu.frames2d[0] = g_rd2dFrames[0];
            g_rdGpu.frames2d[1] = g_rd2dFrames[1];
            g_rdGpu.last2dSeq = g_rd2dSeq;
            needRealign = true;
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
            "uniform vec4 uVpA;\n"    // FEAT-RD21: sx, sy, ox, oy
            "uniform vec4 uVpB;\n"    // FEAT-RD21: offx, offy (12.4), aspect k, enable
            "out vec4 vCol;\n"
            "out vec2 vUv;\n" // FEAT-RD3
            "void main() {\n"
            "    if (uMode2d != 0) {\n" // FEAT-RD8
            "        float ndcZ = (uMode2d == 2 && abs(uVpZ.x) > 1e-6) ? clamp(((aPos.z - uVpZ.y) / uVpZ.x) * uFlip.z, -1.0, 1.0) : 0.0;\n" // FEAT-RD11 // FEAT-RD20
            "        gl_Position = vec4(aPos.x / uFbSize.x * 2.0 - 1.0, 1.0 - aPos.y / uFbSize.y * 2.0, ndcZ, 1.0);\n"
            "    } else if (uVpB.w > 0.5) {\n" // FEAT-RD21: guard-band VU clip -> GS fb pixels -> ndc (linear in x,y,w)
            "        float px = aPos.x * uVpA.x + aPos.w * uVpA.z - aPos.w * uVpB.x * (1.0 / 16.0);\n" // FEAT-RD21b
            "        float py = aPos.y * uVpA.y + aPos.w * uVpA.w - aPos.w * uVpB.y * (1.0 / 16.0);\n" // FEAT-RD21b
            "        float nx = (2.0 * px / uFbSize.x - aPos.w) / uVpB.z;\n"
            "        float ny = aPos.w - 2.0 * py / uFbSize.y;\n"
            "        gl_Position = vec4(nx * uFlip.x, ny * (-uFlip.y), aPos.z * uFlip.z, aPos.w);\n"
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
        g_rdGpu.vpALoc = GetShaderLocation(g_rdGpu.shader, "uVpA");       // FEAT-RD21
        g_rdGpu.vpBLoc = GetShaderLocation(g_rdGpu.shader, "uVpB");       // FEAT-RD21
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
        for (size_t i = 0; i < 4u; ++i) // FEAT-RD14
            g_rdGpu.slots[i].vao = rlLoadVertexArray();
        g_rdGpu.vao2d = rlLoadVertexArray(); // FEAT-RD8
    }

    for (size_t idx = 0; idx < 4u; ++idx) // FEAT-RD14
    {
        if (pendingUpload[idx])
        {
            g_rdGpu.everUploaded = true;
            uploadRdSlot(g_rdGpu.slots[idx], g_rdGpu.pendingVerts[idx], g_rdGpu.pendingIndices[idx], g_rdGpu.slots[idx].batches);
            if (g_rd24On || g_rdCamOn) // PROBE-RD24 // PROBE-RD26: CPU copy of uploaded verts + final 16-bit indices (relative to batch.baseVertex)
            {
                g_rd24Verts[idx] = g_rdGpu.pendingVerts[idx];
                if (g_rdGpu.slots[idx].indexCount != 0)
                    g_rd24Idx[idx] = g_rdGpu.idx16Scratch;
                else
                    g_rd24Idx[idx].clear();
            }
        }
    }

    if (needRealign) // FEAT-RD13 // FEAT-RD14
    {
        uint32_t ringMin = 0u;
        uint32_t ringMax = 0u;
        for (size_t i = 0; i < g_rdGpu.ring3dCount; ++i)
        {
            const RdGpuSlot &f = g_rdGpu.slots[g_rdGpu.activeOrder[i]];
            if (f.sMin == 0u)
                continue;
            if (ringMin == 0u || f.sMin < ringMin)
                ringMin = f.sMin;
            if (f.sMax > ringMax)
                ringMax = f.sMax;
        }

        const Rd2DFrame *chosen2d = nullptr;
        bool usedFallbackPrev = false;
        const Rd2DFrame &newest2d = g_rdGpu.frames2d[0];
        const Rd2DFrame &prev2d = g_rdGpu.frames2d[1];
        if (newest2d.valid)
        {
            const bool covered = (newest2d.gMin == 0u) ||
                                 (ringMin != 0u && ringMin <= newest2d.gMin && newest2d.gMax <= ringMax);
            if (covered)
            {
                chosen2d = &newest2d;
            }
            else if (prev2d.valid)
            {
                chosen2d = &prev2d;
                usedFallbackPrev = true;
            }
            else
            {
                chosen2d = &newest2d;
            }
        }

        if (chosen2d != nullptr)
        {
            g_rdGpu.local2dScratch = chosen2d->verts;
            g_rdGpu.local2dBatches = chosen2d->batches;
            g_rdGpu.lastRtSkipped = chosen2d->rtSkipped;
            g_rdGpu.lastPointLineSkipped = chosen2d->pointLineSkipped;
            g_rdGpu.activeGMin = chosen2d->gMin;
            hasNew2d = true;
        }

        g_rdGpu.drawBatches.clear();
        uint32_t framesUsed = 0u;
        uint64_t fVerts = 0u;
        uint64_t fIndices = 0u;
        if (chosen2d == nullptr)
        {
            if (g_rdGpu.ring3dCount > 0u)
            {
                const uint32_t slotIdx = static_cast<uint32_t>(g_rdGpu.activeOrder[g_rdGpu.ring3dCount - 1u]);
                const RdGpuSlot &f3 = g_rdGpu.slots[slotIdx];
                g_rdGpu.drawBatches.reserve(f3.batches.size());
                for (const RdBatch &b : f3.batches)
                    g_rdGpu.drawBatches.push_back(RdDrawBatch{b, slotIdx});
                fVerts = f3.vertCount;
                fIndices = f3.indexCount;
                g_rdGpu.lastSkippedDraws = f3.skipped;
                g_rdGpu.lastMaskedDraws = f3.masked;
                if (f3.vpScaleZ != 0.0f)
                {
                    g_rdGpu.vpScaleZ = f3.vpScaleZ;
                    g_rdGpu.vpOffsetZ = f3.vpOffsetZ;
                }
                framesUsed = 1u;
            }
        }
        else if (chosen2d->gMin > 0u)
        {
            const uint32_t gMin = chosen2d->gMin;
            const uint32_t gMax = chosen2d->gMax;
            for (size_t i = 0; i < g_rdGpu.ring3dCount; ++i)
            {
                const uint32_t slotIdx = static_cast<uint32_t>(g_rdGpu.activeOrder[i]);
                const RdGpuSlot &f3 = g_rdGpu.slots[slotIdx];
                if (f3.batches.empty() || f3.sMin == 0u || f3.sMax < gMin || f3.sMin > gMax)
                    continue;
                bool anyFromFrame = false;
                for (const RdBatch &b : f3.batches)
                {
                    const uint32_t ms = b.kickSerial & 0x3FFFFFFFu;
                    if (ms >= gMin && ms <= gMax)
                    {
                        if (!anyFromFrame)
                        {
                            anyFromFrame = true;
                            fVerts += f3.vertCount;
                            g_rdGpu.lastSkippedDraws = f3.skipped;
                            g_rdGpu.lastMaskedDraws = f3.masked;
                            if (f3.vpScaleZ != 0.0f)
                            {
                                g_rdGpu.vpScaleZ = f3.vpScaleZ;
                                g_rdGpu.vpOffsetZ = f3.vpOffsetZ;
                            }
                        }
                        g_rdGpu.drawBatches.push_back(RdDrawBatch{b, slotIdx});
                        fIndices += b.count;
                    }
                }
                if (anyFromFrame)
                    ++framesUsed;
            }
        }
        g_rdGpu.frameVerts = fVerts;
        g_rdGpu.frameIndices = fIndices;
        g_rdGpu.frameTris = fIndices / 3u;
        g_rdGpu.curAligned = (chosen2d != nullptr && !usedFallbackPrev);
        g_rdGpu.curFallbackPrev = usedFallbackPrev;
        g_rdGpu.curSpanned2 = (framesUsed >= 2u);
    }

    if (hasNew2d) // FEAT-RD8
    {
        // FEAT-RD16: Hor+ 2D HUD layout when PS2X_ASPECT=fill (g_rdAspectK > 1)
        const float fbW = static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed));
        const float fbH = static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed));
        if (g_rdAspectK > 1.0f && fbW > 0.0f && fbH > 0.0f && w > 0.0f && h > 0.0f)
        {
            const float s = h / fbH;
            const float scaleToShader = (s * fbW) / w;
            for (const Rd2DBatch &b : g_rdGpu.local2dBatches)
            {
                if (b.count == 0u || static_cast<size_t>(b.first) + b.count > g_rdGpu.local2dScratch.size())
                    continue;
                float minX = g_rdGpu.local2dScratch[b.first].pos[0];
                float maxX = minX;
                for (uint32_t k = 1u; k < b.count; ++k)
                {
                    const float vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0];
                    if (vx2 < minX) minX = vx2;
                    if (vx2 > maxX) maxX = vx2;
                }
                if (maxX - minX >= 0.8f * fbW)
                    continue; // full-width draw stretches to full window
                const float cx = 0.5f * (minX + maxX);
                if (cx < 0.5f * fbW)
                {
                    for (uint32_t k = 0u; k < b.count; ++k)
                        g_rdGpu.local2dScratch[b.first + k].pos[0] *= scaleToShader;
                }
                else
                {
                    for (uint32_t k = 0u; k < b.count; ++k)
                    {
                        float &vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0];
                        vx2 = fbW - (fbW - vx2) * scaleToShader;
                    }
                }
            }
        }
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
    g_rdc.reset(); g_rdc.calls = 0; g_rdc.depthTest = 1; // FEAT-RD15
    rlDisableBackfaceCulling();

    uint64_t unsupBlend = 0; // FEAT-RD2
    uint64_t approxAd = 0;   // FEAT-RD7
    uint64_t partialMask = 0; // FEAT-RD9
    static const bool s_draw2d = []{ const char *e = std::getenv("PS2X_RD_2D"); return !(e && e[0] == '0'); }(); // FEAT-RD9
    static const bool s_2dDepth = []{ const char *e = std::getenv("PS2X_RD_2DZ"); return !(e && e[0] == '0'); }(); // FEAT-RD11
    uint64_t draws2dThisFrame = 0; // FEAT-RD8
    uint64_t ztested2dThisFrame = 0; // FEAT-RD11
    uint64_t zclears2dThisFrame = 0; // FEAT-RD20
    struct RdPassEnt { uint32_t fbp, xo, yo, sx0, sy0, sx1, sy1; float vox, voy; uint32_t n; }; // FEAT-RD22
    RdPassEnt passEnts[8]; // FEAT-RD22
    int passN = 0;         // FEAT-RD22
    static const bool s_passLog = []{ const char *e = std::getenv("PS2X_RD_PASSLOG"); return e && e[0] == '1'; }(); // FEAT-RD22
    static uint64_t s_scissorSkipped = 0; // FEAT-RD22
    static uint64_t s_noState3dTotal = 0, s_rtPass3dTotal = 0; // FEAT-RD23
    if ((!g_rdGpu.drawBatches.empty() || g_rdGpu.gpu2dVertCount > 0) && g_rdGpu.shader.id != 0) // FEAT-RD8 // FEAT-RD14
    {
        rlEnableShader(g_rdGpu.shader.id);
        int mode2d = 0; // FEAT-RD8
        if (g_rdGpu.mode2dLoc >= 0)
            rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
        const float fbSize[2] = {
            static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed)),
            static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed))
        };
        if (g_rdGpu.fbSizeLoc >= 0)
            rdSetUniform(g_rdGpu.fbSizeLoc, fbSize, RL_SHADER_UNIFORM_VEC2, 1);
        const float vpZ[2] = { g_rdGpu.vpScaleZ, g_rdGpu.vpOffsetZ }; // FEAT-RD11
        if (g_rdGpu.vpZLoc >= 0)
            rdSetUniform(g_rdGpu.vpZLoc, vpZ, RL_SHADER_UNIFORM_VEC2, 1);
        unsigned int curVao = 0u; // FEAT-RD14
        int prevBlend = -1; // FEAT-RD2b
        size_t i2d = 0; // FEAT-RD8
        const size_t n2d = (s_draw2d && g_rdGpu.gpu2dVertCount > 0 && g_rdGpu.vao2d != 0) ? g_rdGpu.local2dBatches.size() : 0;

        bool scisOn = false; // FEAT-RD22
        int scisRect[4] = {0, 0, 0, 0};
        auto applyScissor = [&](uint64_t packed, bool have) -> bool // FEAT-RD22: false = batch fully outside, skip
        {
            bool on = false;
            int r[4] = {0, 0, 0, 0};
            if (have && packed != 0ull && fbSize[0] > 0.0f && fbSize[1] > 0.0f)
            {
                const float x0 = static_cast<float>(packed & 0x7FFull);
                const float x1 = static_cast<float>((packed >> 16) & 0x7FFull);
                const float y0 = static_cast<float>((packed >> 32) & 0x7FFull);
                const float y1 = static_cast<float>((packed >> 48) & 0x7FFull);
                if (x0 >= fbSize[0] || y0 >= fbSize[1] || x1 < x0 || y1 < y0)
                    return false;
                const float k = (g_rdAspectK > 1.0f) ? g_rdAspectK : 1.0f;
                const float xe = std::min(x1 + 1.0f, fbSize[0]);
                const float ye = std::min(y1 + 1.0f, fbSize[1]);
                float l = static_cast<float>(vx), rr = static_cast<float>(vx + vw);
                if (!(x0 <= 0.0f && xe >= fbSize[0])) // full-width scissor = whole box
                {
                    l = static_cast<float>(vx) + (((2.0f * x0 / fbSize[0] - 1.0f) / k + 1.0f) * 0.5f) * static_cast<float>(vw);
                    rr = static_cast<float>(vx) + (((2.0f * xe / fbSize[0] - 1.0f) / k + 1.0f) * 0.5f) * static_cast<float>(vw);
                }
                const float bt = static_cast<float>(vy) + (1.0f - ye / fbSize[1]) * static_cast<float>(vh);
                const float tp = static_cast<float>(vy) + (1.0f - y0 / fbSize[1]) * static_cast<float>(vh);
                r[0] = static_cast<int>(std::floor(l));
                r[1] = static_cast<int>(std::floor(bt));
                r[2] = std::max(0, static_cast<int>(std::ceil(rr)) - r[0]);
                r[3] = std::max(0, static_cast<int>(std::ceil(tp)) - r[1]);
                on = true;
            }
            if (on != scisOn || (on && std::memcmp(r, scisRect, sizeof(r)) != 0))
            {
                if (on)
                {
                    if (!scisOn)
                        rlEnableScissorTest();
                    rlScissor(r[0], r[1], r[2], r[3]);
                    std::memcpy(scisRect, r, sizeof(r));
                }
                else
                    rlDisableScissorTest();
                scisOn = on;
                ++g_rdc.calls;
            }
            return true;
        };

        bool camDump = false; // PROBE-RD26
        if (g_rdCamOn)
        {
            static std::chrono::steady_clock::time_point s_camLast{};
            const auto nowC = std::chrono::steady_clock::now();
            if (s_camLast.time_since_epoch().count() == 0 || nowC - s_camLast >= std::chrono::seconds(5))
            {
                s_camLast = nowC;
                camDump = true;
                g_camTl.begin();
            }
        }
        auto draw2dUntil = [&](uint32_t maxSerial, bool drawAll) // FEAT-RD8
        {
            const uint32_t maxS = maxSerial & 0x3FFFFFFFu; // FEAT-RD10
            if (i2d >= n2d || (!drawAll && (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) >= maxS))
                return;
            mode2d = 1;
            if (g_rdGpu.mode2dLoc >= 0)
                rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            rdDepthTest(false);
            rdDepthMask(false);
            rlEnableVertexArray(g_rdGpu.vao2d);

            while (i2d < n2d && (drawAll || (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) < maxS))
            {
                const Rd2DBatch &b0 = g_rdGpu.local2dBatches[i2d];
                if (b0.count == 0u)
                {
                    ++i2d;
                    continue;
                }
                if (((b0.prim >> 4) & 1ull) && ps2xRdIsFbFeedback(b0.texSerial)) // FEAT-RD28b
                {
                    ++s_fbFeedbackSkipped;
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
                if (!applyScissor(b0.scissor, true)) // FEAT-RD22
                {
                    ++s_scissorSkipped;
                    ++i2d;
                    continue;
                }
                const bool use2dZ = s_2dDepth && zte && (ztst == 2u || ztst == 3u) && (g_rdGpu.vpScaleZ != 0.0f);
                const bool zWrite2d = s_2dDepth && !((b0.zbuf >> 32) & 1ull) && (g_rdGpu.vpScaleZ != 0.0f); // FEAT-RD20
                const bool zAlways2d = zWrite2d && !use2dZ; // FEAT-RD20: ZTE=0 or ZTST=1 with Z writes -> ALWAYS + write
                const int wantMode2d = (use2dZ || zAlways2d) ? 2 : 1;
                if (wantMode2d != mode2d)
                {
                    mode2d = wantMode2d;
                    if (g_rdGpu.mode2dLoc >= 0)
                        rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
                }
                if (use2dZ)
                {
                    rdDepthTest(true);
                    if ((b0.zbuf >> 32) & 1ull)
                        rdDepthMask(false);
                    else
                        rdDepthMask(true);
                }
                else if (zAlways2d) // FEAT-RD20
                {
                    rdDepthTest(true);
                    rdDepthMask(true);
                }
                else
                {
                    rdDepthTest(false);
                    rdDepthMask(false);
                }
#ifdef _WIN32
                if (g_rdAlwaysFunc != zAlways2d) // FEAT-RD20
                {
                    glDepthFunc(zAlways2d ? 0x0207u : 0x0203u); // GL_ALWAYS : GL_LEQUAL
                    g_rdAlwaysFunc = zAlways2d;
                }
#endif
                applyRdFbmsk(b0.frame, true, partialMask); // FEAT-RD9
                float fixAlpha = -1.0f;
                int blendState = computeRdBlend(b0.prim, b0.alpha, unsupBlend, approxAd, fixAlpha);
                blendState = applyRdDate(b0.test, b0.prim, blendState); // FEAT-RD25
                if (blendState >= 1 + 6 * 128 && g_rdMaskCur != (g_rdMaskCur & 7)) // FEAT-RD25: no alpha write
                {
                    g_rdMaskCur &= 7;
                    rlColorMask((g_rdMaskCur & 1) != 0, (g_rdMaskCur & 2) != 0, (g_rdMaskCur & 4) != 0, false);
                }
                if (blendState != prevBlend)
                {
                    applyRdBlend(blendState, g_rdGpu.vao2d);
                    prevBlend = blendState;
                }
                int atstVal = static_cast<int>(atst);
                int arefVal = static_cast<int>((b0.test >> 4) & 0xFFull);
                if (g_rdGpu.atstLoc >= 0)
                    rdSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.arefLoc >= 0)
                    rdSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.fixLoc >= 0)
                    rdSetUniform(g_rdGpu.fixLoc, &fixAlpha, RL_SHADER_UNIFORM_FLOAT, 1);

                int texVal = 0, tfxVal = 0, tccVal = 0;
                unsigned int texId = 0u;
                if ((b0.prim >> 4) & 1ull)
                {
                    texId = ps2xRdGetTextureForBatch(b0.texSerial, b0.tex0, b0.texa);
                    if (texId != 0u)
                    {
                        const uint32_t wms = static_cast<uint32_t>(b0.clamp & 3ull);
                        const uint32_t wmt = static_cast<uint32_t>((b0.clamp >> 2) & 3ull);
                        rdBindTex(texId, (wms != 0u ? 1u : 0u) | (wmt != 0u ? 2u : 0u) | ((((b0.tex1 >> 5) & 1ull) != 0ull) ? 4u : 0u) | ((((b0.tex1 >> 6) & 7ull) != 0ull) ? 8u : 0u)); // FEAT-RD15
                        texVal = 1;
                        tfxVal = static_cast<int>((b0.tex0 >> 35) & 3ull);
                        tccVal = static_cast<int>((b0.tex0 >> 34) & 1ull);
                    }
                }
                if (g_rdGpu.texLoc >= 0)
                    rdSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.tfxLoc >= 0)
                    rdSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
                if (g_rdGpu.tccLoc >= 0)
                    rdSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);

                uint32_t totalCount = b0.count;
                size_t j = i2d + 1;
                while (j < n2d && (drawAll || (g_rdGpu.local2dBatches[j].afterSerial & 0x3FFFFFFFu) < maxS)) // FEAT-RD10
                {
                    const Rd2DBatch &bn = g_rdGpu.local2dBatches[j];
                    if (bn.first != b0.first + totalCount ||
                        bn.prim != b0.prim || bn.tex0 != b0.tex0 || bn.texa != b0.texa ||
                        bn.clamp != b0.clamp || bn.tex1 != b0.tex1 || bn.alpha != b0.alpha ||
                        bn.test != b0.test || bn.frame != b0.frame || bn.zbuf != b0.zbuf || bn.scissor != b0.scissor) // FEAT-RD22
                        break;
                    if (texVal != 0 && ps2xRdGetTextureForBatch(bn.texSerial, bn.tex0, bn.texa) != texId)
                        break;
                    totalCount += bn.count;
                    ++j;
                }
                if (camDump) // PROBE-RD26
                {
                    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                    const size_t e2 = std::min<size_t>(static_cast<size_t>(b0.first) + totalCount, g_rdGpu.local2dScratch.size());
                    for (size_t k = b0.first; k < e2; ++k)
                    {
                        const float *pp = g_rdGpu.local2dScratch[k].pos;
                        x0 = std::min(x0, pp[0]); x1 = std::max(x1, pp[0]);
                        y0 = std::min(y0, pp[1]); y1 = std::max(y1, pp[1]);
                    }
                    x0 = std::max(0.0f, std::min(x0, fbSize[0])); x1 = std::max(0.0f, std::min(x1, fbSize[0]));
                    y0 = std::max(0.0f, std::min(y0, fbSize[1])); y1 = std::max(0.0f, std::min(y1, fbSize[1]));
                    const bool big2 = (x1 - x0) * (y1 - y0) > 0.20f * fbSize[0] * fbSize[1];
                    g_camTl.add2d(big2, b0.afterSerial, totalCount / 3u, x0, y0, x1, y1, b0.zbuf, b0.test, b0.frame, b0.alpha, b0.prim);
                }
                rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
                ++draws2dThisFrame;
                if (use2dZ) // FEAT-RD11
                    ++ztested2dThisFrame;
                if (zWrite2d) // FEAT-RD20
                    ++zclears2dThisFrame;
                i2d = j;
            }
#ifdef _WIN32
            if (g_rdAlwaysFunc) // FEAT-RD20: restore LEQUAL
            {
                glDepthFunc(0x0203u);
                g_rdAlwaysFunc = false;
            }
#endif

            mode2d = 0;
            if (g_rdGpu.mode2dLoc >= 0)
                rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            curVao = 0u; // FEAT-RD14
        };

        if (g_rdGpu.activeGMin > 0u) // FEAT-RD13: draw frame-start 2D draws (clears/backgrounds) first
            draw2dUntil(g_rdGpu.activeGMin, false);
        bool rd24Dump = false; // PROBE-RD24
        int rd24BigN = 0;
        std::vector<std::string> rd24Ref;
        if (g_rd24On)
        {
            static std::chrono::steady_clock::time_point s_rd24Last{};
            const auto now24 = std::chrono::steady_clock::now();
            if (s_rd24Last.time_since_epoch().count() == 0 || now24 - s_rd24Last >= std::chrono::seconds(5))
            {
                s_rd24Last = now24;
                rd24Dump = true;
            }
        }
        for (const RdDrawBatch &db : g_rdGpu.drawBatches) // FEAT-RD2 // FEAT-RD14
        {
            const RdBatch &batch = db.batch;
            draw2dUntil(batch.kickSerial, false); // FEAT-RD8
            RdGpuSlot &slot = g_rdGpu.slots[db.slotIdx]; // FEAT-RD14
            if (batch.count == 0u || slot.vao == 0u)
                continue;
            uint64_t gsPrim = 0ull, gsAlpha = 0ull, gsTest = 0ull, gsFrame = 0ull, gsZbuf = 0ull; // FEAT-RD17
            uint64_t gsTex0 = 0ull, gsTex1 = 0ull, gsClamp = 0ull, gsTexa = 0ull, gsFba = 0ull;   // FEAT-RD17
            const bool gsOk = ps2xRdLookupState(batch.kickSerial, gsPrim, gsAlpha, gsTest, gsFrame,
                                                gsZbuf, gsTex0, gsTex1, gsClamp, gsTexa, gsFba); // FEAT-RD17
            if (gsOk)
                ++g_rdGpu.gsStateCount;
            else
                ++g_rdGpu.parsedFallbackCount;
            const uint64_t test = gsOk ? gsTest : batch.test;
            const uint64_t zbuf = gsOk ? gsZbuf : batch.zbuf;
            const uint64_t frame = gsOk ? gsFrame : batch.frame;
            if (ps2xRdHasDisplayFbp() && !ps2xRdIsDisplayFbp(static_cast<uint32_t>(frame & 0x1FFull))) // FEAT-RD19
            {
                ++g_rdGpu.rtSkipped3d;
                continue;
            }
            { // FEAT-RD23: only main-pass batches with GS-thread state are drawn
                uint64_t xoMp = 0ull;
                if (!gsOk || !ps2xRdLookupXyoff(batch.kickSerial, xoMp))
                {
                    ++s_noState3dTotal;
                    continue;
                }
                if (!ps2xRdIsMainPass(static_cast<uint32_t>(frame & 0x1FFull), xoMp))
                {
                    ++s_rtPass3dTotal;
                    continue;
                }
            }
            const uint64_t alpha = gsOk ? gsAlpha : batch.alpha;
            constexpr uint64_t kPrimAbeTmeMask = (1ull << 6) | (1ull << 4);
            const uint64_t prim = gsOk ? ((batch.prim & ~kPrimAbeTmeMask) | (gsPrim & kPrimAbeTmeMask)) : batch.prim;
            if (((prim >> 4) & 1ull) && ps2xRdIsFbFeedback(batch.kickSerial)) // FEAT-RD28b
            {
                ++s_fbFeedbackSkipped;
                continue;
            }
            const uint64_t tex0 = gsOk ? gsTex0 : batch.tex0;
            const uint64_t tex1 = gsOk ? gsTex1 : batch.tex1;
            const uint64_t clamp = gsOk ? gsClamp : batch.clamp;
            const uint64_t texa = gsOk ? gsTexa : batch.texa;
            (void)gsFba;
            const uint32_t ate = static_cast<uint32_t>(test & 1ull);
            const uint32_t atst = ate ? static_cast<uint32_t>((test >> 1) & 7ull) : 1u;
            if (atst == 0u)
                continue;
            const uint32_t zte = static_cast<uint32_t>((test >> 16) & 1ull);
            const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
            if (zte && ztst == 0u)
                continue;
            if (!zte || ztst == 1u)
                rdDepthTest(false);
            else
                rdDepthTest(true);
            if ((zbuf >> 32) & 1ull)
                rdDepthMask(false);
            else
                rdDepthMask(true);

            // FEAT-RD7: blendState 0 = off, else 1 + sfk*64 + dfk*16 + eqk (factor kinds: 0 ZERO, 1 ONE, 2 C, 3 1-C)
            applyRdFbmsk(frame, false, partialMask); // FEAT-RD9
            float fixAlpha = -1.0f;
            int blendState = computeRdBlend(prim, alpha, unsupBlend, approxAd, fixAlpha); // FEAT-RD8
            blendState = applyRdDate(test, prim, blendState); // FEAT-RD25
            if (blendState >= 1 + 6 * 128 && g_rdMaskCur != (g_rdMaskCur & 7)) // FEAT-RD25: no alpha write
            {
                g_rdMaskCur &= 7;
                rlColorMask((g_rdMaskCur & 1) != 0, (g_rdMaskCur & 2) != 0, (g_rdMaskCur & 4) != 0, false);
            }
            if (blendState != prevBlend) // FEAT-RD2b: blend calls flush raylib batch and unbind shader/VAO
            {
                applyRdBlend(blendState, slot.vao); // FEAT-RD8 // FEAT-RD14
                prevBlend = blendState;
                curVao = slot.vao;
            }
            else if (curVao != slot.vao) // FEAT-RD14
            {
                rlEnableVertexArray(slot.vao);
                curVao = slot.vao;
            }
            if (batch.baseVertex != slot.curBaseVertex) // FEAT-RD14: >65536 vertex split path
            {
                rlEnableVertexBuffer(slot.vbo);
                const int byteOff = static_cast<int>(static_cast<size_t>(batch.baseVertex) * sizeof(RdVert));
                rlSetVertexAttribute(0, 4, RL_FLOAT, false, sizeof(RdVert), byteOff + 0);
                rlSetVertexAttribute(1, 4, RL_UNSIGNED_BYTE, true, sizeof(RdVert), byteOff + 16);
                rlSetVertexAttribute(2, 2, RL_FLOAT, false, sizeof(RdVert), byteOff + 20);
                rlDisableVertexBuffer();
                slot.curBaseVertex = batch.baseVertex;
            }
            int atstVal = static_cast<int>(atst);
            int arefVal = static_cast<int>((test >> 4) & 0xFFull);
            if (g_rdGpu.atstLoc >= 0)
                rdSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.arefLoc >= 0)
                rdSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.fixLoc >= 0) // FEAT-RD7
                rdSetUniform(g_rdGpu.fixLoc, &fixAlpha, RL_SHADER_UNIFORM_FLOAT, 1);

            { // FEAT-RD22
                uint64_t sc = 0ull;
                const bool scOk = ps2xRdLookupScissor(batch.kickSerial, sc);
                if (!applyScissor(sc, scOk))
                {
                    ++s_scissorSkipped;
                    continue;
                }
                if (s_passLog)
                {
                    uint64_t xo2 = 0ull;
                    ps2xRdLookupXyoff(batch.kickSerial, xo2);
                    RdPassEnt e{};
                    e.fbp = static_cast<uint32_t>(frame & 0x1FFull);
                    e.xo = static_cast<uint32_t>(xo2 & 0xFFFFull);
                    e.yo = static_cast<uint32_t>((xo2 >> 32) & 0xFFFFull);
                    e.sx0 = static_cast<uint32_t>(sc & 0x7FFull);
                    e.sx1 = static_cast<uint32_t>((sc >> 16) & 0x7FFull);
                    e.sy0 = static_cast<uint32_t>((sc >> 32) & 0x7FFull);
                    e.sy1 = static_cast<uint32_t>((sc >> 48) & 0x7FFull);
                    e.vox = batch.vpO[0];
                    e.voy = batch.vpO[1];
                    int found = -1;
                    for (int pi = 0; pi < passN; ++pi)
                        if (passEnts[pi].fbp == e.fbp && passEnts[pi].xo == e.xo && passEnts[pi].yo == e.yo &&
                            passEnts[pi].sx0 == e.sx0 && passEnts[pi].sx1 == e.sx1 && passEnts[pi].sy0 == e.sy0 &&
                            passEnts[pi].sy1 == e.sy1 && passEnts[pi].vox == e.vox && passEnts[pi].voy == e.voy)
                        {
                            found = pi;
                            break;
                        }
                    if (found >= 0)
                        ++passEnts[found].n;
                    else if (passN < 8)
                    {
                        e.n = 1u;
                        passEnts[passN++] = e;
                    }
                }
            }
            if (g_rdGpu.vpALoc >= 0 && g_rdGpu.vpBLoc >= 0) // FEAT-RD21
            {
                static float s_lastOfx = 0.0f, s_lastOfy = 0.0f;
                static bool s_haveOff = false, s_loggedNoOff = false;
                uint64_t xo = 0ull;
                if (ps2xRdLookupXyoff(batch.kickSerial, xo))
                {
                    s_lastOfx = static_cast<float>(xo & 0xFFFFull);
                    s_lastOfy = static_cast<float>((xo >> 32) & 0xFFFFull);
                    s_haveOff = true;
                }
                else if (!s_haveOff && !s_loggedNoOff)
                {
                    s_loggedNoOff = true;
                    ps2xSafeLog("[rd] FEAT-RD21: no XYOFFSET for serial, using 0\n");
                }
                const bool vpOk = (batch.vpS[0] != 0.0f && batch.vpS[1] != 0.0f);
                const float vpA[4] = {batch.vpS[0], batch.vpS[1], batch.vpO[0], batch.vpO[1]};
                const float vpB[4] = {s_lastOfx, s_lastOfy, (g_rdAspectK > 1.0f) ? g_rdAspectK : 1.0f, vpOk ? 1.0f : 0.0f};
                static float s_prevA[4] = {}, s_prevB[4] = {};
                static bool s_prevValid = false;
                if (!s_prevValid || std::memcmp(s_prevA, vpA, sizeof(vpA)) != 0 || std::memcmp(s_prevB, vpB, sizeof(vpB)) != 0)
                {
                    rlSetUniform(g_rdGpu.vpALoc, vpA, RL_SHADER_UNIFORM_VEC4, 1);
                    rlSetUniform(g_rdGpu.vpBLoc, vpB, RL_SHADER_UNIFORM_VEC4, 1);
                    std::memcpy(s_prevA, vpA, sizeof(vpA));
                    std::memcpy(s_prevB, vpB, sizeof(vpB));
                    s_prevValid = true;
                    g_rdc.calls += 2;
                }
            }

            int texVal = 0; // FEAT-RD3
            int tfxVal = 0;
            int tccVal = 0;
            if ((prim >> 4) & 1ull)
            {
                const unsigned int texId = ps2xRdGetTextureForBatch(batch.kickSerial, tex0, texa); // FEAT-RD5
                if (texId != 0u)
                {
                    const uint32_t wms = static_cast<uint32_t>(clamp & 3ull);
                    const uint32_t wmt = static_cast<uint32_t>((clamp >> 2) & 3ull);
                    rdBindTex(texId, (wms != 0u ? 1u : 0u) | (wmt != 0u ? 2u : 0u) | ((((tex1 >> 5) & 1ull) != 0ull) ? 4u : 0u) | ((((tex1 >> 6) & 7ull) != 0ull) ? 8u : 0u)); // FEAT-RD15
                    texVal = 1;
                    tfxVal = static_cast<int>((tex0 >> 35) & 3ull);
                    tccVal = static_cast<int>((tex0 >> 34) & 1ull);
                }
            }
            if (g_rdGpu.texLoc >= 0)
                rdSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tfxLoc >= 0)
                rdSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tccLoc >= 0)
                rdSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);

            if (rd24Dump) // PROBE-RD24
            {
                const std::vector<RdVert> &pv = g_rd24Verts[db.slotIdx];
                const std::vector<uint16_t> &pi = g_rd24Idx[db.slotIdx];
                const float fbWf = fbSize[0], fbHf = fbSize[1];
                uint64_t xo24 = 0ull, sc24 = 0ull;
                ps2xRdLookupXyoff(batch.kickSerial, xo24);
                ps2xRdLookupScissor(batch.kickSerial, sc24);
                const float offx = static_cast<float>(xo24 & 0xFFFFull);
                const float offy = static_cast<float>((xo24 >> 32) & 0xFFFFull);
                float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f;
                uint32_t vmin = 0xFFFFFFFFu, vmax = 0u;
                const size_t endI = static_cast<size_t>(batch.first) + batch.count;
                if (endI <= pi.size() && batch.vpS[0] != 0.0f && batch.vpS[1] != 0.0f)
                {
                    for (size_t k = batch.first; k < endI; ++k)
                    {
                        const uint32_t vi = static_cast<uint32_t>(pi[k]) + batch.baseVertex;
                        if (vi >= pv.size())
                            continue;
                        vmin = std::min(vmin, vi);
                        vmax = std::max(vmax, vi);
                        const float *p = pv[vi].pos;
                        const float pw = p[3];
                        if (pw <= 0.0f)
                            continue;
                        const float px = (p[0] * batch.vpS[0] + pw * batch.vpO[0] - pw * offx * (1.0f / 16.0f)) / pw;
                        const float py = (p[1] * batch.vpS[1] + pw * batch.vpO[1] - pw * offy * (1.0f / 16.0f)) / pw;
                        bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                        by0 = std::min(by0, py); by1 = std::max(by1, py);
                        const float zw = p[2] / pw;
                        zmin = std::min(zmin, zw); zmax = std::max(zmax, zw);
                        wmin = std::min(wmin, pw);
                    }
                }
                if (bx1 >= bx0 && by1 >= by0)
                {
                    bx0 = std::max(0.0f, std::min(bx0, fbWf)); bx1 = std::max(0.0f, std::min(bx1, fbWf));
                    by0 = std::max(0.0f, std::min(by0, fbHf)); by1 = std::max(0.0f, std::min(by1, fbHf));
                    const float area = (bx1 - bx0) * (by1 - by0);
                    const bool big = area > 0.30f * fbWf * fbHf;
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "[%s] serial=%08x verts=%u tris=%u bbox=%d,%d-%d,%d prim=%llx test=%llx alpha=%llx frame=%llx zbuf=%llx tex0=%llx xyoff=%llx scis=%llx zmin=%.4f zmax=%.4f wmin=%.3f\n",
                                  big ? "rdbig" : "rdref", batch.kickSerial,
                                  (vmax >= vmin && vmin != 0xFFFFFFFFu) ? (vmax - vmin + 1u) : 0u, batch.count / 3u,
                                  static_cast<int>(bx0), static_cast<int>(by0), static_cast<int>(bx1), static_cast<int>(by1),
                                  (unsigned long long)prim, (unsigned long long)test, (unsigned long long)alpha,
                                  (unsigned long long)frame, (unsigned long long)zbuf, (unsigned long long)tex0,
                                  (unsigned long long)xo24, (unsigned long long)sc24,
                                  zmin, zmax, wmin);
                    if (big)
                    {
                        if (rd24BigN < 12)
                        {
                            ++rd24BigN;
                            ps2xSafeLog("%s", buf);
                        }
                    }
                    else
                        rd24Ref.emplace_back(buf);
                }
            }
            if (camDump) // PROBE-RD26
            {
                const std::vector<RdVert> &pv = g_rd24Verts[db.slotIdx];
                const std::vector<uint16_t> &pi = g_rd24Idx[db.slotIdx];
                uint64_t xoC = 0ull;
                ps2xRdLookupXyoff(batch.kickSerial, xoC);
                const float offx = static_cast<float>(xoC & 0xFFFFull);
                const float offy = static_cast<float>((xoC >> 32) & 0xFFFFull);
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                const size_t endI = static_cast<size_t>(batch.first) + batch.count;
                if (endI <= pi.size() && batch.vpS[0] != 0.0f && batch.vpS[1] != 0.0f)
                {
                    for (size_t k = batch.first; k < endI; ++k)
                    {
                        const uint32_t vi = static_cast<uint32_t>(pi[k]) + batch.baseVertex;
                        if (vi >= pv.size())
                            continue;
                        const float *pp = pv[vi].pos;
                        const float pw = pp[3];
                        if (pw <= 0.0f)
                            continue;
                        const float px = (pp[0] * batch.vpS[0] + pw * batch.vpO[0] - pw * offx * (1.0f / 16.0f)) / pw;
                        const float py = (pp[1] * batch.vpS[1] + pw * batch.vpO[1] - pw * offy * (1.0f / 16.0f)) / pw;
                        x0 = std::min(x0, px); x1 = std::max(x1, px);
                        y0 = std::min(y0, py); y1 = std::max(y1, py);
                    }
                }
                const bool hb = (x1 >= x0 && y1 >= y0);
                if (hb)
                {
                    x0 = std::max(0.0f, std::min(x0, fbSize[0])); x1 = std::max(0.0f, std::min(x1, fbSize[0]));
                    y0 = std::max(0.0f, std::min(y0, fbSize[1])); y1 = std::max(0.0f, std::min(y1, fbSize[1]));
                }
                g_camTl.add3d(batch.camHash, batch.kickSerial, batch.count / 3u, x0, y0, x1, y1, hb);
            }
            rlDrawVertexArrayElements(static_cast<int>(batch.first), static_cast<int>(batch.count), nullptr); // FEAT-RD14
        }
        if (rd24Dump) // PROBE-RD24
        {
            const size_t nRef = std::min<size_t>(3u, rd24Ref.size());
            for (size_t r = 0; r < nRef; ++r)
            {
                const size_t pick = r + static_cast<size_t>(std::rand()) % (rd24Ref.size() - r);
                std::swap(rd24Ref[r], rd24Ref[pick]);
                ps2xSafeLog("%s", rd24Ref[r].c_str());
            }
        }
        draw2dUntil(0u, true); // FEAT-RD8
        if (camDump) // PROBE-RD26
            g_camTl.finish();
        if (scisOn) // FEAT-RD22
        {
            rlDisableScissorTest();
            scisOn = false;
        }
        rlActiveTextureSlot(0); // FEAT-RD3
        rlDisableTexture();     // FEAT-RD3
        g_rdc.bound = 0u; // FEAT-RD15
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

    if (s_passLog) // FEAT-RD22
    {
        static std::chrono::steady_clock::time_point s_lastPass{};
        const auto nowP = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowP - s_lastPass).count() >= 5000)
        {
            s_lastPass = nowP;
            for (int pi = 0; pi < passN; ++pi)
            {
                const RdPassEnt &e = passEnts[pi];
                ps2xSafeLog("[rdpass] fbp=%u xyoff=%u,%u scis=%u,%u-%u,%u vpo=%.1f,%.1f n=%u\n",
                            e.fbp, e.xo, e.yo, e.sx0, e.sy0, e.sx1, e.sy1,
                            static_cast<double>(e.vox), static_cast<double>(e.voy), e.n);
            }
            ps2xSafeLog("[rdpass] scissorSkipped(total)=%llu\n", static_cast<unsigned long long>(s_scissorSkipped));
        }
    }

    ++g_rdGpu.frames;
    if (g_rdGpu.curAligned) ++g_rdGpu.alignedFrames; // FEAT-RD13
    if (g_rdGpu.curFallbackPrev) ++g_rdGpu.fallbackPrevFrames; // FEAT-RD13
    if (g_rdGpu.curSpanned2) ++g_rdGpu.spanned2Frames; // FEAT-RD13
    g_rdGpu.totalTris += g_rdGpu.frameTris; // FEAT-RD14
    g_rdGpu.totalVerts += g_rdGpu.frameVerts; // FEAT-RD14
    g_rdGpu.totalIndices += g_rdGpu.frameIndices; // FEAT-RD14
    g_rdGpu.totalSkippedDraws += static_cast<uint64_t>(g_rdGpu.lastSkippedDraws);
    g_rdGpu.totalMaskedDraws += static_cast<uint64_t>(g_rdGpu.lastMaskedDraws); // FEAT-RD2
    g_rdGpu.totalUnsupportedBlend += unsupBlend; // FEAT-RD2
    g_rdGpu.totalApproxAd += approxAd; // FEAT-RD7
    g_rdGpu.total2dDraws += draws2dThisFrame; // FEAT-RD8
    g_rdGpu.total2dZtested += ztested2dThisFrame; // FEAT-RD11
    g_rdGpu.total2dZclears += zclears2dThisFrame; // FEAT-RD20
    g_rdGpu.total2dTris += static_cast<uint64_t>(g_rdGpu.gpu2dVertCount / 3u); // FEAT-RD8
    g_rdGpu.totalRtSkipped += static_cast<uint64_t>(g_rdGpu.lastRtSkipped); // FEAT-RD8
    g_rdGpu.totalPointLineSkipped += static_cast<uint64_t>(g_rdGpu.lastPointLineSkipped); // FEAT-RD8
    g_rdc.totalCalls += g_rdc.calls; // FEAT-RD15
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rdGpu.lastLog).count() >= 5000)
    {
        g_rdGpu.lastLog = now;
        const double fr = g_rdGpu.frames ? static_cast<double>(g_rdGpu.frames) : 1.0;
        ps2xSafeLog("[rd] frames=%u tris/frame=%.0f verts/frame=%.0f indices/frame=%.0f skippedDraws/frame=%.1f maskedDraws/frame=%.1f unsupBlend/frame=%.1f approxAd/frame=%.1f\n",
                    g_rdGpu.frames,
                    static_cast<double>(g_rdGpu.totalTris) / fr,
                    static_cast<double>(g_rdGpu.totalVerts) / fr,
                    static_cast<double>(g_rdGpu.totalIndices) / fr,
                    static_cast<double>(g_rdGpu.totalSkippedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalMaskedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalUnsupportedBlend) / fr,
                    static_cast<double>(g_rdGpu.totalApproxAd) / fr); // FEAT-RD7 // FEAT-RD14
        ps2xSafeLog("[rd] glcalls/frame=%.0f\n", static_cast<double>(g_rdc.totalCalls) / fr); // FEAT-RD15
        ps2xSafeLog("[rd] date/frame=%.1f dateWithAbe/frame=%.1f\n", static_cast<double>(g_rdDateCount) / fr, static_cast<double>(g_rdDateAbeCount) / fr); // FEAT-RD25
        { // FEAT-RD23
            const uint64_t mx = ps2xRdMainXyoff();
            ps2xSafeLog("[rd] noState3d/frame=%.1f rtPass3d/frame=%.1f mainXyoff=%lld,%lld\n",
                        static_cast<double>(s_noState3dTotal) / fr, static_cast<double>(s_rtPass3dTotal) / fr,
                        mx == ~0ull ? -1ll : static_cast<long long>(mx & 0xFFFFull),
                        mx == ~0ull ? -1ll : static_cast<long long>((mx >> 32) & 0xFFFFull));
        }
        ps2xSafeLog("[rd2d] draws/frame=%.1f tris/frame=%.1f rtSkipped=%.1f pointLineSkipped=%.1f\n",
                    static_cast<double>(g_rdGpu.total2dDraws) / fr,
                    static_cast<double>(g_rdGpu.total2dTris) / fr,
                    static_cast<double>(g_rdGpu.totalRtSkipped) / fr,
                    static_cast<double>(g_rdGpu.totalPointLineSkipped) / fr); // FEAT-RD8
        ps2xSafeLog("[rd2d] ztested/frame=%.1f zclears2d/frame=%.2f vpZ=%f,%f\n",
                    static_cast<double>(g_rdGpu.total2dZtested) / fr,
                    static_cast<double>(g_rdGpu.total2dZclears) / fr, // FEAT-RD20
                    g_rdGpu.vpScaleZ,
                    g_rdGpu.vpOffsetZ); // FEAT-RD11
        ps2xSafeLog("[rd] aligned=%u fallbackPrev=%u spanned2=%u\n",
                    g_rdGpu.alignedFrames,
                    g_rdGpu.fallbackPrevFrames,
                    g_rdGpu.spanned2Frames); // FEAT-RD13
        ps2xSafeLog("[rd] gsState=%u parsedFallback=%u stateChangesInKick=%u mergedSpan=%u\n",
                    g_rdGpu.gsStateCount,
                    g_rdGpu.parsedFallbackCount,
                    g_rdStateChangesInKick.exchange(0u, std::memory_order_relaxed),
                    g_rdMergedSpan.exchange(0u, std::memory_order_relaxed)); // FEAT-RD17
        ps2xSafeLog("[rd] rtSkipped3d/frame=%.1f\n", static_cast<double>(g_rdGpu.rtSkipped3d) / fr); // FEAT-RD19
        ps2xSafeLog("[rd] fbFeedbackSkipped/frame=%.1f\n", static_cast<double>(s_fbFeedbackSkipped) / fr); // FEAT-RD28b
        s_fbFeedbackSkipped = 0u;
        g_rdGpu.rtSkipped3d = 0u;
        g_rdGpu.gsStateCount = 0u;
        g_rdGpu.parsedFallbackCount = 0u;
    }
    return true;
}
