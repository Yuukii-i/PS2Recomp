// FEAT-RD1: Direct geometry view from the VU1 scene buffer via OpenGL/rlgl.
#include <unordered_map>
#include <cstring>
#include "raylib.h"
#include "rd_gfx.h" // RDG1
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *lpPerformanceCount); // FIX-RD90
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *lpFrequency); // FIX-RD90
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
#include <tuple> // PROBE-RD29
#include <algorithm> // PROBE-RD26
#include <cmath> // FIX-P1
#include <mutex>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD1
extern unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa); // FEAT-RD5
#if defined(PS2X_HAS_SDL3)
namespace rdgfx_vk
{
bool active();
void setFog(float, float, float, float);
}
#endif

// PROBE-PACE2
static inline bool pace2Enabled()
{
    static const bool s_on = []() {
        const char *e = std::getenv("PS2X_PACE_LOG");
        return e && e[0] == '1';
    }();
    return s_on;
}
static inline int64_t pace2QpcNow()
{
#ifdef _WIN32
    long long t = 0;
    QueryPerformanceCounter(&t);
    return static_cast<int64_t>(t);
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
struct Pace2Scope
{
    int64_t *dst;
    int64_t t0;
    explicit Pace2Scope(int64_t &target, bool on) : dst(on ? &target : nullptr), t0(on ? pace2QpcNow() : 0) {}
    ~Pace2Scope()
    {
        if (dst)
            *dst += (pace2QpcNow() - t0);
    }
    Pace2Scope(const Pace2Scope &) = delete;
    Pace2Scope &operator=(const Pace2Scope &) = delete;
};
static struct Pace2RdState
{
    int64_t total = 0;
    int64_t b1 = 0;
    int64_t b2 = 0;
    int64_t b3 = 0;
    int64_t b4 = 0;
    int64_t b5 = 0;
} g_pace2Rd;
extern "C" void ps2xRdPace2Take(int64_t *out6)
{
    out6[0] = g_pace2Rd.total;
    out6[1] = g_pace2Rd.b1;
    out6[2] = g_pace2Rd.b2;
    out6[3] = g_pace2Rd.b3;
    out6[4] = g_pace2Rd.b4;
    out6[5] = g_pace2Rd.b5;
    g_pace2Rd = Pace2RdState{};
}
extern void ps2xRdBt2SetKind(int kind); // PROBE-BT2
extern bool ps2xRdIsDisplayFbp(uint32_t fbp); // FEAT-RD19
extern bool ps2xRdHasDisplayFbp(); // FEAT-RD19
extern bool ps2xRdLookupState(uint32_t serial, uint64_t &prim, uint64_t &alpha, uint64_t &test, uint64_t &frame,
                              uint64_t &zbuf, uint64_t &tex0, uint64_t &tex1, uint64_t &clamp, uint64_t &texa, uint64_t &fba); // FEAT-RD17
extern bool ps2xRdLookupXyoff(uint32_t serial, uint64_t &xyoff); // FEAT-RD21
extern bool ps2xRdLookupScissor(uint32_t serial, uint64_t &scis); // FEAT-RD22
extern bool ps2xRdIsMainPass(uint32_t fbp, uint64_t xyoffset); // FEAT-RD23
extern uint64_t ps2xRdMainXyoff(); // FEAT-RD23
extern bool ps2xRdIsFbFeedback(uint32_t serial); // FEAT-RD28b
extern uint32_t ps2xRdFbPageRanges(uint32_t *first, uint32_t *last); // FIX-RD79
extern bool ps2xRdGetClutRgba256(uint64_t tex0, uint64_t texa, uint32_t out[256]); // FIX-RD82B
static uint32_t s_fbFeedbackSkipped = 0u; // FEAT-RD28b
static uint32_t s_fbTexUsed = 0u; // FIX-RD79
static uint32_t s_fbTexCopies = 0u; // FIX-RD79
static uint32_t s_fbTexFrozen = 0u; // FIX-RD86
static uint32_t s_fbCopySkip = 0u; // FIX-RD87
static unsigned int s_prevTexId = 0u; // FIX-RD79
static int s_prevW = 0, s_prevH = 0; // FXC1
static bool s_prevFilled = false; // FXC1
static unsigned int s_lastSceneTexId = 0u; // FIX-RD85
static bool s_lastSceneFilled = false; // FIX-RD85
static uint32_t s_fbTexLastCopies = 0u; // FIX-RD85
static uint32_t s_fbTexBlendCount = 0u; // FIX-RD80
static float s_fbTexLastF = 1.0f; // FIX-RD80
static uint32_t s_fbTexPal = 0u; // FIX-RD82B
static uint32_t s_fbTexPalFail = 0u; // FIX-RD82B
static uint32_t s_fbTexPalFb = 0u; // FIX-RD84
static uint32_t s_fbTexAlphaFix = 0u; // FIX-RD82J
static uint32_t s_psm27WhyOff = 0u; // PROBE-RD82C
static uint32_t s_psm27WhyNoPrev = 0u; // PROBE-RD82C
static uint32_t s_psm27WhyNotFb = 0u; // PROBE-RD82C
static uint32_t s_psm27WhyDispFbp = 0u; // PROBE-RD82C
static uint32_t s_psm27WhyBadPsm = 0u; // PROBE-RD82C
static uint32_t s_psm27WhyNoRange = 0u; // PROBE-RD82C
static uint32_t s_psm27Ok = 0u; // PROBE-RD82C
static uint32_t s_nextFrameTrim = 0u; // FIX-RD88
static uint32_t s_nextFrameSkip = 0u; // FIX-RD88B
static std::atomic<uint32_t> s_rdDisplayBlendBits{0x3F800000u}; // FIX-RD80

void ps2xRdSetDisplayBlend(float f) // FIX-RD80
{
    uint32_t bits = 0x3F800000u;
    std::memcpy(&bits, &f, sizeof(bits));
    s_rdDisplayBlendBits.store(bits, std::memory_order_release);
}

static float rdGetDisplayBlend() // FIX-RD80
{
    const uint32_t bits = s_rdDisplayBlendBits.load(std::memory_order_acquire);
    float f = 1.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static bool rdFbTexOn() // FIX-RD79
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX");
        return e && e[0] == '1';
    }();
    return s_on;
}

static bool rdFbTexSubstOn() // FIX-RD80
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_SUBST");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static bool rdFbTexAnyPsmOn() // FIX-RD82
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_ANYPSM");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static bool rdFbTexPalOn() // FIX-RD82B
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_PAL");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static bool rdFbTexPalFbOn() // FIX-RD84B
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_PALFB");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static bool rdFbTexCtOn() // FIX-RD82H
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_CT");
        return e && e[0] == '1';
    }();
    return s_on;
}
static bool rdFbTexLastOn() // FIX-RD85 // FIX-RD86
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_FBTEX_LAST");
        return e && e[0] == '1'; // FIX-RD86: default 0 (LASTSCENE off)
    }();
    return s_on;
}
static uint32_t rdFbTexLast3dMin() // FIX-RD85B
{
    static const uint32_t s_min = []() -> uint32_t {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_LAST3D");
        if (e && e[0] != '\0')
        {
            const long v = std::strtol(e, nullptr, 10);
            if (v >= 0)
                return static_cast<uint32_t>(v);
        }
        return 12u;
    }();
    return s_min;
}
static unsigned int rdFbTexSelectId(uint64_t tex0) // FIX-RD85
{
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Full);
    const bool isPalPsm = (psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu);
    if (rdFbTexLastOn() && isPalPsm && s_lastSceneFilled && s_lastSceneTexId != 0u)
        return s_lastSceneTexId;
    return s_prevTexId;
}
bool ps2xRdGetPrevFrame(uint32_t &texId, int &w, int &h) // FXC1
{
    if (!s_prevFilled || s_prevTexId == 0u || s_prevW <= 0 || s_prevH <= 0)
        return false;
    texId = s_prevTexId;
    w = s_prevW;
    h = s_prevH;
    return true;
}
static bool rdSnapOn() // FIX-RD81
{
    static const bool s_on = []() {
        if (!rdFbTexOn())
            return false;
        const char *e = std::getenv("PS2X_RD_SNAP");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static bool rdCopySkipOn() // FIX-RD87
{
    static const bool s_on = []() {
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_COPYSKIP");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
struct RdCopyBufEntry // FIX-RD87
{
    uint32_t destFbp = 0xFFFFFFFFu;
    uint64_t lastSeenFrame = 0ull;
};
struct RdSnapReq // FIX-RD81
{
    uint32_t destFbp = 0xFFFFFFFFu;
    int w = 0;
    int h = 0;
};
struct RdSnapSlot // FIX-RD81
{
    uint32_t destFbp = 0xFFFFFFFFu;
    unsigned int texId = 0u;
    unsigned int fbo = 0u;
    int w = 0;
    int h = 0;
    bool valid = false;
    uint64_t lastUsedFrame = 0ull;
    int reqW = 0; // PROBE-V3A
    int reqH = 0; // PROBE-V3A
    int dstW = 0; // PROBE-V3A
    int dstH = 0; // PROBE-V3A
};
static constexpr size_t kRdSnapSlots = 4u; // FIX-RD81
static std::mutex s_snapMutex; // FIX-RD81
static std::vector<RdSnapReq> s_snapPending; // FIX-RD81
static std::atomic<bool> s_snapNeedReset{false}; // FIX-RD81
static RdSnapSlot s_snapSlots[kRdSnapSlots]; // FIX-RD81
static std::vector<RdCopyBufEntry> copyBufFbps; // FIX-RD87
static std::atomic<uint64_t> s_snapFrameSeq{0ull}; // FIX-RD81 // FIX-RD87
static uint32_t s_snapCount = 0u; // FIX-RD81
static uint32_t s_snapUsedCount = 0u; // FIX-RD81
static uint32_t s_snapSkippedNoGeom = 0u; // FIX-RD81b

void ps2xRdNoteSnapshot(uint32_t destFbp, int w, int h) // FIX-RD81
{
    if (!rdSnapOn() || destFbp == 0xFFFFFFFFu || w <= 0 || h <= 0)
        return;
    const uint64_t curFrame = s_snapFrameSeq.load(std::memory_order_relaxed); // FIX-RD87
    std::lock_guard<std::mutex> lock(s_snapMutex);
    bool foundCopy = false; // FIX-RD87
    for (RdCopyBufEntry &cb : copyBufFbps) // FIX-RD87
    {
        if (cb.destFbp == destFbp)
        {
            cb.lastSeenFrame = curFrame;
            foundCopy = true;
            break;
        }
    }
    if (!foundCopy) // FIX-RD87
    {
        if (copyBufFbps.size() >= 16u)
        {
            size_t oldest = 0u;
            for (size_t i = 1u; i < copyBufFbps.size(); ++i)
                if (copyBufFbps[i].lastSeenFrame < copyBufFbps[oldest].lastSeenFrame)
                    oldest = i;
            copyBufFbps[oldest] = {destFbp, curFrame};
        }
        else
        {
            copyBufFbps.push_back({destFbp, curFrame});
        }
    }
    for (RdSnapReq &req : s_snapPending)
    {
        if (req.destFbp == destFbp)
        {
            req.w = w;
            req.h = h;
            return;
        }
    }
    if (s_snapPending.size() < kRdSnapSlots)
        s_snapPending.push_back({destFbp, w, h});
}
extern std::atomic<uint32_t> g_rdStateChangesInKick; // FEAT-RD17
extern std::atomic<uint32_t> g_rdMergedSpan;        // FEAT-RD17
extern void ps2xRdTexTickFrame(); // FEAT-RD3

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2]; // FEAT-RD3
    float fog;   // FIX-E1b
};
static_assert(sizeof(RdVert) == 32, "RdVert size"); // FEAT-RD3 // FIX-E1b

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
    uint32_t fogcol; // FIX-E1c
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

struct RdSerialRange // FEAT-RD33
{
    uint32_t first = 0;
    uint32_t last = 0;
};

struct Rd2DFrame // FEAT-RD13
{
    std::vector<RdVert> verts;
    std::vector<Rd2DBatch> batches;
    std::vector<RdSerialRange> mainRanges; // FEAT-RD33
    uint32_t mainSerialCount = 0;          // FEAT-RD33
    uint32_t rtSkipped = 0;
    uint32_t pointLineSkipped = 0;
    uint32_t gMin = 0;
    uint32_t gMax = 0;
    bool valid = false;
};

static bool rdIdx32On(); // FIX-RD90

namespace
{
constexpr size_t kRdSlotCount = 6u; // FEAT-RD34
std::mutex g_rdMutex;
Rd3DFrame g_rd3dRing[kRdSlotCount]; // FEAT-RD13 // FEAT-RD34
bool g_rdPinnedSlots[kRdSlotCount] = {}; // FEAT-RD34
uint32_t g_rd3dEvictedMaxSerial = 0u; // FEAT-RD34
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
    RdGpuSlot slots[kRdSlotCount]; // FEAT-RD14 // FEAT-RD34
    size_t activeOrder[kRdSlotCount] = {0, 1, 2, 3, 4, 5}; // FEAT-RD14 // FEAT-RD34
    std::vector<RdVert> pendingVerts[kRdSlotCount]; // FEAT-RD14 // FEAT-RD34
    std::vector<uint32_t> pendingIndices[kRdSlotCount]; // FEAT-RD14 // FEAT-RD34
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
    bool shaderReady = false; // RDG1
    float flip[3] = {1.0f, -1.0f, 1.0f}; // FEAT-RD2
    std::vector<RdDrawBatch> drawBatches; // FEAT-RD14
    std::vector<RdVert> local2dScratch; // FEAT-RD8
    std::vector<Rd2DBatch> local2dBatches; // FEAT-RD8
    size_t ring3dCount = 0; // FEAT-RD13
    uint64_t last3dSeq = 0; // FEAT-RD13
    Rd2DFrame frames2d[2]; // FEAT-RD13
    uint64_t last2dSeq = 0; // FEAT-RD13
    uint32_t activeGMin = 0; // FEAT-RD13
    bool hasCompleteComposition = false; // FEAT-RD33
    uint32_t lastMainSerialCount = 0;    // FEAT-RD33
    uint32_t composedFrames = 0;         // FEAT-RD33
    uint32_t redrawPrevFrames = 0;       // FEAT-RD33
    uint32_t noMainFrames = 0;           // FEAT-RD33b
    uint32_t usedPrev2dFrames = 0;       // FEAT-RD33
    uint64_t totalMainSerials = 0;       // FEAT-RD33
    uint32_t prevDrawn3dCount = 0;       // FEAT-RD34
    int lastEmptyReason = 0;             // FEAT-RD34: 1=Sempty, 2=missing, 3=overwritten, 4=skipped
    uint32_t drawsNew = 0;               // FEAT-RD34
    uint32_t drawsPrev = 0;              // FEAT-RD34
    uint32_t drawsEmpty = 0;             // FEAT-RD34
    uint32_t drawsEmptySempty = 0;       // FEAT-RD34
    uint32_t drawsEmptyMissing = 0;      // FEAT-RD34
    uint32_t drawsEmptyOverwritten = 0;  // FEAT-RD34
    uint32_t drawsEmptySkipped = 0;      // FEAT-RD34
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
    double totalUploadMs = 0.0; // FIX-RD90
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
    {
        ++g_rdDateAbeCount;
        return blendState; // FEAT-RD31: DATE with ABE uses the batch's own blend
    }
    return 1 + (((test >> 15) & 1ull) ? 7 : 6) * 128;
}

// FEAT-RD15: GL state cache (redundant call elimination)
struct RdGlCache
{
    static constexpr unsigned int kFastTexCap = 16384u; // PERF-EX5
    std::vector<uint32_t> texParamsVec;                 // PERF-EX5: direct lookup for normal GL texIds
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
        {
            std::fill(texParamsVec.begin(), texParamsVec.end(), 0u); // PERF-EX5
            texParams.clear();
        }
    }
    void invalidateTex(unsigned int texId) // FIX-RD85B
    {
        if (texId == 0u)
            return;
        if (bound == texId)
            bound = 0u;
        if (texId < kFastTexCap)
        {
            if (texId < texParamsVec.size())
                texParamsVec[texId] = 0u;
        }
        else
        {
            texParams.erase(texId);
        }
    }
} g_rdc; // FEAT-RD15

static inline void rdSetFog(float r, float g, float b, float enable) // FIX-E1c
{
#if defined(PS2X_HAS_SDL3)
    if (rdgfx_vk::active())
        rdgfx_vk::setFog(r, g, b, enable);
#else
    (void)r; (void)g; (void)b; (void)enable;
#endif
}

static inline bool rdFogEnvEnabled() // FIX-E1c
{
    static const bool s_fogEnabled = []{
        const char *e = std::getenv("PS2X_RD_FOG");
        return !(e && e[0] == '0');
    }();
    return s_fogEnabled;
}

static inline bool rdFogZeroEnvEnabled() // FIX-E1d
{
    static const bool s_fogZero = []{
        const char *e = std::getenv("PS2X_RD_FOG_ZERO");
        return e && e[0] == '1';
    }();
    return s_fogZero;
}

void rdSetUniformInt(rdgfx::Uniform u, int v) // FEAT-RD15 // RDG1
{
    const int loc = static_cast<int>(u);
    uint32_t w = 0u;
    std::memcpy(&w, &v, 4);
    if (loc >= 0 && loc < 64)
    {
        if (g_rdc.uniValid[loc] && g_rdc.uni[loc][0] == w && g_rdc.uni[loc][1] == 0u)
            return;
        g_rdc.uni[loc][0] = w; g_rdc.uni[loc][1] = 0u; g_rdc.uniValid[loc] = true;
    }
    ++g_rdc.calls;
    rdgfx::setUniformInt(u, v);
}
void rdSetUniformFloat(rdgfx::Uniform u, float v) // FEAT-RD15 // RDG1
{
    const int loc = static_cast<int>(u);
    uint32_t w = 0u;
    std::memcpy(&w, &v, 4);
    if (loc >= 0 && loc < 64)
    {
        if (g_rdc.uniValid[loc] && g_rdc.uni[loc][0] == w && g_rdc.uni[loc][1] == 0u)
            return;
        g_rdc.uni[loc][0] = w; g_rdc.uni[loc][1] = 0u; g_rdc.uniValid[loc] = true;
    }
    ++g_rdc.calls;
    rdgfx::setUniformFloat(u, v);
}
void rdSetUniformVec2(rdgfx::Uniform u, const float *v) // FEAT-RD15 // RDG1
{
    const int loc = static_cast<int>(u);
    uint32_t w[2] = {0u, 0u};
    std::memcpy(w, v, 8);
    if (loc >= 0 && loc < 64)
    {
        if (g_rdc.uniValid[loc] && g_rdc.uni[loc][0] == w[0] && g_rdc.uni[loc][1] == w[1])
            return;
        g_rdc.uni[loc][0] = w[0]; g_rdc.uni[loc][1] = w[1]; g_rdc.uniValid[loc] = true;
    }
    ++g_rdc.calls;
    rdgfx::setUniformVec2(u, v);
}
void rdDepthTest(bool on) // FEAT-RD15
{
    if (g_rdc.depthTest == (on ? 1 : 0)) return;
    g_rdc.depthTest = on ? 1 : 0; ++g_rdc.calls;
    rdgfx::setDepthTest(on); // RDG1
}
void rdDepthMask(bool on) // FEAT-RD15
{
    if (g_rdc.depthMask == (on ? 1 : 0)) return;
    g_rdc.depthMask = on ? 1 : 0; ++g_rdc.calls;
    rdgfx::setDepthMask(on); // RDG1
}
void rdBindTex(unsigned int texId, uint32_t params) // FEAT-RD15
{
    uint32_t *cachedPtr = nullptr; // PERF-EX5
    if (texId < RdGlCache::kFastTexCap)
    {
        if (texId >= g_rdc.texParamsVec.size())
            g_rdc.texParamsVec.resize(texId + 1u, 0u);
        cachedPtr = &g_rdc.texParamsVec[texId];
    }
    else
    {
        cachedPtr = &g_rdc.texParams[texId];
    }
    uint32_t &cached = *cachedPtr;
    const uint32_t want = params | 0x80000000u;
    if (cached != want)
    {
        const uint32_t old = cached;
        const bool fresh = !(old & 0x80000000u);
        const rdgfx::TextureWrap wrapS = (params & 1u) ? rdgfx::TextureWrap::Clamp : rdgfx::TextureWrap::Repeat; // RDG1
        const rdgfx::TextureWrap wrapT = (params & 2u) ? rdgfx::TextureWrap::Clamp : rdgfx::TextureWrap::Repeat; // RDG1
        const rdgfx::TextureFilter mag = (params & 4u) ? rdgfx::TextureFilter::Linear : rdgfx::TextureFilter::Nearest; // RDG1
        const rdgfx::TextureFilter mn = (params & 8u) ? rdgfx::TextureFilter::Linear : rdgfx::TextureFilter::Nearest; // RDG1
        if (fresh || ((old ^ want) & 1u)) { rdgfx::setTextureWrap(texId, false, wrapS); ++g_rdc.calls; } // RDG1
        if (fresh || ((old ^ want) & 2u)) { rdgfx::setTextureWrap(texId, true, wrapT); ++g_rdc.calls; } // RDG1
        if (fresh || ((old ^ want) & 4u)) { rdgfx::setTextureFilter(texId, false, mag); ++g_rdc.calls; } // RDG1
        if (fresh || ((old ^ want) & 8u)) { rdgfx::setTextureFilter(texId, true, mn); ++g_rdc.calls; } // RDG1
        cached = want;
    }
    if (g_rdc.bound != texId)
    {
        rdgfx::bindTexture(0u, texId); // RDG1
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
        rdgfx::setColorMask(ch[0], ch[1], ch[2], ch[3]); // RDG1
        ++g_rdc.calls; // FEAT-RD15
        g_rdMaskCur = packed;
    }
}

// FEAT-RD30: AFAIL (TEST bits 12-13) write rules for fragments failing the alpha test
uint64_t g_rdAfail2Pass = 0, g_rdAfailNever = 0; // FEAT-RD30
void rdSetMaskPacked(int packed)                 // FEAT-RD30
{
    if (packed == g_rdMaskCur)
        return;
    rdgfx::setColorMask((packed & 1) != 0, (packed & 2) != 0, (packed & 4) != 0, (packed & 8) != 0); // RDG1
    ++g_rdc.calls;
    g_rdMaskCur = packed;
}
void rdApplyAfailWrites(int afail, bool zmskOff, int baseMask) // FEAT-RD30
{
    if (afail == 1) // FB_ONLY
    {
        rdSetMaskPacked(baseMask);
        rdDepthMask(false);
    }
    else if (afail == 2) // ZB_ONLY
    {
        rdSetMaskPacked(0);
        rdDepthMask(zmskOff);
    }
    else // RGB_ONLY
    {
        rdSetMaskPacked(baseMask & 7);
        rdDepthMask(false);
    }
}

void applyRdBlend(int blendState, unsigned int vao) // FEAT-RD8
{
    if (blendState == 0)
        rdgfx::setBlend(false); // RDG1
    else
    {
        const int code = blendState - 1;
        const int sk = code / 128, dk = (code / 16) & 7, ek = code & 3; // FEAT-RD9
        auto glf = [](int k) { return k == 0 ? rdgfx::BlendFactor::Zero : k == 1 ? rdgfx::BlendFactor::One : k == 2 ? rdgfx::BlendFactor::SrcAlpha : k == 3 ? rdgfx::BlendFactor::OneMinusSrcAlpha : k == 4 ? rdgfx::BlendFactor::DstAlpha : rdgfx::BlendFactor::OneMinusDstAlpha; }; // RDG1
        if (sk >= 6) // FEAT-RD25: DATE approximation
            rdgfx::setBlend(true, sk == 6 ? rdgfx::BlendFactor::OneMinusDstAlpha : rdgfx::BlendFactor::DstAlpha, sk == 6 ? rdgfx::BlendFactor::DstAlpha : rdgfx::BlendFactor::OneMinusDstAlpha, rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero, rdgfx::BlendEq::Add, rdgfx::BlendEq::Add); // FEAT-RD31 // RDG1
        else
            rdgfx::setBlend(true, glf(sk), glf(dk), rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero, ek == 0 ? rdgfx::BlendEq::Add : ek == 1 ? rdgfx::BlendEq::Subtract : rdgfx::BlendEq::ReverseSubtract, rdgfx::BlendEq::Add); // FEAT-RD31: GS writes source alpha // RDG1
    }
    rdgfx::enableShader(); // RDG1
    rdgfx::bindVertexArray(vao); // RDG1
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
            rdgfx::freeVertexBuffer(vbo); // RDG1
        const size_t newCap = std::max<size_t>(n, capVerts ? capVerts * 2 : 4096);
        vbo = rdgfx::createVertexBuffer(nullptr, static_cast<int>(newCap * sizeof(RdVert)), true); // RDG1
        capVerts = newCap;

        rdgfx::bindVertexArray(vao); // RDG1
        rdgfx::bindVertexBuffer(vbo); // RDG1
        rdgfx::setVertexAttribute(0, 4, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 0); // RDG1
        rdgfx::enableVertexAttribute(0); // RDG1
        rdgfx::setVertexAttribute(1, 4, rdgfx::VertexAttrFormat::UnsignedByte, true, sizeof(RdVert), 16); // RDG1
        rdgfx::enableVertexAttribute(1); // RDG1
        rdgfx::setVertexAttribute(2, 2, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 20); // RDG1
        rdgfx::enableVertexAttribute(2); // RDG1
        rdgfx::setVertexAttribute(3, 1, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 28); // FIX-E1b
        rdgfx::enableVertexAttribute(3); // FIX-E1b
        rdgfx::bindVertexBuffer(0u); // RDG1
        rdgfx::bindVertexArray(0u); // RDG1
    }
    rdgfx::updateVertexBuffer(vbo, verts.data(), static_cast<int>(n * sizeof(RdVert)), 0); // RDG1
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

#ifdef _WIN32 // FIX-RD90
    static const long long s_qpcFreq = []() {
        long long f = 0;
        QueryPerformanceFrequency(&f);
        return f > 0 ? f : 1;
    }();
    long long t0 = 0;
    QueryPerformanceCounter(&t0);
#endif

    const bool idx32 = rdIdx32On(); // FIX-RD90
    size_t nIdx = 0;
    const void *idxPtr = nullptr;
    if (idx32) // FIX-RD90
    {
        for (RdBatch &b : batches)
            b.baseVertex = 0u;
        nIdx = ni;
        idxPtr = indices.data();
    }
    else
    {
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
        nIdx = idx16.size();
        idxPtr = idx16.data();
    }

    const size_t idxElemSize = idx32 ? sizeof(uint32_t) : sizeof(uint16_t); // FIX-RD90
    bool rebindVao = false;
    if (nv > slot.vboCapVerts)
    {
        if (slot.vbo != 0)
            rdgfx::freeVertexBuffer(slot.vbo); // RDG1
        const size_t newCap = std::max<size_t>(nv, slot.vboCapVerts ? slot.vboCapVerts * 2 : 4096);
        slot.vbo = rdgfx::createVertexBuffer(nullptr, static_cast<int>(newCap * sizeof(RdVert)), true); // RDG1
        slot.vboCapVerts = newCap;
        rebindVao = true;
    }
    if (nIdx > slot.eboCapIndices)
    {
        if (slot.ebo != 0)
            rdgfx::freeIndexBuffer(slot.ebo); // RDG1
        const size_t newCap = std::max<size_t>(nIdx, slot.eboCapIndices ? slot.eboCapIndices * 2 : 8192);
        slot.ebo = rdgfx::createIndexBuffer(nullptr, static_cast<int>(newCap * idxElemSize), true); // FIX-RD90 // RDG1
        slot.eboCapIndices = newCap;
        rebindVao = true;
    }
    if (rebindVao)
    {
        rdgfx::bindVertexArray(slot.vao); // RDG1
        rdgfx::bindVertexBuffer(slot.vbo); // RDG1
        rdgfx::setVertexAttribute(0, 4, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 0); // RDG1
        rdgfx::enableVertexAttribute(0); // RDG1
        rdgfx::setVertexAttribute(1, 4, rdgfx::VertexAttrFormat::UnsignedByte, true, sizeof(RdVert), 16); // RDG1
        rdgfx::enableVertexAttribute(1); // RDG1
        rdgfx::setVertexAttribute(2, 2, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 20); // RDG1
        rdgfx::enableVertexAttribute(2); // RDG1
        rdgfx::setVertexAttribute(3, 1, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), 28); // FIX-E1b
        rdgfx::enableVertexAttribute(3); // FIX-E1b
        rdgfx::bindIndexBuffer(slot.ebo); // RDG1
        rdgfx::bindVertexArray(0u); // RDG1
        rdgfx::bindVertexBuffer(0u); // RDG1
        rdgfx::bindIndexBuffer(0u); // RDG1
        slot.curBaseVertex = 0u; // FEAT-RD14b: attributes re-pointed at offset 0
    }
    rdgfx::updateVertexBuffer(slot.vbo, verts.data(), static_cast<int>(nv * sizeof(RdVert)), 0); // RDG1
    if (nIdx > 0)
        rdgfx::updateIndexBuffer(slot.ebo, idxPtr, static_cast<int>(nIdx * idxElemSize), 0); // FIX-RD90 // RDG1
    slot.vertCount = nv;
    slot.indexCount = nIdx;

#ifdef _WIN32 // FIX-RD90
    long long t1 = 0;
    QueryPerformanceCounter(&t1);
    g_rdGpu.totalUploadMs += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(s_qpcFreq);
#endif
}
} // namespace

static std::atomic<int8_t> s_rdViewOnCache{-1}; // PERF-PF1
bool ps2xRdViewOn() // FEAT-RD1
{
    int8_t v = s_rdViewOnCache.load(std::memory_order_relaxed);
    if (v < 0)
    {
        extern void ps2xLoadConfig(); // FIX-CFG1
        ps2xLoadConfig(); // FIX-CFG1
        const char *e = std::getenv("PS2X_RD_VIEW");
        v = (e && e[0] == '1') ? 1 : 0;
        s_rdViewOnCache.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

extern bool ps2xRdForceClassic(); // FEAT-RD49

bool ps2xRdActive() // FEAT-RD49
{
    return ps2xRdViewOn() && !ps2xRdForceClassic();
}

static std::atomic<bool> g_rdNeedGpuReset{false}; // FEAT-RD49

void ps2xRdResetState() // FEAT-RD49
{
    std::lock_guard<std::mutex> lock(g_rdMutex);
    for (size_t i = 0; i < kRdSlotCount; ++i)
    {
        g_rdPinnedSlots[i] = false;
        g_rd3dRing[i].seq = 0u;
        g_rd3dRing[i].sMin = 0u;
        g_rd3dRing[i].sMax = 0u;
        g_rd3dRing[i].verts.clear();
        g_rd3dRing[i].indices.clear();
        g_rd3dRing[i].batches.clear();
    }
    g_rd3dEvictedMaxSerial = 0u;
    g_rd3dWriteIdx = 0u;
    g_rd3dCount = 0u;
    g_rdEverPublished = false;
    g_rd2dFrames[0] = Rd2DFrame{};
    g_rd2dFrames[1] = Rd2DFrame{};
    g_rdNeedGpuReset.store(true, std::memory_order_release);
    ps2xRdSetDisplayBlend(1.0f); // FIX-RD80
    { // FIX-RD81
        std::lock_guard<std::mutex> snapLock(s_snapMutex);
        s_snapPending.clear();
        copyBufFbps.clear(); // FIX-RD87
        s_snapNeedReset.store(true, std::memory_order_release);
    }
}

float g_rdAspectK = 1.0f; // FEAT-RD16: published each frame by present loop when PS2X_ASPECT=fill & takeover

bool ps2xRdTakeoverOn() // FEAT-RD12
{
    static const bool v = []() {
        extern void ps2xLoadConfig(); // FIX-CFG1
        ps2xLoadConfig(); // FIX-CFG1
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
    return v && ps2xRdActive(); // FEAT-RD49: takeover implies active
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
    size_t chosen = kRdSlotCount; // FEAT-RD34: never overwrite a pinned slot
    for (size_t step = 0; step < kRdSlotCount; ++step)
    {
        const size_t cand = (g_rd3dWriteIdx + step) % kRdSlotCount;
        if (!g_rdPinnedSlots[cand] && g_rd3dRing[cand].seq == 0u)
        {
            chosen = cand;
            break;
        }
    }
    if (chosen == kRdSlotCount)
    {
        uint64_t bestSeq = ~0ull;
        for (size_t cand = 0; cand < kRdSlotCount; ++cand)
        {
            if (!g_rdPinnedSlots[cand] && g_rd3dRing[cand].seq < bestSeq)
            {
                bestSeq = g_rd3dRing[cand].seq;
                chosen = cand;
            }
        }
    }
    if (chosen == kRdSlotCount) // FEAT-RD34: all slots pinned/busy -> drop incoming 3D frame
        return;
    Rd3DFrame &slot = g_rd3dRing[chosen]; // FEAT-RD13 // FEAT-RD34
    if (slot.seq != 0u && slot.sMax > g_rd3dEvictedMaxSerial)
        g_rd3dEvictedMaxSerial = slot.sMax;
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
    g_rd3dWriteIdx = (chosen + 1u) % kRdSlotCount;
    if (g_rd3dCount < kRdSlotCount)
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

static bool rdSerialInRanges(uint32_t s, const std::vector<RdSerialRange> &ranges) // FEAT-RD33
{
    if (s == 0u || ranges.empty())
        return false;
    size_t lo = 0u, hi = ranges.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2u;
        if (ranges[mid].last < s)
            lo = mid + 1u;
        else if (ranges[mid].first > s)
            hi = mid;
        else
            return true;
    }
    return false;
}

// FIX-V3R: env-gated tail tolerance for 2D/3D cover chooser
static const uint32_t s_coverTol = []{
    extern void ps2xLoadConfig();
    ps2xLoadConfig();
    const char *e = std::getenv("PS2X_RD_COVER_TOL");
    return (e && *e) ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
}(); // FIX-V3R

static bool rdRingCoversRanges(uint32_t ringMin, uint32_t ringMax, const std::vector<RdSerialRange> &ranges) // FEAT-RD33 // FIX-V3R
{
    if (ranges.empty())
        return true;
    if (ringMin == 0u)
        return false;
    if (s_coverTol == 0u)
        return ringMin <= ranges.front().first && ranges.back().last <= ringMax;
    for (const RdSerialRange &r : ranges) // FIX-V3R
    {
        if (r.first < ringMin)
            return false;
        if (static_cast<uint64_t>(r.last) > static_cast<uint64_t>(ringMax) + s_coverTol)
            return false;
    }
    return true;
}

void ps2xRdPublish2D(const RdVert *v, size_t n, const Rd2DBatch *b, size_t nb, uint32_t rtSkipped, uint32_t pointLineSkipped, uint32_t gMin, uint32_t gMax, const uint32_t *mainRangePairs, size_t numMainRanges) // FEAT-RD8 // FEAT-RD13 // FEAT-RD33
{
    static thread_local std::vector<RdVert> t_staging2d;
    static thread_local std::vector<Rd2DBatch> t_batchStaging2d;
    static thread_local std::vector<RdSerialRange> t_rangeStaging; // FEAT-RD33
    if (v && n > 0)
        t_staging2d.assign(v, v + n);
    else
        t_staging2d.clear();
    if (b && nb > 0)
        t_batchStaging2d.assign(b, b + nb);
    else
        t_batchStaging2d.clear();
    t_rangeStaging.clear();
    uint32_t serialCnt = 0u;
    if (mainRangePairs && numMainRanges > 0)
    {
        t_rangeStaging.reserve(numMainRanges);
        for (size_t i = 0; i < numMainRanges; ++i)
        {
            const uint32_t a = mainRangePairs[i * 2u];
            const uint32_t bEnd = mainRangePairs[i * 2u + 1u];
            t_rangeStaging.push_back(RdSerialRange{a, bEnd});
            if (bEnd >= a)
                serialCnt += (bEnd - a + 1u);
        }
    }

    std::lock_guard<std::mutex> lock(g_rdMutex);
    if (serialCnt == 0u) ++g_rdGpu.noMainFrames; // FEAT-RD33b
    std::swap(g_rd2dFrames[1], g_rd2dFrames[0]); // FEAT-RD13: previous <- newest
    g_rd2dFrames[0].verts.swap(t_staging2d);
    g_rd2dFrames[0].batches.swap(t_batchStaging2d);
    g_rd2dFrames[0].mainRanges.swap(t_rangeStaging); // FEAT-RD33
    g_rd2dFrames[0].mainSerialCount = serialCnt;     // FEAT-RD33
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
static std::vector<RdVert> g_rd24Verts[kRdSlotCount]; // PROBE-RD24 // FEAT-RD34
static std::vector<uint16_t> g_rd24Idx[kRdSlotCount]; // PROBE-RD24 // FEAT-RD34
static const bool g_rdBlendOn = []{ const char *e = std::getenv("PS2X_RD_BLENDLOG"); return e && e[0] == '1'; }(); // PROBE-RD29
static const bool g_rdReasonOn = []{ const char *e = std::getenv("PS2X_RD_DRAWREASON"); return e && e[0] == '1'; }(); // PROBE-RD59
static const bool g_rdFogLog = []{ const char *e = std::getenv("PS2X_RD_FOGLOG"); return e && e[0] == '1'; }(); // PROBE-E1c
static bool rdIdx32On() // FIX-RD90
{
    static const bool s_on = []() {
        if (g_rd24On || g_rdCamOn || g_rdBlendOn || g_rdReasonOn || g_rdFogLog) // PROBE-E1c
            return false;
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_IDX32");
        return !(e && e[0] == '0');
    }();
    return s_on;
}
static inline void rdDrawElems(uint32_t first, uint32_t count) // FIX-RD90
{
    rdgfx::drawIndexed(rdgfx::PrimitiveTopology::Triangles, first, count, rdIdx32On()); // RDG1
}
static const bool g_rd2dReasonOn = []{ const char *e = std::getenv("PS2X_RD_2DREASON"); return e && e[0] == '1'; }(); // PROBE-RD62
static const bool g_rd2dReasonBig = []{ const char *e = std::getenv("PS2X_RD_2DREASON_BIG"); return e && e[0] == '1'; }(); // PROBE-RD62B
static const int g_rd2dMax = []{ const char *e = std::getenv("PS2X_RD_2DMAX"); return (e && *e) ? std::atoi(e) : 0; }(); // PROBE-RD83
static const int g_rd2dOnly = []{ const char *e = std::getenv("PS2X_RD_2DONLY"); return (e && *e) ? std::atoi(e) : -1; }(); // PROBE-RD83
static inline bool g_rd2dLateOn() // FIX-CFG2
{
    static const bool s_on = [] { extern void ps2xLoadConfig(); ps2xLoadConfig(); const char *e = std::getenv("PS2X_RD_2DLATE"); return e && e[0] == '1'; }(); // FIX-RD63 // FIX-CFG2
    return s_on;
}
static const bool g_rdDimLateOn = []{ const char *e = std::getenv("PS2X_RD_DIMLATE"); return !(e && e[0] == '0'); }(); // FIX-RD66
static const bool g_rdNoZTest = []{ const char *e = std::getenv("PS2X_RD_NOZTEST"); return e && e[0] == '1'; }(); // PROBE-RD59
static const float g_rdZBias = []{ const char *e = std::getenv("PS2X_RD_ZBIAS"); return (e && *e) ? static_cast<float>(std::atof(e)) : 0.0f; }(); // PROBE-RD59
// PROBE-E3a: ABE debug filter configuration and counters
struct AbeFilterConfig
{
    bool active = false;
    int inv = 0;
    std::string listStr;
    std::vector<uint64_t> values;

    bool contains(uint64_t v) const
    {
        for (uint64_t x : values)
            if (x == v)
                return true;
        return false;
    }
};

static const AbeFilterConfig &getAbeFilter() // PROBE-E3a
{
    static const AbeFilterConfig cfg = []() {
        AbeFilterConfig c;
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_ABEFILTER");
        if (!e || !*e)
            return c;
        c.listStr = e;
        const char *invE = std::getenv("PS2X_RD_ABEFILTER_INV");
        c.inv = (invE && invE[0] == '1') ? 1 : 0;
        const char *p = e;
        while (*p)
        {
            while (*p == ' ' || *p == '\t' || *p == ',')
                ++p;
            if (!*p)
                break;
            char *endp = nullptr;
            uint64_t val = std::strtoull(p, &endp, 16);
            if (endp && endp != p)
            {
                c.values.push_back(val);
                p = endp;
            }
            else
            {
                ++p;
            }
        }
        if (!c.values.empty())
        {
            c.active = true;
            ps2xSafeLog("[abefilter] list=%s inv=%d\n", c.listStr.c_str(), c.inv);
        }
        return c;
    }();
    return cfg;
}
static const AbeFilterConfig &s_abeFilter = getAbeFilter(); // PROBE-E3a
static uint64_t s_abeDrawn3dSec = 0; // PROBE-E3a
static uint64_t s_abeDrawn2dSec = 0; // PROBE-E3a
static uint64_t s_abeSkippedSec = 0; // PROBE-E3a
// PROBE-E3d: TEX debug filter configuration and counters
struct TexFilterConfig
{
    bool active = false;
    int inv = 0;
    std::string listStr;
    std::vector<uint64_t> values;

    bool contains(uint64_t v) const
    {
        for (uint64_t x : values)
            if (x == v)
                return true;
        return false;
    }
};

static const TexFilterConfig &getTexFilter() // PROBE-E3d
{
    static const TexFilterConfig cfg = []() {
        TexFilterConfig c;
        extern void ps2xLoadConfig();
        ps2xLoadConfig();
        const char *e = std::getenv("PS2X_RD_TEXFILTER");
        if (!e || !*e)
            return c;
        c.listStr = e;
        const char *invE = std::getenv("PS2X_RD_TEXFILTER_INV");
        c.inv = (invE && invE[0] == '1') ? 1 : 0;
        const char *p = e;
        while (*p)
        {
            while (*p == ' ' || *p == '\t' || *p == ',')
                ++p;
            if (!*p)
                break;
            char *endp = nullptr;
            uint64_t val = std::strtoull(p, &endp, 16);
            if (endp && endp != p)
            {
                c.values.push_back(val);
                p = endp;
            }
            else
            {
                ++p;
            }
        }
        if (!c.values.empty())
        {
            c.active = true;
            ps2xSafeLog("[texfilter] n=%u inv=%d\n", static_cast<unsigned>(c.values.size()), c.inv);
        }
        return c;
    }();
    return cfg;
}
static const TexFilterConfig &s_texFilter = getTexFilter(); // PROBE-E3d
static uint64_t s_texDrawn3dSec = 0; // PROBE-E3d
static uint64_t s_texDrawn2dSec = 0; // PROBE-E3d
static uint64_t s_texSkippedSec = 0; // PROBE-E3d
// RDG1: g_rdZBiasLoc moved to rdgfx::Uniform::ZBias
struct RdBlendCensus // PROBE-RD29
{
    typedef std::tuple<int, uint64_t, uint64_t, uint32_t, uint64_t, uint64_t, int> Key; // path, prim, alpha, fbmsk, test, tex0, reason
    struct Val { uint32_t n = 0, tris = 0; float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f; int blend = 0; };
    std::map<Key, Val> m;
    static const char *reasonName(int r)
    {
        static const char *n[] = {"none", "feedback", "rtPass", "noState", "atstNever", "ztstNever", "fbmskAll", "scissor"};
        return (r >= 0 && r < 8) ? n[r] : "?";
    }
    void add(int path, uint64_t prim, uint64_t alpha, uint64_t frame, uint64_t test, uint64_t tex0, int reason, uint32_t tris,
             bool hasBox, float x0, float y0, float x1, float y1, int blend)
    {
        if (reason == 0 && static_cast<uint32_t>(frame >> 32) == 0xFFFFFFFFu)
            reason = 6;
        Val &v = m[Key(path, prim & 0x7FFull, alpha, static_cast<uint32_t>(frame >> 32), test, tex0 & 0x3FFFFFFFFull, reason)];
        ++v.n; v.tris += tris; v.blend = blend;
        if (hasBox) { v.x0 = std::min(v.x0, x0); v.y0 = std::min(v.y0, y0); v.x1 = std::max(v.x1, x1); v.y1 = std::max(v.y1, y1); }
    }
    void dump()
    {
        std::vector<std::pair<Key, Val>> v(m.begin(), m.end());
        auto isStd = [](const Key &k) { const uint64_t a = std::get<2>(k); return a == 0x44ull || a == 0x8000000044ull; };
        std::sort(v.begin(), v.end(), [&](const auto &a, const auto &b) {
            const bool sa = isStd(a.first), sb = isStd(b.first);
            if (sa != sb) return !sa;
            return a.second.n > b.second.n;
        });
        ps2xSafeLog("[rdblend] census distinct=%u\n", static_cast<unsigned>(v.size()));
        for (size_t i = 0; i < v.size() && i < 50u; ++i)
        {
            const Key &k = v[i].first; const Val &s = v[i].second;
            ps2xSafeLog("[rdblend] %s prim=%03llx alpha=%llx fbmsk=%08x test=%llx tex0=%09llx skip=%s n=%u tris=%u bbox=%d,%d-%d,%d blend=%d\n",
                        std::get<0>(k) ? "2D" : "3D", (unsigned long long)std::get<1>(k), (unsigned long long)std::get<2>(k),
                        std::get<3>(k), (unsigned long long)std::get<4>(k), (unsigned long long)std::get<5>(k),
                        reasonName(std::get<6>(k)), s.n, s.tris,
                        s.x1 >= s.x0 ? (int)s.x0 : 0, s.y1 >= s.y0 ? (int)s.y0 : 0, s.x1 >= s.x0 ? (int)s.x1 : 0, s.y1 >= s.y0 ? (int)s.y1 : 0, s.blend);
        }
        m.clear();
    }
};
static RdBlendCensus g_rdbCensus; // PROBE-RD29

std::atomic<uint32_t> g_rdRepeatFrames{0};       // FEAT-RD33c
std::atomic<uint32_t> g_rdClassicFramesTotal{0}; // FEAT-RD33c

bool isRdScissorVisible(uint64_t packed, bool have, float fbW, float fbH) // FEAT-RD34
{
    if (have && packed != 0ull && fbW > 0.0f && fbH > 0.0f)
    {
        const float x0 = static_cast<float>(packed & 0x7FFull);
        const float x1 = static_cast<float>((packed >> 16) & 0x7FFull);
        const float y0 = static_cast<float>((packed >> 32) & 0x7FFull);
        const float y1 = static_cast<float>((packed >> 48) & 0x7FFull);
        if (x0 >= fbW || y0 >= fbH || x1 < x0 || y1 < y0)
            return false;
    }
    return true;
}

struct Rd35Rec { uint64_t prim, alpha, frame, tex0; float bb[4]; }; // PROBE-RD35
struct Rd35Stats { Rd35Rec last[3]; uint32_t n2d = 0, n3d = 0; bool usedNew = false, valid = false; int chosen2d = -1; uint32_t draws2d = 0, mainSerials = 0, glErr = 0; }; // PROBE-RD35 // RDG1
extern Rd35Stats g_rd35;

bool isRd3dBatchDrawable(const RdBatch &batch, const RdGpuSlot &slot, float fbW, float fbH) // FEAT-RD34
{
    if (batch.count == 0u || slot.vao == 0u)
        return false;
    uint64_t gsPrim = 0ull, gsAlpha = 0ull, gsTest = 0ull, gsFrame = 0ull, gsZbuf = 0ull;
    uint64_t gsTex0 = 0ull, gsTex1 = 0ull, gsClamp = 0ull, gsTexa = 0ull, gsFba = 0ull;
    const bool gsOk = ps2xRdLookupState(batch.kickSerial, gsPrim, gsAlpha, gsTest, gsFrame,
                                        gsZbuf, gsTex0, gsTex1, gsClamp, gsTexa, gsFba);
    uint64_t xoMp = 0ull;
    if (!gsOk || !ps2xRdLookupXyoff(batch.kickSerial, xoMp))
        return false;
    const uint64_t frame = gsFrame;
    if (ps2xRdHasDisplayFbp() && !ps2xRdIsDisplayFbp(static_cast<uint32_t>(frame & 0x1FFull)))
        return false;
    constexpr uint64_t kPrimAbeTmeMask = (1ull << 6) | (1ull << 4);
    const uint64_t prim = (batch.prim & ~kPrimAbeTmeMask) | (gsPrim & kPrimAbeTmeMask);
    if (((prim >> 4) & 1ull) && ps2xRdIsFbFeedback(batch.kickSerial))
        return false;
    const uint32_t ate = static_cast<uint32_t>(gsTest & 1ull);
    const uint32_t atst = ate ? static_cast<uint32_t>((gsTest >> 1) & 7ull) : 1u;
    const int afail = (ate && atst != 1u) ? static_cast<int>((gsTest >> 12) & 3ull) : 0;
    if (atst == 0u && afail == 0)
        return false;
    const uint32_t zte = static_cast<uint32_t>((gsTest >> 16) & 1ull);
    const uint32_t ztst = static_cast<uint32_t>((gsTest >> 17) & 3ull);
    if (zte && ztst == 0u)
        return false;
    uint64_t sc = 0ull;
    const bool scOk = ps2xRdLookupScissor(batch.kickSerial, sc);
    return isRdScissorVisible(sc, scOk, fbW, fbH);
}

Rd35Stats g_rd35; // PROBE-RD35

static bool isRdFbTexDisplayBatch(const Rd2DBatch &b) // FIX-RD79
{
    const uint32_t psm = static_cast<uint32_t>((b.tex0 >> 20) & 0x3Full); // PROBE-RD82C
    const bool isPsm27 = (psm == 0x1Bu); // PROBE-RD82C
    if (!rdFbTexOn() || !rdFbTexSubstOn()) // FIX-RD80 // PROBE-RD82C
    {
        if (isPsm27) ++s_psm27WhyOff; // PROBE-RD82C
        return false;
    }
    if (s_prevTexId == 0u) // FIX-RD80 // PROBE-RD82C
    {
        if (isPsm27) ++s_psm27WhyNoPrev; // PROBE-RD82C
        return false;
    }
    if (((b.prim >> 4) & 1ull) == 0ull || !ps2xRdIsFbFeedback(b.texSerial))
    {
        if (isPsm27) ++s_psm27WhyNotFb; // PROBE-RD82C
        return false;
    }
    const bool okPsm = (rdFbTexCtOn() && (psm == 0u || psm == 1u || psm == 2u || psm == 10u)) || // FIX-RD82H: CT32, CT24, CT16, CT16S
                       (rdFbTexAnyPsmOn() && (psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu)); // FIX-RD82: PSMT8H, PSMT4HL, PSMT4HH
    if (!okPsm)
    {
        if (isPsm27) ++s_psm27WhyBadPsm; // PROBE-RD82C
        return false;
    }
    const uint32_t page = static_cast<uint32_t>(b.tex0 & 0x3FFFull) / 32u;
    const uint32_t dstFbp = static_cast<uint32_t>(b.frame & 0x1FFull); // FIX-RD82D
    uint32_t fbF[8], fbL[8];
    const uint32_t nfb = ps2xRdFbPageRanges(fbF, fbL);
    for (uint32_t k = 0u; k < nfb; ++k)
    {
        if (ps2xRdIsDisplayFbp(fbF[k]) && page >= fbF[k] && page <= fbL[k])
        {
            if (dstFbp == page) // FIX-RD82E
            {
                if (isPsm27) ++s_psm27WhyDispFbp; // PROBE-RD82C
                return false;
            }
            if (isPsm27) ++s_psm27Ok; // PROBE-RD82C
            return true;
        }
    }
    if (isPsm27) // PROBE-RD82C
    {
        ++s_psm27WhyNoRange;
        static uint32_t s_rangeMissLogs = 0u;
        if (s_rangeMissLogs < 5u)
        {
            ++s_rangeMissLogs;
            if (nfb == 0u)
                ps2xSafeLog("[rdfb] range-miss page=%x nfb=0 first=0..0 isDisp=0\n", page);
            for (uint32_t k = 0u; k < nfb; ++k)
                ps2xSafeLog("[rdfb] range-miss page=%x nfb=%u first=%x..%x isDisp=%d\n",
                            page, nfb, fbF[k], fbL[k], ps2xRdIsDisplayFbp(fbF[k]) ? 1 : 0);
        }
    }
    return false;
}

static unsigned int findRdSnapTex(const Rd2DBatch &b, bool touch) // FIX-RD81
{
    if (!rdSnapOn() || ((b.prim >> 4) & 1ull) == 0ull)
        return 0u;
    const uint32_t psm = static_cast<uint32_t>((b.tex0 >> 20) & 0x3Full);
    if (psm != 0u && psm != 1u && psm != 2u && psm != 10u)
        return 0u;
    const uint32_t startPage = static_cast<uint32_t>(b.tex0 & 0x3FFFull) / 32u;
    for (size_t i = 0u; i < kRdSnapSlots; ++i)
    {
        RdSnapSlot &slot = s_snapSlots[i];
        if (!slot.valid || slot.texId == 0u || slot.destFbp == 0xFFFFFFFFu)
            continue;
        if (startPage >= slot.destFbp && startPage <= slot.destFbp + 0x1Bu)
        {
            if (touch)
            {
                slot.lastUsedFrame = s_snapFrameSeq;
                ++s_snapUsedCount;
            }
            return slot.texId;
        }
    }
    return 0u;
}

static bool isRdCopyBufSkip(const Rd2DBatch &b) // FIX-RD87 // FIX-RD87B
{
    if (!rdFbTexOn() || !rdCopySkipOn() || ((b.prim >> 4) & 1ull) == 0ull)
        return false;
    const uint32_t tw = static_cast<uint32_t>((b.tex0 >> 26) & 0xFull); // FIX-RD87B
    const uint32_t th = static_cast<uint32_t>((b.tex0 >> 30) & 0xFull); // FIX-RD87B
    if (tw != 9u || th != 9u) // FIX-RD87B
        return false;
    const float rawW = static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed)); // FIX-RD87B
    const float rawH = static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed)); // FIX-RD87B
    const float vpW = (rawW > 0.0f) ? rawW : 512.0f; // FIX-RD87B
    const float vpH = (rawH > 0.0f) ? rawH : 448.0f; // FIX-RD87B
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f; // FIX-RD87B
    const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size()); // FIX-RD87B
    for (size_t vi = b.first; vi < e2; ++vi) // FIX-RD87B
    {
        const RdVert &v = g_rdGpu.local2dScratch[vi];
        x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
        y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
    }
    if (x1 < x0 || y1 < y0) // FIX-RD87B
        return false;
    x0 = std::max(0.0f, std::min(x0, vpW)); x1 = std::max(0.0f, std::min(x1, vpW)); // FIX-RD87B
    y0 = std::max(0.0f, std::min(y0, vpH)); y1 = std::max(0.0f, std::min(y1, vpH)); // FIX-RD87B
    if ((x1 - x0) * (y1 - y0) < 0.80f * vpW * vpH) // FIX-RD87B
        return false;
    const uint32_t tbp0 = static_cast<uint32_t>(b.tex0 & 0x3FFFull); // FIX-RD87B
    const uint64_t curFrame = s_snapFrameSeq.load(std::memory_order_relaxed);
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(s_snapMutex);
        for (const RdCopyBufEntry &cb : copyBufFbps)
        {
            if ((curFrame - cb.lastSeenFrame) < 900ull && tbp0 == cb.destFbp * 32u) // FIX-RD87B
            {
                registered = true;
                break;
            }
        }
    }
    if (!registered)
        return false;
    return findRdSnapTex(b, false) == 0u;
}

// PROBE-V3N: draw sequence tracking
static uint64_t s_v3nLast3dSeq = 0; // PROBE-V3N
static uint64_t s_v3nLast2dSeq = 0; // PROBE-V3N
static uint32_t s_v3nLastFlags = 0; // PROBE-V3N

void ps2xRdGetDrawSeqs(uint64_t *seq3d, uint64_t *seq2d, uint32_t *flags) // PROBE-V3N
{
    if (seq3d)
        *seq3d = s_v3nLast3dSeq;
    if (seq2d)
        *seq2d = s_v3nLast2dSeq;
    if (flags)
        *flags = s_v3nLastFlags;
}

// PROBE-V3O: published sequences and chooser probe
static int32_t s_v3oLastChosen2d = -1;  // PROBE-V3O
static uint32_t s_v3oLastCoverFail = 0; // PROBE-V3O

void ps2xRdGetPublishedSeqs(uint64_t *gen3d, uint64_t *gen2d) // PROBE-V3O
{
    std::lock_guard<std::mutex> lock(g_rdMutex);
    if (gen3d)
        *gen3d = g_rd3dSeq;
    if (gen2d)
        *gen2d = g_rd2dSeq;
}

void ps2xRdGetDrawSeqs2(int32_t *chosen2d, uint32_t *coverFail) // PROBE-V3O
{
    if (chosen2d)
        *chosen2d = s_v3oLastChosen2d;
    if (coverFail)
        *coverFail = s_v3oLastCoverFail;
}

// PROBE-V3Q: cover probe for rdRingCoversRanges failures
static const bool s_coverLog = []{
    extern void ps2xLoadConfig();
    ps2xLoadConfig();
    const char *e = std::getenv("PS2X_RD_COVERLOG");
    return e && e[0] == '1';
}(); // PROBE-V3Q
static uint32_t s_coverLogLines = 0;  // PROBE-V3Q
static uint32_t s_coverFailCount = 0; // PROBE-V3Q

// FIX-V3S: env-gated constant one-flip pairing lag in 2D/3D chooser
static const bool s_pairLag = []{
    extern void ps2xLoadConfig();
    ps2xLoadConfig();
    const char *e = std::getenv("PS2X_RD_PAIRLAG");
    return e && e[0] == '1';
}(); // FIX-V3S

bool ps2xRdDraw(float x, float y, float w, float h) // FEAT-RD1
{
    rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c
    const bool pace2On = pace2Enabled(); // PROBE-PACE2
    if (pace2On) // PROBE-PACE2
        g_pace2Rd = Pace2RdState{};
    Pace2Scope pace2TotalScope(g_pace2Rd.total, pace2On); // PROBE-PACE2
    g_rd35.valid = false; // PROBE-RD35
    g_rd35.n2d = 0u;
    g_rd35.n3d = 0u;
    g_rd35.usedNew = false;
    g_rd35.chosen2d = -1;
    bool hasNew2d = false; // FEAT-RD8
    bool needRealign = false; // FEAT-RD13
    int chosen2dIdx = -1;     // FEAT-RD33
    uint32_t coverFail = 0;   // PROBE-V3O
    bool pendingUpload[kRdSlotCount] = {}; // FEAT-RD14 // FEAT-RD34
    uint32_t evictedMaxSnap = 0u; // FEAT-RD34
    bool v3nNew3d = false; // PROBE-V3N
    bool v3nNew2d = false; // PROBE-V3N
    {
        Pace2Scope pace2B1Scope(g_pace2Rd.b1, pace2On); // PROBE-PACE2
        std::lock_guard<std::mutex> lock(g_rdMutex);
        if (!g_rdEverPublished && !g_rdGpu.everUploaded)
        {
            ++g_rdGpu.drawsEmpty;
            ++g_rdGpu.drawsEmptySempty;
            s_v3nLast3dSeq = g_rdGpu.last3dSeq; // PROBE-V3N
            s_v3nLast2dSeq = g_rdGpu.last2dSeq; // PROBE-V3N
            s_v3nLastFlags = (1u << 2);         // PROBE-V3N
            s_v3oLastChosen2d = -1;             // PROBE-V3O
            s_v3oLastCoverFail = 0;             // PROBE-V3O
            if (ps2xRdTakeoverOn()) // FEAT-RD12: clear to black instead of drawing stale GS texture
                return true;
            return false;
        }
        evictedMaxSnap = g_rd3dEvictedMaxSerial;
        if (g_rd3dSeq != g_rdGpu.last3dSeq || g_rd2dSeq != g_rdGpu.last2dSeq) // FEAT-RD13 // FEAT-RD33
        {
            uint32_t ringMin = 0u;
            uint32_t ringMax = 0u;
            for (size_t idx = 0; idx < kRdSlotCount; ++idx) // FEAT-RD34
            {
                const Rd3DFrame &rf = g_rd3dRing[idx];
                const RdGpuSlot &gs = g_rdGpu.slots[idx];
                if (rf.seq == 0u && gs.seq == 0u)
                    continue;
                const uint32_t sMin = (rf.seq != 0u) ? rf.sMin : gs.sMin;
                const uint32_t sMax = (rf.seq != 0u) ? rf.sMax : gs.sMax;
                if (sMin == 0u)
                    continue;
                if (ringMin == 0u || sMin < ringMin)
                    ringMin = sMin;
                if (sMax > ringMax)
                    ringMax = sMax;
            }
            const int firstTry = s_pairLag ? 1 : 0;   // FIX-V3S
            const int secondTry = s_pairLag ? 0 : 1;  // FIX-V3S
            if (g_rd2dFrames[firstTry].valid && rdRingCoversRanges(ringMin, ringMax, g_rd2dFrames[firstTry].mainRanges)) // FIX-V3S
                chosen2dIdx = firstTry; // FIX-V3S
            else if (g_rd2dFrames[secondTry].valid && rdRingCoversRanges(ringMin, ringMax, g_rd2dFrames[secondTry].mainRanges)) // FIX-V3S
                chosen2dIdx = secondTry; // FIX-V3S
            else if (g_rd2dFrames[0].valid || g_rd2dFrames[1].valid) // PROBE-V3O
            {
                coverFail = 1; // PROBE-V3O
                if (s_coverLog && s_coverLogLines < 400u) // PROBE-V3Q
                {
                    const uint32_t counter = s_coverFailCount++; // PROBE-V3Q
                    if (counter < 40u || (counter % 5u) == 0u) // PROBE-V3Q
                    {
                        ++s_coverLogLines; // PROBE-V3Q
                        std::string slotsStr = "["; // PROBE-V3Q
                        bool firstSlot = true; // PROBE-V3Q
                        for (size_t idx = 0; idx < kRdSlotCount; ++idx) // PROBE-V3Q
                        {
                            const Rd3DFrame &rf = g_rd3dRing[idx]; // PROBE-V3Q
                            const RdGpuSlot &gs = g_rdGpu.slots[idx]; // PROBE-V3Q
                            const uint64_t seq = (rf.seq != 0u) ? rf.seq : gs.seq; // PROBE-V3Q
                            if (seq == 0u) // PROBE-V3Q
                                continue; // PROBE-V3Q
                            const uint32_t sMin = (rf.seq != 0u) ? rf.sMin : gs.sMin; // PROBE-V3Q
                            const uint32_t sMax = (rf.seq != 0u) ? rf.sMax : gs.sMax; // PROBE-V3Q
                            if (!firstSlot) // PROBE-V3Q
                                slotsStr += ","; // PROBE-V3Q
                            char sbuf[64]; // PROBE-V3Q
                            std::snprintf(sbuf, sizeof(sbuf), "%llu:%u-%u", // PROBE-V3Q
                                          static_cast<unsigned long long>(seq), sMin, sMax); // PROBE-V3Q
                            slotsStr += sbuf; // PROBE-V3Q
                            firstSlot = false; // PROBE-V3Q
                        }
                        slotsStr += "]"; // PROBE-V3Q
                        const uint32_t f0First = g_rd2dFrames[0].mainRanges.empty() ? 0u : g_rd2dFrames[0].mainRanges.front().first; // PROBE-V3Q
                        const uint32_t f0Last = g_rd2dFrames[0].mainRanges.empty() ? 0u : g_rd2dFrames[0].mainRanges.back().last; // PROBE-V3Q
                        const uint32_t f1First = g_rd2dFrames[1].mainRanges.empty() ? 0u : g_rd2dFrames[1].mainRanges.front().first; // PROBE-V3Q
                        const uint32_t f1Last = g_rd2dFrames[1].mainRanges.empty() ? 0u : g_rd2dFrames[1].mainRanges.back().last; // PROBE-V3Q
                        uint32_t missing0 = 0u; // PROBE-V3Q
                        for (const RdSerialRange &r : g_rd2dFrames[0].mainRanges) // PROBE-V3Q
                        {
                            if (r.last >= r.first) // PROBE-V3Q
                            {
                                const uint32_t total = r.last - r.first + 1u; // PROBE-V3Q
                                if (ringMin != 0u && ringMin <= ringMax) // PROBE-V3Q
                                {
                                    const uint32_t ovFirst = std::max(r.first, ringMin); // PROBE-V3Q
                                    const uint32_t ovLast = std::min(r.last, ringMax); // PROBE-V3Q
                                    const uint32_t inside = (ovFirst <= ovLast) ? (ovLast - ovFirst + 1u) : 0u; // PROBE-V3Q
                                    missing0 += (total - inside); // PROBE-V3Q
                                }
                                else // PROBE-V3Q
                                {
                                    missing0 += total; // PROBE-V3Q
                                }
                            }
                        }
                        ps2xSafeLog("[cover] g3=%llu g2=%llu ringMin=%u ringMax=%u slots=%s f0.valid=%d f0.ranges=%zu f0.first=%u f0.last=%u f0.n=%u f1.valid=%d f1.ranges=%zu f1.first=%u f1.last=%u f1.n=%u missing0=%u\n", // PROBE-V3Q
                                    static_cast<unsigned long long>(g_rd3dSeq), // PROBE-V3Q
                                    static_cast<unsigned long long>(g_rd2dSeq), // PROBE-V3Q
                                    ringMin, ringMax, slotsStr.c_str(), // PROBE-V3Q
                                    g_rd2dFrames[0].valid ? 1 : 0, // PROBE-V3Q
                                    g_rd2dFrames[0].mainRanges.size(), // PROBE-V3Q
                                    f0First, f0Last, g_rd2dFrames[0].mainSerialCount, // PROBE-V3Q
                                    g_rd2dFrames[1].valid ? 1 : 0, // PROBE-V3Q
                                    g_rd2dFrames[1].mainRanges.size(), // PROBE-V3Q
                                    f1First, f1Last, g_rd2dFrames[1].mainSerialCount, // PROBE-V3Q
                                    missing0); // PROBE-V3Q
                    }
                }
            }

            if (chosen2dIdx >= 0)
            {
                if (g_rd3dSeq != g_rdGpu.last3dSeq) // FEAT-RD13 // FEAT-RD14
                {
                    size_t orderCount = 0u;
                    uint64_t orderSeq[kRdSlotCount] = {};
                    for (size_t idx = 0; idx < kRdSlotCount; ++idx) // FEAT-RD34
                    {
                        const uint64_t s = (g_rd3dRing[idx].seq != 0u) ? g_rd3dRing[idx].seq : g_rdGpu.slots[idx].seq;
                        if (s == 0u)
                            continue;
                        size_t pos = orderCount;
                        while (pos > 0u && orderSeq[pos - 1u] > s)
                        {
                            g_rdGpu.activeOrder[pos] = g_rdGpu.activeOrder[pos - 1u];
                            orderSeq[pos] = orderSeq[pos - 1u];
                            --pos;
                        }
                        g_rdGpu.activeOrder[pos] = idx;
                        orderSeq[pos] = s;
                        ++orderCount;
                    }
                    g_rdGpu.ring3dCount = orderCount;
                    for (size_t i = 0; i < orderCount; ++i)
                    {
                        const size_t idx = g_rdGpu.activeOrder[i];
                        Rd3DFrame &rf = g_rd3dRing[idx];
                        RdGpuSlot &gs = g_rdGpu.slots[idx];
                        if (!g_rdPinnedSlots[idx] && rf.seq != 0u && rf.seq != gs.seq) // FEAT-RD34
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
                    v3nNew3d = true; // PROBE-V3N
                }
                if (g_rd2dSeq != g_rdGpu.last2dSeq) // FEAT-RD13
                {
                    g_rdGpu.frames2d[0] = g_rd2dFrames[0];
                    g_rdGpu.frames2d[1] = g_rd2dFrames[1];
                    g_rdGpu.last2dSeq = g_rd2dSeq;
                    v3nNew2d = true; // PROBE-V3N
                }
                needRealign = true;
            }
            else if (!g_rdGpu.hasCompleteComposition)
            {
                ++g_rdGpu.drawsEmpty;
                ++g_rdGpu.drawsEmptyMissing;
                s_v3nLast3dSeq = g_rdGpu.last3dSeq; // PROBE-V3N
                s_v3nLast2dSeq = g_rdGpu.last2dSeq; // PROBE-V3N
                s_v3nLastFlags = (1u << 2);         // PROBE-V3N
                s_v3oLastChosen2d = chosen2dIdx;    // PROBE-V3O
                s_v3oLastCoverFail = coverFail;     // PROBE-V3O
                return ps2xRdTakeoverOn();
            }
        }
    }

    if (!g_rdGpu.init)
    {
        g_rdGpu.init = true;
        g_rdGpu.shaderReady = rdgfx::initShader(); // RDG1
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        g_rdGpu.flip[0] = 1.0f;
        g_rdGpu.flip[1] = (flipBits & 1) ? 1.0f : -1.0f; // FEAT-RD2: y flip default; bit 1 = do NOT flip y
        g_rdGpu.flip[2] = (flipBits & 2) ? -1.0f : 1.0f;
        if (rdgfx::hasUniform(rdgfx::Uniform::Flip)) // RDG1
            rdgfx::setUniformVec3Immediate(rdgfx::Uniform::Flip, g_rdGpu.flip); // RDG1
        for (size_t i = 0; i < kRdSlotCount; ++i) // FEAT-RD14 // FEAT-RD34
            g_rdGpu.slots[i].vao = rdgfx::createVertexArray(); // RDG1
        g_rdGpu.vao2d = rdgfx::createVertexArray(); // FEAT-RD8 // RDG1
    }

    { // PROBE-PACE2
        Pace2Scope pace2B2Scope(g_pace2Rd.b2, pace2On); // PROBE-PACE2
        for (size_t idx = 0; idx < kRdSlotCount; ++idx) // FEAT-RD14 // FEAT-RD34
        {
            if (pendingUpload[idx])
            {
                g_rdGpu.everUploaded = true;
                uploadRdSlot(g_rdGpu.slots[idx], g_rdGpu.pendingVerts[idx], g_rdGpu.pendingIndices[idx], g_rdGpu.slots[idx].batches);
                if (g_rd24On || g_rdCamOn || g_rdBlendOn || g_rdReasonOn || g_rdFogLog) // PROBE-RD24 // PROBE-RD29 // PROBE-RD26 // PROBE-RD59 // PROBE-E1c: CPU copy of uploaded verts + final 16-bit indices (relative to batch.baseVertex)
                {
                    g_rd24Verts[idx] = g_rdGpu.pendingVerts[idx];
                    if (g_rdGpu.slots[idx].indexCount != 0)
                        g_rd24Idx[idx] = g_rdGpu.idx16Scratch;
                    else
                        g_rd24Idx[idx].clear();
                }
            }
        }
    }

    const float fbW = static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed)); // FEAT-RD34
    const float fbH = static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed)); // FEAT-RD34
    bool usedNewComposition = false; // FEAT-RD34
    uint32_t candDrawable3d = 0u;    // FEAT-RD34
    static double s_avgMainSerials = 0.0; // FEAT-RD37
    static uint32_t s_consecPartial = 0u; // FEAT-RD37
    static uint32_t s_partialFrames = 0u; // FEAT-RD37
    if (g_rdNeedGpuReset.exchange(false, std::memory_order_acq_rel)) // FEAT-RD49: clear stale composition/EMA/slot state after reset
    {
        s_avgMainSerials = 0.0;
        s_consecPartial = 0u;
        g_rdGpu.hasCompleteComposition = false;
        g_rdGpu.prevDrawn3dCount = 0u;
        g_rdGpu.lastMainSerialCount = 0u;
        g_rdGpu.activeGMin = 0u;
        g_rdGpu.ring3dCount = 0u;
        g_rdGpu.last3dSeq = 0u;
        g_rdGpu.last2dSeq = 0u;
        g_rdGpu.everUploaded = false;
        g_rdGpu.drawBatches.clear();
        g_rdGpu.local2dScratch.clear();
        g_rdGpu.local2dBatches.clear();
        g_rdGpu.frames2d[0] = Rd2DFrame{};
        g_rdGpu.frames2d[1] = Rd2DFrame{};
        for (size_t i = 0; i < kRdSlotCount; ++i)
        {
            g_rdGpu.slots[i].seq = 0u;
            g_rdGpu.slots[i].batches.clear();
            g_rdGpu.pendingVerts[i].clear();
            g_rdGpu.pendingIndices[i].clear();
        }
    }
    if (needRealign && chosen2dIdx >= 0) // FEAT-RD13 // FEAT-RD14 // FEAT-RD33
    {
        const Rd2DFrame &chosen2d = g_rdGpu.frames2d[chosen2dIdx];
        std::vector<RdDrawBatch> candBatches; // FEAT-RD34
        uint64_t fVerts = 0u;
        uint64_t fIndices = 0u;
        uint32_t candSkipped = g_rdGpu.lastSkippedDraws;
        uint32_t candMasked = g_rdGpu.lastMaskedDraws;
        float candVpScaleZ = g_rdGpu.vpScaleZ;
        float candVpOffsetZ = g_rdGpu.vpOffsetZ;
        if (!chosen2d.mainRanges.empty())
        {
            const uint32_t sFirst = chosen2d.mainRanges.front().first;
            const uint32_t sLast = chosen2d.mainRanges.back().last;
            for (size_t i = 0; i < g_rdGpu.ring3dCount; ++i)
            {
                const uint32_t slotIdx = static_cast<uint32_t>(g_rdGpu.activeOrder[i]);
                const RdGpuSlot &f3 = g_rdGpu.slots[slotIdx];
                if (f3.batches.empty() || f3.sMin == 0u || f3.sMax < sFirst || f3.sMin > sLast)
                    continue;
                bool anyFromFrame = false;
                for (const RdBatch &b : f3.batches)
                {
                    const uint32_t ms = b.kickSerial & 0x3FFFFFFFu;
                    if (rdSerialInRanges(ms, chosen2d.mainRanges))
                    {
                        if (!anyFromFrame)
                        {
                            anyFromFrame = true;
                            fVerts += f3.vertCount;
                            candSkipped = f3.skipped;
                            candMasked = f3.masked;
                            if (f3.vpScaleZ != 0.0f)
                            {
                                candVpScaleZ = f3.vpScaleZ;
                                candVpOffsetZ = f3.vpOffsetZ;
                            }
                        }
                        candBatches.push_back(RdDrawBatch{b, slotIdx});
                        fIndices += b.count;
                    }
                }
            }
        }

        int candEmptyReason = 0; // FEAT-RD34: 1=Sempty, 2=missing, 3=overwritten, 4=skipped
        if (chosen2d.mainRanges.empty())
            candEmptyReason = 1;
        else if (candBatches.empty())
            candEmptyReason = (evictedMaxSnap >= chosen2d.mainRanges.front().first) ? 3 : 2;
        else
        {
            for (const RdDrawBatch &db : candBatches)
            {
                if (isRd3dBatchDrawable(db.batch, g_rdGpu.slots[db.slotIdx], fbW, fbH))
                    ++candDrawable3d;
            }
            if (candDrawable3d == 0u)
                candEmptyReason = 4;
        }

        // FEAT-RD37: reject partially captured compositions (< 0.6 * exponential average), adapting after 4 consecutive
        const uint32_t candSerials = chosen2d.mainSerialCount; // FEAT-RD37
        bool rejectPartial = false; // FEAT-RD37
        if (!(candDrawable3d == 0u && g_rdGpu.prevDrawn3dCount > 0u) &&
            g_rdGpu.hasCompleteComposition && s_avgMainSerials > 0.0 &&
            static_cast<double>(candSerials) < 0.6 * s_avgMainSerials) // FEAT-RD37
        {
            ++s_consecPartial; // FEAT-RD37
            if (s_consecPartial < 4u) // FEAT-RD37
                rejectPartial = true; // FEAT-RD37
        }

        // FEAT-RD34(b): if a composition would draw 0 3D batches while the previous had > 0, present the previous instead
        if ((candDrawable3d == 0u && g_rdGpu.prevDrawn3dCount > 0u) || rejectPartial) // FEAT-RD34 // FEAT-RD37
        {
            if (rejectPartial) // FEAT-RD37
                ++s_partialFrames; // FEAT-RD37
            needRealign = false;
        }
        else
        {
            if (!g_rdGpu.hasCompleteComposition || s_avgMainSerials <= 0.0 || s_consecPartial >= 4u) // FEAT-RD37
                s_avgMainSerials = static_cast<double>(candSerials); // FEAT-RD37
            else // FEAT-RD37
                s_avgMainSerials = 0.1 * static_cast<double>(candSerials) + 0.9 * s_avgMainSerials; // FEAT-RD37
            s_consecPartial = 0u; // FEAT-RD37

            usedNewComposition = true;
            g_rdGpu.lastEmptyReason = candEmptyReason;
            g_rdGpu.local2dScratch = chosen2d.verts;
            g_rdGpu.local2dBatches = chosen2d.batches;
            g_rdGpu.lastRtSkipped = chosen2d.rtSkipped;
            g_rdGpu.lastPointLineSkipped = chosen2d.pointLineSkipped;
            g_rdGpu.activeGMin = chosen2d.mainRanges.empty() ? 0u : chosen2d.mainRanges.front().first;
            hasNew2d = true;

            g_rdGpu.drawBatches.swap(candBatches);
            g_rdGpu.frameVerts = fVerts;
            g_rdGpu.frameIndices = fIndices;
            g_rdGpu.frameTris = fIndices / 3u;
            g_rdGpu.lastSkippedDraws = candSkipped;
            g_rdGpu.lastMaskedDraws = candMasked;
            g_rdGpu.vpScaleZ = candVpScaleZ;
            g_rdGpu.vpOffsetZ = candVpOffsetZ;
            g_rdGpu.lastMainSerialCount = chosen2d.mainSerialCount;
            g_rdGpu.hasCompleteComposition = true;

            std::lock_guard<std::mutex> lock(g_rdMutex); // FEAT-RD34(a): pin slots referenced by last presented composition
            for (size_t i = 0; i < kRdSlotCount; ++i)
                g_rdPinnedSlots[i] = false;
            for (const RdDrawBatch &db : g_rdGpu.drawBatches)
            {
                if (db.slotIdx < kRdSlotCount)
                    g_rdPinnedSlots[db.slotIdx] = true;
            }
        }
    }

    uint32_t first3dSerial30 = 0x3FFFFFFFu; // FIX-RD66 // FIX-RD65c
    for (const auto &db : g_rdGpu.drawBatches)
    {
        const uint32_t s30 = db.batch.kickSerial & 0x3FFFFFFFu;
        if (s30 < first3dSerial30)
            first3dSerial30 = s30;
    }
    bool has2dFbp[512]{}; // FIX-RD66 // FIX-RD65c
    for (const auto &b2 : g_rdGpu.local2dBatches)
        has2dFbp[b2.frame & 0x1FFull] = true;
    auto isRdDimOverlay = [&](const Rd2DBatch &b, float vpW, float vpH) -> bool // FIX-RD66 // FIX-RD65c
    {
        if (!g_rdDimLateOn || g_rdGpu.drawBatches.empty())
            return false;
        if (((b.prim >> 4) & 1ull) == 0ull || ((b.prim >> 6) & 1ull) == 0ull)
            return false;
        if ((b.afterSerial & 0x3FFFFFFFu) >= first3dSerial30)
            return false;
        const uint32_t tbp0 = static_cast<uint32_t>(b.tex0 & 0x3FFFull);
        if (((tbp0 & 31u) == 0u) && has2dFbp[tbp0 >> 5])
            return false;
        const float cw = (vpW > 0.0f) ? vpW : 512.0f;
        const float ch = (vpH > 0.0f) ? vpH : 448.0f;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size());
        for (size_t vi = b.first; vi < e2; ++vi)
        {
            const RdVert &v = g_rdGpu.local2dScratch[vi];
            x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
            y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
        }
        if (x1 < x0 || y1 < y0)
            return false;
        x0 = std::max(0.0f, std::min(x0, cw)); x1 = std::max(0.0f, std::min(x1, cw));
        y0 = std::max(0.0f, std::min(y0, ch)); y1 = std::max(0.0f, std::min(y1, ch));
        return (x1 - x0) * (y1 - y0) >= 0.90f * cw * ch;
    };

    if (hasNew2d) // FEAT-RD8
    {
        // FEAT-RD16: Hor+ 2D HUD layout when PS2X_ASPECT=fill (g_rdAspectK > 1)
        static const bool s_hudCenter = []{ const char *e = std::getenv("PS2X_RD_HUDCENTER"); return !(e && e[0] == '0'); }(); // FIX-RD64
        static const bool s_menuLayer = []{ const char *e = std::getenv("PS2X_RD_MENULAYER"); return !(e && e[0] == '0'); }(); // FIX-RD65
        static uint32_t s_menuLatch = 0u; // FIX-RD65
        static bool s_menuLayerActive = false; // FIX-RD65
        static const char *s_menuTrigger = "none"; // FIX-RD65c
        if (g_rdAspectK > 1.0f && fbW > 0.0f && fbH > 0.0f && w > 0.0f && h > 0.0f)
        {
            const auto batchBounds = [&](const Rd2DBatch &b, float &minX, float &maxX, float &minY, float &maxY) -> bool {
                if (b.count == 0u || static_cast<size_t>(b.first) + b.count > g_rdGpu.local2dScratch.size())
                    return false;
                minX = maxX = g_rdGpu.local2dScratch[b.first].pos[0];
                minY = maxY = g_rdGpu.local2dScratch[b.first].pos[1];
                for (uint32_t k = 1u; k < b.count; ++k)
                {
                    const float vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0];
                    const float vy2 = g_rdGpu.local2dScratch[b.first + k].pos[1];
                    if (vx2 < minX) minX = vx2;
                    if (vx2 > maxX) maxX = vx2;
                    if (vy2 < minY) minY = vy2;
                    if (vy2 > maxY) maxY = vy2;
                }
                return true;
            };
            bool tallHit = false; // FIX-RD65 // FIX-RD65c
            bool dimHit = false; // FIX-RD65c
            size_t triggerIdx = SIZE_MAX;
            float trigMinX = 0.0f, trigMaxX = 0.0f, trigMinY = 0.0f, trigMaxY = 0.0f;
            if (s_menuLayer) // FIX-RD65
            {
                for (size_t bi = 0; bi < g_rdGpu.local2dBatches.size(); ++bi)
                {
                    const Rd2DBatch &b = g_rdGpu.local2dBatches[bi];
                    float minX, maxX, minY, maxY;
                    if (!batchBounds(b, minX, maxX, minY, maxY))
                        continue;
                    const float bw = maxX - minX;
                    const float bh = maxY - minY;
                    const float dc = 0.5f * (minX + maxX) - 0.5f * fbW;
                    if (bw >= 0.35f * fbW && bh >= 0.2f * fbH && bh < 0.9f * fbH && dc >= -0.1f * fbW && dc <= 0.1f * fbW) // FIX-RD65b
                    {
                        tallHit = true;
                        triggerIdx = bi;
                        trigMinX = minX;
                        trigMaxX = maxX;
                        trigMinY = minY;
                        trigMaxY = maxY;
                        break;
                    }
                    if (!dimHit && isRdDimOverlay(b, fbW, fbH)) // FIX-RD65c
                    {
                        dimHit = true;
                        triggerIdx = bi;
                        trigMinX = minX;
                        trigMaxX = maxX;
                        trigMinY = minY;
                        trigMaxY = maxY;
                    }
                }
            }
            const bool menuHit = tallHit || dimHit; // FIX-RD65 // FIX-RD65c
            const bool latched = (s_menuLatch > 0u); // FIX-RD65
            if (tallHit) // FIX-RD65
            {
                s_menuLatch = 45u;
                s_menuTrigger = "tall"; // FIX-RD65c
            }
            else if (dimHit) // FIX-RD65c
            {
                s_menuLatch = (s_menuLatch > 8u) ? (s_menuLatch - 1u) : 8u;
                s_menuTrigger = "dim";
            }
            else if (s_menuLatch > 0u)
                --s_menuLatch;
            const bool menuFrame = menuHit || latched; // FIX-RD65
            if (menuFrame != s_menuLayerActive) // FIX-RD65
            {
                s_menuLayerActive = menuFrame;
                ps2xSafeLog("[rd] menuLayer %s (%s)\n", menuFrame ? "on" : "off", s_menuTrigger); // FIX-RD65c
                if (menuFrame && triggerIdx < g_rdGpu.local2dBatches.size())
                {
                    const Rd2DBatch &tb = g_rdGpu.local2dBatches[triggerIdx];
                    ps2xSafeLog("[rd] menuLayer trigger bbox=%d,%d-%d,%d tex=%d abe=%d tex0=%llx fbp=%x\n",
                                static_cast<int>(trigMinX), static_cast<int>(trigMinY),
                                static_cast<int>(trigMaxX), static_cast<int>(trigMaxY),
                                static_cast<int>((tb.prim >> 4) & 1ull),
                                static_cast<int>((tb.prim >> 6) & 1ull),
                                static_cast<unsigned long long>(tb.tex0),
                                static_cast<unsigned int>(tb.frame & 0x1FFull));
                    int otherLogged = 0;
                    for (size_t bi = 0; bi < g_rdGpu.local2dBatches.size() && otherLogged < 8; ++bi)
                    {
                        if (bi == triggerIdx)
                            continue;
                        const Rd2DBatch &b = g_rdGpu.local2dBatches[bi];
                        float minX, maxX, minY, maxY;
                        if (!batchBounds(b, minX, maxX, minY, maxY))
                            continue;
                        const float bw = maxX - minX;
                        const float bh = maxY - minY;
                        if (bw >= 0.8f * fbW && bh >= 0.9f * fbH)
                            continue;
                        ps2xSafeLog("[rd] menuLayer other bbox=%d,%d-%d,%d tex=%d abe=%d tex0=%llx\n",
                                    static_cast<int>(minX), static_cast<int>(minY),
                                    static_cast<int>(maxX), static_cast<int>(maxY),
                                    static_cast<int>((b.prim >> 4) & 1ull),
                                    static_cast<int>((b.prim >> 6) & 1ull),
                                    static_cast<unsigned long long>(b.tex0));
                        ++otherLogged;
                    }
                }
            }
            const float s = h / fbH;
            const float scaleToShader = (s * fbW) / w;
            // FIX-P1: live HUD safe area anchor mode
            extern float ps2xGfxGetHudSafeAspect(); // FIX-P1
            const float hudSafeAspect = ps2xGfxGetHudSafeAspect(); // FIX-P1
            const bool isEdge = (hudSafeAspect <= 0.0f); // FIX-P1
            const float safeW = isEdge ? w : std::min(w, h * hudSafeAspect); // FIX-P1
            const float m = (isEdge || safeW >= w) ? 0.0f : (0.5f * fbW * (1.0f - safeW / w)); // FIX-P1
            static float s_lastHudSafeAspect = -999.0f; // FIX-P1
            if (std::abs(s_lastHudSafeAspect - hudSafeAspect) > 0.001f) // FIX-P1
            {
                s_lastHudSafeAspect = hudSafeAspect; // FIX-P1
                const char *modeStr = "edge"; // FIX-P1
                char modeBuf[32]; // FIX-P1
                if (hudSafeAspect <= 0.0f)
                {
                    modeStr = "edge"; // FIX-P1
                }
                else if (std::abs(hudSafeAspect - 16.0f / 9.0f) < 0.01f)
                {
                    modeStr = "16:9"; // FIX-P1
                }
                else if (std::abs(hudSafeAspect - 4.0f / 3.0f) < 0.01f)
                {
                    modeStr = "4:3"; // FIX-P1
                }
                else
                {
                    std::snprintf(modeBuf, sizeof(modeBuf), "safe=%.2f", hudSafeAspect); // FIX-P1
                    modeStr = modeBuf; // FIX-P1
                }
                ps2xSafeLog("[hud] anchor=%s margin=%.1f fb px\n", modeStr, m); // FIX-P1
            }
            for (const Rd2DBatch &b : g_rdGpu.local2dBatches)
            {
                float minX, maxX, minY, maxY;
                if (!batchBounds(b, minX, maxX, minY, maxY))
                    continue;
                const float bw = maxX - minX; // FIX-RD64
                const float bh = maxY - minY; // FIX-RD64
                const float cx = 0.5f * (minX + maxX);
                const float dc = cx - 0.5f * fbW; // FIX-RD64
                const bool isWide = (bw >= 0.8f * fbW); // FIX-RD64
                if (isWide && (bh >= 0.9f * fbH || (!menuFrame && !s_hudCenter))) // FIX-RD64 / FIX-RD65
                    continue; // full-width draw stretches to full window
                if (menuFrame || (s_hudCenter && (isWide || (dc >= -0.1f * fbW && dc <= 0.1f * fbW)))) // FIX-RD64 / FIX-RD65
                {
                    for (uint32_t k = 0u; k < b.count; ++k)
                    {
                        float &vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0];
                        vx2 = 0.5f * fbW + (vx2 - 0.5f * fbW) * scaleToShader;
                    }
                }
                else if (cx < 0.5f * fbW)
                {
                    for (uint32_t k = 0u; k < b.count; ++k)
                    {
                        float &vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0]; // FIX-R8
                        vx2 = m + vx2 * scaleToShader; // FIX-R8
                    }
                }
                else
                {
                    for (uint32_t k = 0u; k < b.count; ++k)
                    {
                        float &vx2 = g_rdGpu.local2dScratch[b.first + k].pos[0];
                        vx2 = fbW - m - (fbW - vx2) * scaleToShader; // FIX-R8
                    }
                }
            }
        }
        g_rdGpu.everUploaded = true;
        g_rdGpu.gpu2dVertCount = g_rdGpu.local2dScratch.size();
        uploadRdVbo(g_rdGpu.vao2d, g_rdGpu.vbo2d, g_rdGpu.vbo2dCapVerts, g_rdGpu.local2dScratch);
    }

    static const bool s_draw2d = []{ const char *e = std::getenv("PS2X_RD_2D"); return !(e && e[0] == '0'); }(); // FEAT-RD9
    const bool hasDrawable3d = usedNewComposition ? (candDrawable3d > 0u) : (g_rdGpu.prevDrawn3dCount > 0u); // FEAT-RD34
    const bool hasDrawable2d = (s_draw2d && g_rdGpu.gpu2dVertCount > 0 && g_rdGpu.vao2d != 0 && !g_rdGpu.local2dBatches.empty()); // FEAT-RD34
    if (!g_rdGpu.shaderReady || (!hasDrawable3d && !hasDrawable2d)) // FEAT-RD34(c): never call the clear without drawing something // RDG1
    {
        ++g_rdGpu.drawsEmpty;
        if (g_rdGpu.lastEmptyReason == 1) ++g_rdGpu.drawsEmptySempty;
        else if (g_rdGpu.lastEmptyReason == 2) ++g_rdGpu.drawsEmptyMissing;
        else if (g_rdGpu.lastEmptyReason == 3) ++g_rdGpu.drawsEmptyOverwritten;
        else if (g_rdGpu.lastEmptyReason == 4) ++g_rdGpu.drawsEmptySkipped;
        else ++g_rdGpu.drawsEmptySempty;
        s_v3nLast3dSeq = g_rdGpu.last3dSeq; // PROBE-V3N
        s_v3nLast2dSeq = g_rdGpu.last2dSeq; // PROBE-V3N
        s_v3nLastFlags = (1u << 2);         // PROBE-V3N
        s_v3oLastChosen2d = chosen2dIdx;    // PROBE-V3O
        s_v3oLastCoverFail = coverFail;     // PROBE-V3O
        return ps2xRdTakeoverOn();
    }

    rdgfx::flushBatch(); // RDG1
    ps2xRdTexTickFrame(); // FEAT-RD3

    int rw = 0, rh = 0;
    rdgfx::getBackbufferSize(rw, rh); // RDG1
    int sw = 0, sh = 0; // FIX-RDG3D
    rdgfx::getWindowSize(sw, sh); // FIX-RDG3D
    const float scaleX = (sw > 0) ? (static_cast<float>(rw) / static_cast<float>(sw)) : 1.0f;
    const float scaleY = (sh > 0) ? (static_cast<float>(rh) / static_cast<float>(sh)) : 1.0f;
    const int vx = static_cast<int>(x * scaleX);
    const int vy = static_cast<int>((static_cast<float>(sh) - y - h) * scaleY);
    const int vw = static_cast<int>(w * scaleX);
    const int vh = static_cast<int>(h * scaleY);
    const bool boxClipped = (vw < rw || vh < rh); // FIX-R7

    rdgfx::setViewport(vx, vy, vw, vh); // RDG1
    // FEAT-RD34b: force known GL state (raylib/ImGui may have changed it since our last frame)
    if (boxClipped) // FIX-R7
    {
        rdgfx::setScissorTest(true);
        rdgfx::setScissorRect(vx, vy, vw, vh);
    }
    else
        rdgfx::setScissorTest(false); // RDG1
    rdgfx::setColorMask(true, true, true, true); // RDG1
    g_rdMaskCur = 15;
    rdgfx::setDepthMask(true); // RDG1
    rdgfx::setBlendAlpha(); // RDG1
    rdgfx::bindVertexArray(0u); // RDG1
    { // FEAT-RD9: clear box alpha to 0 (colour untouched via mask)
        rdgfx::setScissorTest(true); // RDG1
        rdgfx::setScissorRect(vx, vy, vw, vh); // RDG1
        rdgfx::setColorMask(false, false, false, true); // RDG1
        rdgfx::clearScreenBuffers(0.0f, 0.0f, 0.0f, 0.0f); // RDG1
        rdgfx::setColorMask(true, true, true, true); // RDG1
        if (boxClipped) // FIX-R7
        {
            rdgfx::setScissorTest(true);
            rdgfx::setScissorRect(vx, vy, vw, vh);
        }
        else
            rdgfx::setScissorTest(false); // RDG1
    }
    rdgfx::setDepthTest(true); // RDG1
    g_rdc.reset(); g_rdc.calls = 0; g_rdc.depthTest = 1; g_rdc.depthMask = 1; // FEAT-RD15 // FEAT-RD34b
    rdgfx::setBackfaceCulling(false); // RDG1

    uint64_t unsupBlend = 0; // FEAT-RD2
    uint64_t approxAd = 0;   // FEAT-RD7
    uint64_t partialMask = 0; // FEAT-RD9
    static const bool s_2dDepth = []{ const char *e = std::getenv("PS2X_RD_2DZ"); return !(e && e[0] == '0'); }(); // FEAT-RD11
    uint64_t draws2dThisFrame = 0; // FEAT-RD8
    uint32_t drawn3dThisFrame = 0u; // FEAT-RD34
    bool palDrawnThisFrame = false; // FIX-RD86
    uint64_t ztested2dThisFrame = 0; // FEAT-RD11
    uint64_t zclears2dThisFrame = 0; // FEAT-RD20
    struct RdPassEnt { uint32_t fbp, xo, yo, sx0, sy0, sx1, sy1; float vox, voy; uint32_t n; }; // FEAT-RD22
    RdPassEnt passEnts[8]; // FEAT-RD22
    int passN = 0;         // FEAT-RD22
    static const bool s_passLog = []{ const char *e = std::getenv("PS2X_RD_PASSLOG"); return e && e[0] == '1'; }(); // FEAT-RD22
    static uint64_t s_scissorSkipped = 0; // FEAT-RD22
    static uint64_t s_noState3dTotal = 0, s_rtPass3dTotal = 0; // FEAT-RD23
    static uint32_t s_tailClearsDropped = 0u; // FEAT-RD36
    if ((!g_rdGpu.drawBatches.empty() || g_rdGpu.gpu2dVertCount > 0) && g_rdGpu.shaderReady) // FEAT-RD8 // FEAT-RD14 // RDG1
    {
        rdgfx::enableShader(); // RDG1
        int mode2d = 0; // FEAT-RD8
        if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
            rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
        const float fbSize[2] = {
            static_cast<float>(g_rdPresentW.load(std::memory_order_relaxed)),
            static_cast<float>(g_rdPresentH.load(std::memory_order_relaxed))
        };
        if (rdgfx::hasUniform(rdgfx::Uniform::FbSize)) // RDG1
            rdSetUniformVec2(rdgfx::Uniform::FbSize, fbSize); // RDG1
        const float vpZ[2] = { g_rdGpu.vpScaleZ, g_rdGpu.vpOffsetZ }; // FEAT-RD11
        if (rdgfx::hasUniform(rdgfx::Uniform::VpZ)) // RDG1
            rdSetUniformVec2(rdgfx::Uniform::VpZ, vpZ); // RDG1
        unsigned int curVao = 0u; // FEAT-RD14
        int prevBlend = -1; // FEAT-RD2b
        size_t n2d = (s_draw2d && g_rdGpu.gpu2dVertCount > 0 && g_rdGpu.vao2d != 0) ? g_rdGpu.local2dBatches.size() : 0; // FEAT-RD36
        size_t startIdx = 0; // FIX-RD88B
        static const bool s_nfTrimOn = []() { // FIX-RD88
            extern void ps2xLoadConfig();
            ps2xLoadConfig();
            const char *e = std::getenv("PS2X_RD_NFTRIM");
            return !e || e[0] != '0';
        }();
        static const bool s_nfStartOn = []() { // FIX-RD88B // FIX-RD88C
            extern void ps2xLoadConfig();
            ps2xLoadConfig();
            const char *e = std::getenv("PS2X_RD_NFSTART");
            return e && e[0] == '1'; // FIX-RD88C: opt-in (default 0)
        }();
        if (s_nfTrimOn && n2d > 0) // FIX-RD88
        {
            const float vpW = (fbSize[0] > 0.0f) ? fbSize[0] : 512.0f;
            const float vpH = (fbSize[1] > 0.0f) ? fbSize[1] : 448.0f;
            auto isFrameStartCopy = [&](const Rd2DBatch &b) -> bool {
                if (b.count == 0u)
                    return false;
                if (((b.prim >> 4) & 1ull) == 0ull || !ps2xRdIsFbFeedback(b.texSerial))
                    return false;
                const uint32_t dstFbp = static_cast<uint32_t>(b.frame & 0x1FFull);
                const uint32_t srcFbp = static_cast<uint32_t>(b.tex0 & 0x3FFFull) / 32u;
                if (dstFbp == srcFbp)
                    return false;
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size());
                for (size_t vi = b.first; vi < e2; ++vi)
                {
                    const RdVert &v = g_rdGpu.local2dScratch[vi];
                    x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
                    y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
                }
                if (x1 < x0 || y1 < y0)
                    return false;
                x0 = std::max(0.0f, std::min(x0, vpW)); x1 = std::max(0.0f, std::min(x1, vpW));
                y0 = std::max(0.0f, std::min(y0, vpH)); y1 = std::max(0.0f, std::min(y1, vpH));
                return (x1 - x0) * (y1 - y0) >= 0.90f * vpW * vpH;
            };
            size_t lastCopy = n2d;
            for (size_t k = n2d; k > 0; --k)
            {
                if (isFrameStartCopy(g_rdGpu.local2dBatches[k - 1]))
                {
                    lastCopy = k - 1;
                    break;
                }
            }
            if (lastCopy < n2d && (n2d - 1 - lastCopy) < 80u)
            {
                size_t prevCopy = n2d; // FIX-RD88B
                for (size_t k = lastCopy; k > 0; --k)
                {
                    if (isFrameStartCopy(g_rdGpu.local2dBatches[k - 1]))
                    {
                        prevCopy = k - 1;
                        break;
                    }
                }
                if (prevCopy < lastCopy)
                {
                    n2d = lastCopy;
                    ++s_nextFrameTrim;
                    if (s_nfStartOn && prevCopy > 0) // FIX-RD88B
                    {
                        startIdx = prevCopy;
                        s_nextFrameSkip += static_cast<uint32_t>(startIdx);
                    }
                }
            }
        }
        size_t i2d = startIdx; // FEAT-RD8 // FIX-RD88B
        bool rd2dReasonDump = false; // PROBE-RD62
        int rd2dReasonPrinted = 0;
        uint32_t min3dSerial = 0xFFFFFFFFu, max3dSerial = 0u;
        std::vector<size_t> deferred2d; // FIX-RD63
        if (g_rd2dReasonOn)
        {
            static int s_rd2dReasonPrints = 0;
            static std::chrono::steady_clock::time_point s_rd2dReasonLast{};
            static std::chrono::steady_clock::time_point s_rd2dReasonFlagLast{};
            static bool s_rd2dReasonGo = false;
            if (s_rd2dReasonPrints < 4)
            {
                const auto now2dR = std::chrono::steady_clock::now();
                if (s_rd2dReasonFlagLast.time_since_epoch().count() == 0 || now2dR - s_rd2dReasonFlagLast >= std::chrono::seconds(1))
                {
                    s_rd2dReasonFlagLast = now2dR;
                    if (std::FILE *f = std::fopen("logs\\gslog_go.flag", "rb"))
                    {
                        std::fclose(f);
                        s_rd2dReasonGo = true;
                    }
                    else
                    {
                        s_rd2dReasonGo = false;
                    }
                }
                if (s_rd2dReasonGo && (s_rd2dReasonLast.time_since_epoch().count() == 0 || now2dR - s_rd2dReasonLast >= std::chrono::seconds(2)))
                {
                    s_rd2dReasonLast = now2dR;
                    ++s_rd2dReasonPrints;
                    rd2dReasonDump = true;
                }
            }
        }
        auto log2dReason = [&](size_t idx, const Rd2DBatch &b, const char *fate) // PROBE-RD62 // PROBE-RD62B
        {
            const int cap = g_rd2dReasonBig ? 400 : 60; // PROBE-RD62B
            if (!rd2dReasonDump || rd2dReasonPrinted >= cap)
                return;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size());
            for (size_t k = b.first; k < e2; ++k)
            {
                const float *pp = g_rdGpu.local2dScratch[k].pos;
                x0 = std::min(x0, pp[0]); x1 = std::max(x1, pp[0]);
                y0 = std::min(y0, pp[1]); y1 = std::max(y1, pp[1]);
            }
            if (g_rd2dReasonBig) // PROBE-RD62B
            {
                const float screenArea = (fbSize[0] > 0.0f && fbSize[1] > 0.0f) ? (fbSize[0] * fbSize[1]) : 0.0f;
                const float cx0 = std::max(0.0f, std::min(x0, fbSize[0]));
                const float cx1 = std::max(0.0f, std::min(x1, fbSize[0]));
                const float cy0 = std::max(0.0f, std::min(y0, fbSize[1]));
                const float cy1 = std::max(0.0f, std::min(y1, fbSize[1]));
                const float bboxArea = (x1 >= x0 && y1 >= y0) ? (cx1 - cx0) * (cy1 - cy0) : 0.0f;
                const bool bigBox = (screenArea > 0.0f) && (bboxArea >= 0.15f * screenArea);
                const bool boringFate = (std::strcmp(fate, "deferred-late") == 0 || std::strcmp(fate, "merged") == 0);
                if (!bigBox && boringFate)
                    return;
            }
            ++rd2dReasonPrinted;
            const int bx0 = (x1 >= x0) ? static_cast<int>(x0) : 0;
            const int by0 = (y1 >= y0) ? static_cast<int>(y0) : 0;
            const int bx1 = (x1 >= x0) ? static_cast<int>(x1) : 0;
            const int by1 = (y1 >= y0) ? static_cast<int>(y1) : 0;
            const unsigned int zte = static_cast<unsigned int>((b.test >> 16) & 1ull); // PROBE-RD62B
            const unsigned int ztst = static_cast<unsigned int>((b.test >> 17) & 3ull);
            const unsigned int zmsk = static_cast<unsigned int>((b.zbuf >> 32) & 1ull);
            ps2xSafeLog("[rd2dr] i=%zu fbp=%x count=%u tex=%d abe=%d afterSerial=%u bbox=%d,%d-%d,%d tex0=%llx alpha=%llx frame=%llx test=%llx zbuf=%llx prim=%llx zte=%u ztst=%u zmsk=%u 3d=%x..%x fate=%s\n",
                        idx, static_cast<unsigned int>(b.frame & 0x1FFull), static_cast<unsigned int>(b.count),
                        static_cast<int>((b.prim >> 4) & 1ull), static_cast<int>((b.prim >> 6) & 1ull),
                        static_cast<unsigned int>(b.afterSerial), bx0, by0, bx1, by1,
                        static_cast<unsigned long long>(b.tex0), static_cast<unsigned long long>(b.alpha),
                        static_cast<unsigned long long>(b.frame), static_cast<unsigned long long>(b.test),
                        static_cast<unsigned long long>(b.zbuf), static_cast<unsigned long long>(b.prim),
                        zte, ztst, zmsk,
                        drawn3dThisFrame ? min3dSerial : 0u, drawn3dThisFrame ? max3dSerial : 0u, fate);
        };

        bool scisOn = false; // FEAT-RD22
        int scisRect[4] = {0, 0, 0, 0};
        auto applyScissor = [&](uint64_t packed, bool have) -> bool // FEAT-RD22: false = batch fully outside, skip
        {
            if (!isRdScissorVisible(packed, have, fbSize[0], fbSize[1])) // FEAT-RD34
                return false;
            bool on = false;
            int r[4] = {0, 0, 0, 0};
            if (have && packed != 0ull && fbSize[0] > 0.0f && fbSize[1] > 0.0f)
            {
                const float x0 = static_cast<float>(packed & 0x7FFull);
                const float x1 = static_cast<float>((packed >> 16) & 0x7FFull);
                const float y0 = static_cast<float>((packed >> 32) & 0x7FFull);
                const float y1 = static_cast<float>((packed >> 48) & 0x7FFull);
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
                if (boxClipped) // FIX-R7: intersect GS scissor with display box
                {
                    const int ix0 = std::max(r[0], vx);
                    const int ix1 = std::min(r[0] + r[2], vx + vw);
                    const int iy0 = std::max(r[1], vy);
                    const int iy1 = std::min(r[1] + r[3], vy + vh);
                    if (ix1 <= ix0 || iy1 <= iy0)
                        return false;
                    r[0] = ix0;
                    r[1] = iy0;
                    r[2] = ix1 - ix0;
                    r[3] = iy1 - iy0;
                }
                on = true;
            }
            if (on != scisOn || (on && std::memcmp(r, scisRect, sizeof(r)) != 0))
            {
                if (on)
                {
                    if (!scisOn)
                        rdgfx::setScissorTest(true); // RDG1
                    // FEAT-RD34b: always issue the rectangle
                    rdgfx::setScissorRect(r[0], r[1], r[2], r[3]); // RDG1
                    std::memcpy(scisRect, r, sizeof(r));
                }
                else
                {
                    if (boxClipped) // FIX-R7
                    {
                        rdgfx::setScissorTest(true);
                        rdgfx::setScissorRect(vx, vy, vw, vh);
                    }
                    else
                        rdgfx::setScissorTest(false); // RDG1
                }
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
        bool rdbDump = false; // PROBE-RD29
        if (g_rdBlendOn)
        {
            static std::chrono::steady_clock::time_point s_rdbLast{};
            const auto nowB = std::chrono::steady_clock::now();
            if (s_rdbLast.time_since_epoch().count() == 0 || nowB - s_rdbLast >= std::chrono::seconds(5))
            {
                s_rdbLast = nowB;
                rdbDump = true;
                g_rdbCensus.m.clear();
            }
        }
        auto rdb2d = [&](const Rd2DBatch &b, int reason, uint32_t cnt) // PROBE-RD29
        {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + cnt, g_rdGpu.local2dScratch.size());
            for (size_t k = b.first; k < e2; ++k)
            {
                const float *pp = g_rdGpu.local2dScratch[k].pos;
                x0 = std::min(x0, pp[0]); x1 = std::max(x1, pp[0]);
                y0 = std::min(y0, pp[1]); y1 = std::max(y1, pp[1]);
            }
            uint64_t ub = 0, aa = 0; float fa = -1.0f;
            int bl = computeRdBlend(b.prim, b.alpha, ub, aa, fa);
            bl = applyRdDate(b.test, b.prim, bl);
            g_rdbCensus.add(1, b.prim, b.alpha, b.frame, b.test, b.tex0, reason, cnt / 3u, x1 >= x0, x0, y0, x1, y1, bl);
        };
        auto shouldDefer2d = [&](const Rd2DBatch &b, bool *isDim) -> bool // FIX-RD63 // FIX-RD66
        {
            if (isDim)
                *isDim = false;
            if (!g_rd2dLateOn() || g_rdGpu.drawBatches.empty() || drawn3dThisFrame != 0u) // FIX-CFG2
                return false;
            if (((b.prim >> 4) & 1ull) == 0ull || ((b.prim >> 6) & 1ull) == 0ull)
                return false;
            const float vpW = (fbSize[0] > 0.0f) ? fbSize[0] : 512.0f;
            const float vpH = (fbSize[1] > 0.0f) ? fbSize[1] : 448.0f;
            if (isRdDimOverlay(b, vpW, vpH)) // FIX-RD65c
            {
                if (isDim)
                    *isDim = true;
                return true;
            }
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size());
            for (size_t vi = b.first; vi < e2; ++vi)
            {
                const RdVert &v = g_rdGpu.local2dScratch[vi];
                x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
                y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
            }
            if (x1 < x0 || y1 < y0)
                return true;
            x0 = std::max(0.0f, std::min(x0, vpW)); x1 = std::max(0.0f, std::min(x1, vpW));
            y0 = std::max(0.0f, std::min(y0, vpH)); y1 = std::max(0.0f, std::min(y1, vpH));
            if ((x1 - x0) * (y1 - y0) < 0.90f * vpW * vpH)
                return true;
            return false;
        };
        auto allow2dIdx = [&](size_t idx) -> bool { // PROBE-RD83 // FIX-RD88B
            if (idx < startIdx) // FIX-RD88B
                return false;
            if (g_rd2dMax > 0 && idx >= static_cast<size_t>(g_rd2dMax))
                return false;
            if (g_rd2dOnly >= 0 && idx != static_cast<size_t>(g_rd2dOnly))
                return false;
            return true;
        };
        auto emit2dBatch = [&](size_t idx, const Rd2DBatch &b0, uint32_t totalCount, size_t jEnd, bool forceNoZ, bool logReason) // FIX-RD63
        {
            if (!allow2dIdx(idx)) // PROBE-RD83
                return;
            const uint32_t ate = static_cast<uint32_t>(b0.test & 1ull);
            const uint32_t atst = ate ? static_cast<uint32_t>((b0.test >> 1) & 7ull) : 1u;
            const int afail = (ate && atst != 1u) ? static_cast<int>((b0.test >> 12) & 3ull) : 0; // FEAT-RD30
            const uint32_t zte = static_cast<uint32_t>((b0.test >> 16) & 1ull); // FEAT-RD11
            const uint32_t ztst = static_cast<uint32_t>((b0.test >> 17) & 3ull);
            const bool use2dZ = !forceNoZ && s_2dDepth && zte && (ztst == 2u || ztst == 3u) && (g_rdGpu.vpScaleZ != 0.0f);
            const bool zWrite2d = !forceNoZ && s_2dDepth && !((b0.zbuf >> 32) & 1ull) && (g_rdGpu.vpScaleZ != 0.0f); // FEAT-RD20
            const bool zAlways2d = zWrite2d && !use2dZ; // FEAT-RD20: ZTE=0 or ZTST=1 with Z writes -> ALWAYS + write
            const int wantMode2d = (use2dZ || zAlways2d) ? 2 : 1;
            if (wantMode2d != mode2d)
            {
                mode2d = wantMode2d;
                if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
                    rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
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
                rdgfx::setDepthFunc(zAlways2d ? rdgfx::DepthFunc::Always : rdgfx::DepthFunc::Lequal); // RDG1
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
                rdgfx::setColorMask((g_rdMaskCur & 1) != 0, (g_rdMaskCur & 2) != 0, (g_rdMaskCur & 4) != 0, false); // RDG1
            }
            if (blendState != prevBlend)
            {
                applyRdBlend(blendState, g_rdGpu.vao2d);
                prevBlend = blendState;
            }
            int atstVal = (atst == 0u) ? 1 : static_cast<int>(atst); // FEAT-RD30
            int arefVal = static_cast<int>((b0.test >> 4) & 0xFFull);
            if (rdgfx::hasUniform(rdgfx::Uniform::Atst)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Atst, atstVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Aref)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Aref, arefVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::FixAlpha)) // RDG1
                rdSetUniformFloat(rdgfx::Uniform::FixAlpha, fixAlpha); // RDG1

            int texVal = 0, tfxVal = 0, tccVal = 0;
            int palMode = 0; // FIX-RD82B
            int palFallback = 0; // FIX-RD84
            unsigned int texId = 0u;
            if ((b0.prim >> 4) & 1ull)
            {
                if (isRdFbTexDisplayBatch(b0)) // FIX-RD79
                {
                    texId = rdFbTexSelectId(b0.tex0); // FIX-RD85
                    ++s_fbTexUsed;
                    palMode = 2; // FIX-RD82J
                    ++s_fbTexAlphaFix; // FIX-RD82J
                    const uint32_t psm = static_cast<uint32_t>((b0.tex0 >> 20) & 0x3Full); // FIX-RD82B
                    if (texId != 0u && texId == s_prevTexId && (psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu)) // FIX-RD86
                        palDrawnThisFrame = true; // FIX-RD86
#ifdef _WIN32
                    if (rdFbTexOn() && rdFbTexPalOn() && psm == 27u) // FIX-RD82B // FIX-RD82G
                    {
                        if (rdFbTexPalFbOn()) // FIX-RD84B
                        {
                            palMode = 1;
                            palFallback = 1;
                            ++s_fbTexPalFb;
                        }
                        else
                        {
                        uint32_t palRgba[256];
                        if (rdgfx::isPaletteTextureSupported() && rdgfx::hasUniform(rdgfx::Uniform::PalMode) && rdgfx::hasUniform(rdgfx::Uniform::PalTex) && ps2xRdGetClutRgba256(b0.tex0, b0.texa, palRgba)) // RDG1
                        {
                            uint32_t maxRgb = 0u; // FIX-RD84
                            const uint32_t firstRgb = palRgba[0] & 0x00FFFFFFu; // FIX-RD84
                            bool rgbVaries = false; // FIX-RD84
                            for (int pi = 0; pi < 256; ++pi) // FIX-RD84
                            {
                                const uint32_t c = palRgba[pi];
                                const uint32_t r = c & 0xFFu;
                                const uint32_t g = (c >> 8) & 0xFFu;
                                const uint32_t b = (c >> 16) & 0xFFu;
                                if (r > maxRgb) maxRgb = r;
                                if (g > maxRgb) maxRgb = g;
                                if (b > maxRgb) maxRgb = b;
                                if ((c & 0x00FFFFFFu) != firstRgb)
                                    rgbVaries = true;
                            }
                            if (maxRgb < 40u || !rgbVaries) // FIX-RD84
                            {
                                palMode = 1;
                                palFallback = 1;
                                ++s_fbTexPalFb;
                            }
                            else
                            {
                            const bool palOk = rdgfx::uploadPaletteTexture(palRgba); // RDG1
                            g_rdc.bound = 0u;
                            if (palOk)
                            {
                                palMode = 1;
                                ++s_fbTexPal;
                            }
                            else
                            {
                                ++s_fbTexPalFail;
                            }
                            }
                        }
                        else
                        {
                            ++s_fbTexPalFail;
                        }
                        }
                    }
#endif
                }
                else if (const unsigned int snapTexId = findRdSnapTex(b0, true)) // FIX-RD81
                {
                    texId = snapTexId;
                }
                else
                {
                    ps2xRdBt2SetKind(1); // PROBE-BT2
                    texId = ps2xRdGetTextureForBatch(b0.texSerial, b0.tex0, b0.texa);
                }
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
            if (rdgfx::hasUniform(rdgfx::Uniform::Tex)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tex, texVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Tfx)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tfx, tfxVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Tcc)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tcc, tccVal); // RDG1

            rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c: no fog on 2D batches
            if (palMode != 0 && rdgfx::hasUniform(rdgfx::Uniform::PalMode)) // FIX-RD82I // RDG1
            {
                static bool s_palTexSet = false;
                if (!s_palTexSet && rdgfx::hasUniform(rdgfx::Uniform::PalTex)) // RDG1
                {
                    rdSetUniformInt(rdgfx::Uniform::PalTex, 1); // RDG1
                    s_palTexSet = true;
                }
                rdSetUniformInt(rdgfx::Uniform::PalMode, palMode); // RDG1
                if (palMode == 1 && rdgfx::hasUniform(rdgfx::Uniform::PalFallback)) // FIX-RD84 // RDG1
                    rdSetUniformInt(rdgfx::Uniform::PalFallback, palFallback); // RDG1
            }

            if (rdbDump) rdb2d(b0, 0, totalCount); // PROBE-RD29
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
            if (afail != 0) // FEAT-RD30
            {
                const int sMask = g_rdMaskCur;
                const int sDepth = g_rdc.depthMask;
                const bool zOff = ((b0.zbuf >> 32) & 1ull) == 0ull;
                const int zero = 0, one = 1;
                if (atst == 0u)
                {
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rdgfx::drawArrays(rdgfx::PrimitiveTopology::Triangles, static_cast<int>(b0.first), static_cast<int>(totalCount)); // RDG1
                    ++g_rdAfailNever;
                }
                else
                {
                    rdgfx::drawArrays(rdgfx::PrimitiveTopology::Triangles, static_cast<int>(b0.first), static_cast<int>(totalCount)); // RDG1
                    if (rdgfx::hasUniform(rdgfx::Uniform::AtInvert)) // RDG1
                        rdSetUniformInt(rdgfx::Uniform::AtInvert, one); // RDG1
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rdgfx::drawArrays(rdgfx::PrimitiveTopology::Triangles, static_cast<int>(b0.first), static_cast<int>(totalCount)); // RDG1
                    if (rdgfx::hasUniform(rdgfx::Uniform::AtInvert)) // RDG1
                        rdSetUniformInt(rdgfx::Uniform::AtInvert, zero); // RDG1
                    ++g_rdAfail2Pass;
                }
                rdSetMaskPacked(sMask);
                rdDepthMask(sDepth != 0);
            }
            else
                rdgfx::drawArrays(rdgfx::PrimitiveTopology::Triangles, static_cast<int>(b0.first), static_cast<int>(totalCount)); // RDG1
            if (palMode != 0) // FIX-RD82B // FIX-RD82G // FIX-RD82I
            {
                if (rdgfx::hasUniform(rdgfx::Uniform::PalMode)) // RDG1
                    rdSetUniformInt(rdgfx::Uniform::PalMode, 0); // RDG1
                if (palMode == 1 && palFallback != 0 && rdgfx::hasUniform(rdgfx::Uniform::PalFallback)) // FIX-RD84 // RDG1
                    rdSetUniformInt(rdgfx::Uniform::PalFallback, 0); // RDG1
#ifdef _WIN32
                rdgfx::unbindPaletteTexture(); // RDG1
#endif
                g_rdc.bound = 0u;
            }
            ++draws2dThisFrame;
            if (s_abeFilter.active) // PROBE-E3a
                ++s_abeDrawn2dSec;
            if (s_texFilter.active) // PROBE-E3d
                ++s_texDrawn2dSec;
            if (logReason && rd2dReasonDump) // PROBE-RD62
            {
                const bool texUnres = (((b0.prim >> 4) & 1ull) != 0ull) && (texId == 0u);
                log2dReason(idx, b0, texUnres ? "drawn-texUnresolved" : "drawn");
                for (size_t m = idx + 1; m < jEnd; ++m)
                    log2dReason(m, g_rdGpu.local2dBatches[m], "merged");
            }
            { // PROBE-RD35
                Rd35Rec &r = g_rd35.last[g_rd35.n2d % 3u];
                r.prim = b0.prim; r.alpha = b0.alpha; r.frame = b0.frame; r.tex0 = b0.tex0;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                const size_t e = std::min<size_t>(g_rdGpu.local2dScratch.size(), static_cast<size_t>(b0.first) + b0.count);
                for (size_t vi = b0.first; vi < e; ++vi)
                {
                    const RdVert &v = g_rdGpu.local2dScratch[vi];
                    x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
                    y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
                }
                r.bb[0] = x0; r.bb[1] = y0; r.bb[2] = x1; r.bb[3] = y1;
                ++g_rd35.n2d;
            }
            if (use2dZ) // FEAT-RD11
                ++ztested2dThisFrame;
            if (zWrite2d) // FEAT-RD20
                ++zclears2dThisFrame;
        };
        auto draw2dUntil = [&](uint32_t maxSerial, bool drawAll) // FEAT-RD8
        {
            const uint32_t maxS = maxSerial & 0x3FFFFFFFu; // FEAT-RD10
            if (i2d >= n2d || (!drawAll && (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) >= maxS))
                return;
            Pace2Scope pace2B4Scope(g_pace2Rd.b4, pace2On); // PROBE-PACE2
            mode2d = 1;
            if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
            rdDepthTest(false);
            rdDepthMask(false);
            rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c
            rdgfx::bindVertexArray(g_rdGpu.vao2d); // RDG1

            while (i2d < n2d && (drawAll || (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) < maxS))
            {
                const Rd2DBatch &b0 = g_rdGpu.local2dBatches[i2d];
                if (!allow2dIdx(i2d)) // PROBE-RD83
                {
                    ++i2d;
                    continue;
                }
                // PROBE-E3a: ABE debug filter for 2D batches
                if (s_abeFilter.active)
                {
                    const bool abe = ((b0.prim >> 6) & 1ull) != 0ull;
                    const uint64_t maskedAlpha = b0.alpha & 0x000000FF000000FFull;
                    const bool match = abe && s_abeFilter.contains(maskedAlpha);
                    const bool draw = s_abeFilter.inv ? !match : match;
                    if (!draw)
                    {
                        ++s_abeSkippedSec;
                        ++i2d;
                        continue;
                    }
                }
                // PROBE-E3d: TEX debug filter for 2D batches
                if (s_texFilter.active)
                {
                    const bool tme = ((b0.prim >> 4) & 1ull) != 0ull;
                    const bool match = tme && s_texFilter.contains(b0.tex0);
                    const bool draw = s_texFilter.inv ? !match : match;
                    if (!draw)
                    {
                        ++s_texSkippedSec;
                        ++i2d;
                        continue;
                    }
                }
                if (b0.count == 0u)
                {
                    log2dReason(i2d, b0, "skip-count0"); // PROBE-RD62
                    ++i2d;
                    continue;
                }
                if (((b0.prim >> 4) & 1ull) && ps2xRdIsFbFeedback(b0.texSerial) && !isRdFbTexDisplayBatch(b0)) // FEAT-RD28b // FIX-RD79
                {
                    if (rdbDump) rdb2d(b0, 1, b0.count); // PROBE-RD29
                    log2dReason(i2d, b0, "skip-fbFeedback"); // PROBE-RD62
                    ++s_fbFeedbackSkipped;
                    ++i2d;
                    continue;
                }
                if (isRdCopyBufSkip(b0)) // FIX-RD87
                {
                    log2dReason(i2d, b0, "skip-copybuf"); // PROBE-RD62
                    ++s_fbCopySkip;
                    ++i2d;
                    continue;
                }
                const uint32_t ate = static_cast<uint32_t>(b0.test & 1ull);
                const uint32_t atst = ate ? static_cast<uint32_t>((b0.test >> 1) & 7ull) : 1u;
                const int afail = (ate && atst != 1u) ? static_cast<int>((b0.test >> 12) & 3ull) : 0; // FEAT-RD30
                if (atst == 0u && afail == 0)
                {
                    if (rdbDump) rdb2d(b0, 4, b0.count); // PROBE-RD29
                    log2dReason(i2d, b0, "skip-atstNever"); // PROBE-RD62
                    ++i2d;
                    continue;
                }
                const uint32_t zte = static_cast<uint32_t>((b0.test >> 16) & 1ull); // FEAT-RD11
                const uint32_t ztst = static_cast<uint32_t>((b0.test >> 17) & 3ull);
                if (zte && ztst == 0u)
                {
                    if (rdbDump) rdb2d(b0, 5, b0.count); // PROBE-RD29
                    log2dReason(i2d, b0, "skip-ztstNever"); // PROBE-RD62
                    ++i2d;
                    continue;
                }
                if (!applyScissor(b0.scissor, true)) // FEAT-RD22
                {
                    if (rdbDump) rdb2d(b0, 7, b0.count); // PROBE-RD29
                    log2dReason(i2d, b0, "skip-scissor"); // PROBE-RD62
                    ++s_scissorSkipped;
                    ++i2d;
                    continue;
                }
                bool deferDim = false; // FIX-RD66
                if (shouldDefer2d(b0, &deferDim)) // FIX-RD63
                {
                    log2dReason(i2d, b0, deferDim ? "deferred-late-dim" : "deferred-late");
                    deferred2d.push_back(i2d);
                    ++i2d;
                    continue;
                }
                auto resolve2dTexId = [&](const Rd2DBatch &b) -> unsigned int { // FIX-RD79
                    if (((b.prim >> 4) & 1ull) == 0ull)
                        return 0u;
                    if (isRdFbTexDisplayBatch(b))
                        return rdFbTexSelectId(b.tex0); // FIX-RD85
                    if (const unsigned int snapTexId = findRdSnapTex(b, false)) // FIX-RD81
                        return snapTexId;
                    ps2xRdBt2SetKind(1); // PROBE-BT2
                    return ps2xRdGetTextureForBatch(b.texSerial, b.tex0, b.texa);
                };
                const unsigned int texId0 = resolve2dTexId(b0); // FIX-RD79
                uint32_t totalCount = b0.count;
                size_t j = i2d + 1;
                while (j < n2d && (drawAll || (g_rdGpu.local2dBatches[j].afterSerial & 0x3FFFFFFFu) < maxS)) // FEAT-RD10
                {
                    if (!allow2dIdx(j)) // PROBE-RD83
                        break;
                    const Rd2DBatch &bn = g_rdGpu.local2dBatches[j];
                    if (shouldDefer2d(bn, nullptr)) // FIX-RD63
                        break;
                    if (bn.first != b0.first + totalCount ||
                        bn.prim != b0.prim || bn.tex0 != b0.tex0 || bn.texa != b0.texa ||
                        bn.clamp != b0.clamp || bn.tex1 != b0.tex1 || bn.alpha != b0.alpha ||
                        bn.test != b0.test || bn.frame != b0.frame || bn.zbuf != b0.zbuf || bn.scissor != b0.scissor) // FEAT-RD22
                        break;
                    if (((bn.prim >> 4) & 1ull) && ps2xRdIsFbFeedback(bn.texSerial) && !isRdFbTexDisplayBatch(bn)) // FIX-RD79
                        break;
                    if (texId0 != 0u && resolve2dTexId(bn) != texId0) // PROBE-BT2 // FIX-RD79
                        break;
                    totalCount += bn.count;
                    ++j;
                }
                emit2dBatch(i2d, b0, totalCount, j, false, true);
                i2d = j;
            }
#ifdef _WIN32
            if (g_rdAlwaysFunc) // FEAT-RD20: restore LEQUAL
            {
                rdgfx::setDepthFunc(rdgfx::DepthFunc::Lequal); // RDG1
                g_rdAlwaysFunc = false;
            }
#endif

            mode2d = 0;
            if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
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
        bool reasonDump = false; // PROBE-RD59
        int reasonPrinted = 0;
        if (g_rdReasonOn)
        {
            static int s_reasonPrints = 0;
            static std::chrono::steady_clock::time_point s_reasonLast{};
            const auto nowR = std::chrono::steady_clock::now();
            if (s_reasonPrints < 4 && (s_reasonLast.time_since_epoch().count() == 0 || nowR - s_reasonLast >= std::chrono::seconds(2)))
            {
                s_reasonLast = nowR;
                ++s_reasonPrints;
                reasonDump = true;
            }
        }
        float prevZBias = 0.0f; // PROBE-RD59
        const int64_t pace2B4Before3d = pace2On ? g_pace2Rd.b4 : 0; // PROBE-PACE2
        { // PROBE-PACE2
            Pace2Scope pace2B3Scope(g_pace2Rd.b3, pace2On); // PROBE-PACE2
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
            auto rdb3d = [&](int reason) // PROBE-RD29
            {
                constexpr uint64_t kM = (1ull << 6) | (1ull << 4);
                const uint64_t cPrim = gsOk ? ((batch.prim & ~kM) | (gsPrim & kM)) : batch.prim;
                const uint64_t cAlpha = gsOk ? gsAlpha : batch.alpha;
                const uint64_t cTex0 = gsOk ? gsTex0 : batch.tex0;
                const std::vector<RdVert> &pv = g_rd24Verts[db.slotIdx];
                const std::vector<uint16_t> &pi = g_rd24Idx[db.slotIdx];
                uint64_t xoB = 0ull;
                ps2xRdLookupXyoff(batch.kickSerial, xoB);
                const float offx = static_cast<float>(xoB & 0xFFFFull);
                const float offy = static_cast<float>((xoB >> 32) & 0xFFFFull);
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
                uint64_t ub = 0, aa = 0; float fa = -1.0f;
                int bl = computeRdBlend(cPrim, cAlpha, ub, aa, fa);
                bl = applyRdDate(test, cPrim, bl);
                g_rdbCensus.add(0, cPrim, cAlpha, frame, test, cTex0, reason, batch.count / 3u, x1 >= x0, x0, y0, x1, y1, bl);
            };
            auto rdReasonLog = [&](int reason) // PROBE-RD59
            {
                constexpr uint64_t kM = (1ull << 6) | (1ull << 4);
                const uint64_t cPrim = gsOk ? ((batch.prim & ~kM) | (gsPrim & kM)) : batch.prim;
                if (((cPrim >> 4) & 1ull) == 0ull)
                    return;
                const uint64_t cTex0 = gsOk ? gsTex0 : batch.tex0;
                const std::vector<RdVert> &pv = g_rd24Verts[db.slotIdx];
                const std::vector<uint16_t> &pi = g_rd24Idx[db.slotIdx];
                uint64_t xoR = 0ull;
                ps2xRdLookupXyoff(batch.kickSerial, xoR);
                const float offx = static_cast<float>(xoR & 0xFFFFull);
                const float offy = static_cast<float>((xoR >> 32) & 0xFFFFull);
                float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                float zmin = 1e30f, zmax = -1e30f;
                const size_t endI = static_cast<size_t>(batch.first) + batch.count;
                if (endI <= pi.size() && batch.vpS[0] != 0.0f && batch.vpS[1] != 0.0f)
                {
                    for (size_t k = batch.first; k < endI; ++k)
                    {
                        const uint32_t vi = static_cast<uint32_t>(pi[k]) + batch.baseVertex;
                        if (vi >= pv.size())
                            continue;
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
                    }
                }
                if (bx1 < bx0 || by1 < by0)
                    return;
                bx0 = std::max(0.0f, std::min(bx0, fbSize[0])); bx1 = std::max(0.0f, std::min(bx1, fbSize[0]));
                by0 = std::max(0.0f, std::min(by0, fbSize[1])); by1 = std::max(0.0f, std::min(by1, fbSize[1]));
                if ((bx1 - bx0) < 100.0f)
                    return;
                ++reasonPrinted;
                const char *rname = RdBlendCensus::reasonName(reason);
                ps2xSafeLog("[rdreason] serial=%x bbox=%d,%d-%d,%d tex0=%llx test=%x zmin=%.5f zmax=%.5f drawn=%d reason=%s%s\n",
                            batch.kickSerial,
                            static_cast<int>(bx0), static_cast<int>(by0), static_cast<int>(bx1), static_cast<int>(by1),
                            static_cast<unsigned long long>(cTex0), static_cast<unsigned int>(test),
                            (zmin <= zmax) ? zmin : 0.0f, (zmin <= zmax) ? zmax : 0.0f,
                            (reason == 0) ? 1 : 0,
                            (reason == 0) ? "" : "skipped-",
                            (reason == 0) ? "none" : rname);
            };
            if (ps2xRdHasDisplayFbp() && !ps2xRdIsDisplayFbp(static_cast<uint32_t>(frame & 0x1FFull))) // FEAT-RD19
            {
                if (rdbDump) rdb3d(2); // PROBE-RD29
                if (reasonDump && reasonPrinted < 30) rdReasonLog(2); // PROBE-RD59
                ++g_rdGpu.rtSkipped3d;
                continue;
            }
            { // FEAT-RD23 // FEAT-RD33: only batches with GS-thread state are drawn (S already encodes main pass)
                uint64_t xoMp = 0ull;
                if (!gsOk || !ps2xRdLookupXyoff(batch.kickSerial, xoMp))
                {
                    if (rdbDump) rdb3d(3); // PROBE-RD29
                    if (reasonDump && reasonPrinted < 30) rdReasonLog(3); // PROBE-RD59
                    ++s_noState3dTotal;
                    continue;
                }
            }
            const uint64_t alpha = gsOk ? gsAlpha : batch.alpha;
            constexpr uint64_t kPrimAbeTmeMask = (1ull << 6) | (1ull << 4);
            const uint64_t prim = gsOk ? ((batch.prim & ~kPrimAbeTmeMask) | (gsPrim & kPrimAbeTmeMask)) : batch.prim;
            // PROBE-E3a: ABE debug filter for 3D batches
            if (s_abeFilter.active)
            {
                const bool abe = ((prim >> 6) & 1ull) != 0ull;
                const uint64_t maskedAlpha = alpha & 0x000000FF000000FFull;
                const bool match = abe && s_abeFilter.contains(maskedAlpha);
                const bool draw = s_abeFilter.inv ? !match : match;
                if (!draw)
                {
                    ++s_abeSkippedSec;
                    continue;
                }
            }
            const uint64_t tex0 = gsOk ? gsTex0 : batch.tex0; // PROBE-E3d
            // PROBE-E3d: TEX debug filter for 3D batches
            if (s_texFilter.active)
            {
                const bool tme = ((prim >> 4) & 1ull) != 0ull;
                const bool match = tme && s_texFilter.contains(tex0);
                const bool draw = s_texFilter.inv ? !match : match;
                if (!draw)
                {
                    ++s_texSkippedSec;
                    continue;
                }
            }
            if (((prim >> 4) & 1ull) && ps2xRdIsFbFeedback(batch.kickSerial)) // FEAT-RD28b
            {
                if (rdbDump) rdb3d(1); // PROBE-RD29
                if (reasonDump && reasonPrinted < 30) rdReasonLog(1); // PROBE-RD59
                ++s_fbFeedbackSkipped;
                continue;
            }
            const uint64_t tex1 = gsOk ? gsTex1 : batch.tex1;
            const uint64_t clamp = gsOk ? gsClamp : batch.clamp;
            const uint64_t texa = gsOk ? gsTexa : batch.texa;
            (void)gsFba;
            const uint32_t ate = static_cast<uint32_t>(test & 1ull);
            const uint32_t atst = ate ? static_cast<uint32_t>((test >> 1) & 7ull) : 1u;
            const int afail = (ate && atst != 1u) ? static_cast<int>((test >> 12) & 3ull) : 0; // FEAT-RD30
            if (atst == 0u && afail == 0)
            {
                if (rdbDump) rdb3d(4); // PROBE-RD29
                if (reasonDump && reasonPrinted < 30) rdReasonLog(4); // PROBE-RD59
                continue;
            }
            const uint32_t zte = static_cast<uint32_t>((test >> 16) & 1ull);
            const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
            if (zte && ztst == 0u)
            {
                if (rdbDump) rdb3d(5); // PROBE-RD29
                if (reasonDump && reasonPrinted < 30) rdReasonLog(5); // PROBE-RD59
                continue;
            }
            if (g_rdNoZTest || !zte || ztst == 1u) // PROBE-RD59
                rdDepthTest(false);
            else
                rdDepthTest(true);
            if ((zbuf >> 32) & 1ull)
                rdDepthMask(false);
            else
                rdDepthMask(true);
            if (g_rdZBias != 0.0f && rdgfx::hasUniform(rdgfx::Uniform::ZBias)) // PROBE-RD59 // RDG1
            {
                const float zb = (ztst == 2u) ? g_rdZBias : 0.0f;
                if (zb != prevZBias)
                {
                    rdSetUniformFloat(rdgfx::Uniform::ZBias, zb); // RDG1
                    prevZBias = zb;
                }
            }

            // FEAT-RD7: blendState 0 = off, else 1 + sfk*64 + dfk*16 + eqk (factor kinds: 0 ZERO, 1 ONE, 2 C, 3 1-C)
            applyRdFbmsk(frame, false, partialMask); // FEAT-RD9
            float fixAlpha = -1.0f;
            int blendState = computeRdBlend(prim, alpha, unsupBlend, approxAd, fixAlpha); // FEAT-RD8
            blendState = applyRdDate(test, prim, blendState); // FEAT-RD25
            if (blendState >= 1 + 6 * 128 && g_rdMaskCur != (g_rdMaskCur & 7)) // FEAT-RD25: no alpha write
            {
                g_rdMaskCur &= 7;
                rdgfx::setColorMask((g_rdMaskCur & 1) != 0, (g_rdMaskCur & 2) != 0, (g_rdMaskCur & 4) != 0, false); // RDG1
            }
            if (blendState != prevBlend) // FEAT-RD2b: blend calls flush raylib batch and unbind shader/VAO
            {
                applyRdBlend(blendState, slot.vao); // FEAT-RD8 // FEAT-RD14
                prevBlend = blendState;
                curVao = slot.vao;
            }
            else if (curVao != slot.vao) // FEAT-RD14
            {
                rdgfx::bindVertexArray(slot.vao); // RDG1
                curVao = slot.vao;
            }
            if (batch.baseVertex != slot.curBaseVertex) // FEAT-RD14: >65536 vertex split path
            {
                rdgfx::bindVertexBuffer(slot.vbo); // RDG1
                const int byteOff = static_cast<int>(static_cast<size_t>(batch.baseVertex) * sizeof(RdVert));
                rdgfx::setVertexAttribute(0, 4, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), byteOff + 0); // RDG1
                rdgfx::setVertexAttribute(1, 4, rdgfx::VertexAttrFormat::UnsignedByte, true, sizeof(RdVert), byteOff + 16); // RDG1
                rdgfx::setVertexAttribute(2, 2, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), byteOff + 20); // RDG1
                rdgfx::setVertexAttribute(3, 1, rdgfx::VertexAttrFormat::Float, false, sizeof(RdVert), byteOff + 28); // FIX-E1b
                rdgfx::bindVertexBuffer(0u); // RDG1
                slot.curBaseVertex = batch.baseVertex;
            }
            int atstVal = (atst == 0u) ? 1 : static_cast<int>(atst); // FEAT-RD30: NEVER+AFAIL = single pass without discard
            int arefVal = static_cast<int>((test >> 4) & 0xFFull);
            if (rdgfx::hasUniform(rdgfx::Uniform::Atst)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Atst, atstVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Aref)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Aref, arefVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::FixAlpha)) // FEAT-RD7 // RDG1
                rdSetUniformFloat(rdgfx::Uniform::FixAlpha, fixAlpha); // RDG1

            { // FEAT-RD22
                uint64_t sc = 0ull;
                const bool scOk = ps2xRdLookupScissor(batch.kickSerial, sc);
                if (!applyScissor(sc, scOk))
                {
                    if (rdbDump) rdb3d(7); // PROBE-RD29
                    if (reasonDump && reasonPrinted < 30) rdReasonLog(7); // PROBE-RD59
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
            if (rdgfx::hasUniform(rdgfx::Uniform::VpA) && rdgfx::hasUniform(rdgfx::Uniform::VpB)) // FEAT-RD21 // RDG1
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
                    rdgfx::setUniformVec4(rdgfx::Uniform::VpA, vpA); // RDG1
                    rdgfx::setUniformVec4(rdgfx::Uniform::VpB, vpB); // RDG1
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
                ps2xRdBt2SetKind(0); // PROBE-BT2
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
            if (rdgfx::hasUniform(rdgfx::Uniform::Tex)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tex, texVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Tfx)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tfx, tfxVal); // RDG1
            if (rdgfx::hasUniform(rdgfx::Uniform::Tcc)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Tcc, tccVal); // RDG1

            const uint32_t fge = static_cast<uint32_t>((batch.prim >> 5) & 1ull); // FIX-E1c
            float minFog = 1e30f; // FIX-E1d
            float maxFog = -1e30f; // FIX-E1d
            float meanFog = 0.0f; // FIX-E1d
            uint32_t sampleCount = 0; // FIX-E1d

            if (fge) // FIX-E1d
            {
                const std::vector<RdVert> &pv = !g_rd24Verts[db.slotIdx].empty() ? g_rd24Verts[db.slotIdx] : g_rdGpu.pendingVerts[db.slotIdx];
                const std::vector<uint16_t> &pi = g_rd24Idx[db.slotIdx];
                const std::vector<uint32_t> &pind = g_rdGpu.pendingIndices[db.slotIdx];
                const size_t endI = static_cast<size_t>(batch.first) + batch.count;
                double sumFog = 0.0;

                if (g_rdFogLog)
                {
                    if (!pi.empty() && endI <= pi.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            const uint32_t vi = static_cast<uint32_t>(pi[k]) + batch.baseVertex;
                            if (vi < pv.size())
                            {
                                const float f = pv[vi].fog;
                                if (f < minFog) minFog = f;
                                if (f > maxFog) maxFog = f;
                                sumFog += f;
                                ++sampleCount;
                            }
                        }
                    }
                    else if (!pind.empty() && endI <= pind.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            const uint32_t vi = pind[k] + batch.baseVertex;
                            if (vi < pv.size())
                            {
                                const float f = pv[vi].fog;
                                if (f < minFog) minFog = f;
                                if (f > maxFog) maxFog = f;
                                sumFog += f;
                                ++sampleCount;
                            }
                        }
                    }
                    else if (endI <= pv.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            const float f = pv[k].fog;
                            if (f < minFog) minFog = f;
                            if (f > maxFog) maxFog = f;
                            sumFog += f;
                            ++sampleCount;
                        }
                    }
                    if (sampleCount > 0)
                        meanFog = static_cast<float>(sumFog / sampleCount);
                }
                else if (!rdFogZeroEnvEnabled())
                {
                    if (!pi.empty() && endI <= pi.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            const uint32_t vi = static_cast<uint32_t>(pi[k]) + batch.baseVertex;
                            if (vi < pv.size() && pv[vi].fog > 0.0f)
                            {
                                maxFog = pv[vi].fog;
                                break;
                            }
                        }
                    }
                    else if (!pind.empty() && endI <= pind.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            const uint32_t vi = pind[k] + batch.baseVertex;
                            if (vi < pv.size() && pv[vi].fog > 0.0f)
                            {
                                maxFog = pv[vi].fog;
                                break;
                            }
                        }
                    }
                    else if (endI <= pv.size())
                    {
                        for (size_t k = batch.first; k < endI; ++k)
                        {
                            if (pv[k].fog > 0.0f)
                            {
                                maxFog = pv[k].fog;
                                break;
                            }
                        }
                    }
                }
            }

            if (rdFogEnvEnabled() && fge && (maxFog > 0.0f || rdFogZeroEnvEnabled())) // FIX-E1d
            {
                const float r = static_cast<float>(batch.fogcol & 0xFFu) / 255.0f;
                const float g = static_cast<float>((batch.fogcol >> 8) & 0xFFu) / 255.0f;
                const float b = static_cast<float>((batch.fogcol >> 16) & 0xFFu) / 255.0f;
                rdSetFog(r, g, b, 1.0f);
            }
            else
            {
                rdSetFog(0.0f, 0.0f, 0.0f, 0.0f);
            }

            if (g_rdFogLog) // PROBE-E1c
            {
                struct FogTopBatch {
                    uint32_t idx = 0;
                    uint32_t verts = 0;
                    float minFog = 0.0f;
                    float maxFog = 0.0f;
                    float meanFog = 0.0f;
                    uint64_t prim = 0;
                    uint32_t tex0low = 0;
                };
                static uint32_t s_fogBatches = 0;
                static uint32_t s_fogAllZero = 0;
                static uint32_t s_fogAllOne = 0;
                static uint32_t s_fogVarying = 0;
                static FogTopBatch s_fogTop[6]{};
                static uint32_t s_fogTopCount = 0;
                static std::chrono::steady_clock::time_point s_fogLastTime{};

                if (fge && sampleCount > 0) // FIX-E1d
                {
                    ++s_fogBatches;
                    if (maxFog <= 0.0f)
                        ++s_fogAllZero;
                    if (minFog >= 1.0f)
                        ++s_fogAllOne;
                    if (minFog < maxFog)
                        ++s_fogVarying;

                    FogTopBatch cur;
                    cur.idx = static_cast<uint32_t>(&db - g_rdGpu.drawBatches.data());
                    cur.verts = batch.count;
                    cur.minFog = minFog;
                    cur.maxFog = maxFog;
                    cur.meanFog = meanFog;
                    cur.prim = prim;
                    cur.tex0low = static_cast<uint32_t>(tex0 & 0xFFFFFFFFull);

                    if (s_fogTopCount < 6)
                    {
                        s_fogTop[s_fogTopCount++] = cur;
                        std::sort(s_fogTop, s_fogTop + s_fogTopCount, [](const FogTopBatch &a, const FogTopBatch &b) {
                            return a.verts > b.verts;
                        });
                    }
                    else if (cur.verts > s_fogTop[5].verts)
                    {
                        s_fogTop[5] = cur;
                        std::sort(s_fogTop, s_fogTop + 6, [](const FogTopBatch &a, const FogTopBatch &b) {
                            return a.verts > b.verts;
                        });
                    }
                }

                const auto nowFog = std::chrono::steady_clock::now();
                if (s_fogLastTime.time_since_epoch().count() == 0)
                {
                    s_fogLastTime = nowFog;
                }
                else if (nowFog - s_fogLastTime >= std::chrono::seconds(1))
                {
                    char topBuf[512] = "";
                    int topLen = 0;
                    for (uint32_t i = 0; i < s_fogTopCount; ++i)
                    {
                        const FogTopBatch &tb = s_fogTop[i];
                        topLen += std::snprintf(topBuf + topLen, sizeof(topBuf) - topLen,
                                                "%s%u:%u:%.2f/%.2f/%.2f:%llx:%x",
                                                (i == 0 ? "" : ", "),
                                                tb.idx, tb.verts, tb.minFog, tb.maxFog, tb.meanFog,
                                                static_cast<unsigned long long>(tb.prim), tb.tex0low);
                    }
                    ps2xSafeLog("[foglog] fgeBatches=%u allZero=%u allOne=%u varying=%u top=[%s]\n",
                                s_fogBatches, s_fogAllZero, s_fogAllOne, s_fogVarying, topBuf);
                    s_fogBatches = 0;
                    s_fogAllZero = 0;
                    s_fogAllOne = 0;
                    s_fogVarying = 0;
                    s_fogTopCount = 0;
                    s_fogLastTime = nowFog;
                }
            }

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
            if (rdbDump) rdb3d(0); // PROBE-RD29
            if (reasonDump && reasonPrinted < 30) rdReasonLog(0); // PROBE-RD59
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
            ++drawn3dThisFrame; // FEAT-RD34
            if (s_abeFilter.active) // PROBE-E3a
                ++s_abeDrawn3dSec;
            if (s_texFilter.active) // PROBE-E3d
                ++s_texDrawn3dSec;
            if (batch.kickSerial < min3dSerial) min3dSerial = batch.kickSerial; // PROBE-RD62
            if (batch.kickSerial > max3dSerial) max3dSerial = batch.kickSerial; // PROBE-RD62
            if (afail != 0) // FEAT-RD30
            {
                const int sMask = g_rdMaskCur;
                const int sDepth = g_rdc.depthMask;
                const bool zOff = ((zbuf >> 32) & 1ull) == 0ull;
                const int zero = 0, one = 1;
                if (atst == 0u)
                {
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rdDrawElems(batch.first, batch.count); // FIX-RD90
                    ++g_rdAfailNever;
                }
                else
                {
                    rdDrawElems(batch.first, batch.count); // FIX-RD90
                    if (rdgfx::hasUniform(rdgfx::Uniform::AtInvert)) // RDG1
                        rdSetUniformInt(rdgfx::Uniform::AtInvert, one); // RDG1
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rdDrawElems(batch.first, batch.count); // FIX-RD90
                    if (rdgfx::hasUniform(rdgfx::Uniform::AtInvert)) // RDG1
                        rdSetUniformInt(rdgfx::Uniform::AtInvert, zero); // RDG1
                    ++g_rdAfail2Pass;
                }
                rdSetMaskPacked(sMask);
                rdDepthMask(sDepth != 0);
            }
            else
                rdDrawElems(batch.first, batch.count); // FEAT-RD14 // FIX-RD90
            }
        }
        if (pace2On) // PROBE-PACE2
            g_pace2Rd.b3 -= (g_pace2Rd.b4 - pace2B4Before3d);
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
        { // FEAT-RD36: drop next frame's full-screen clear from the 2D tail
            const float vpW = (fbSize[0] > 0.0f) ? fbSize[0] : 512.0f;
            const float vpH = (fbSize[1] > 0.0f) ? fbSize[1] : 448.0f;
            for (size_t k = i2d; k < n2d; ++k)
            {
                const Rd2DBatch &b = g_rdGpu.local2dBatches[k];
                if (b.count == 0u)
                    continue;
                if (((b.prim >> 4) & 1ull) != 0ull)
                    continue;
                const uint64_t ptype = b.prim & 7ull;
                if (ptype != 6ull && ptype != 3ull && ptype != 4ull && ptype != 5ull)
                    continue;
                if (((b.frame >> 32) & 0xFFFFFFull) != 0ull)
                    continue;
                const bool abeOff = ((b.prim >> 6) & 1ull) == 0ull;
                const bool alphaReplace = (b.alpha & 0xFFull) == 0x44ull;
                if (!abeOff && !alphaReplace)
                    continue;
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                uint8_t maxA = 0u;
                const size_t e2 = std::min<size_t>(static_cast<size_t>(b.first) + b.count, g_rdGpu.local2dScratch.size());
                for (size_t vi = b.first; vi < e2; ++vi)
                {
                    const RdVert &v = g_rdGpu.local2dScratch[vi];
                    x0 = std::min(x0, v.pos[0]); x1 = std::max(x1, v.pos[0]);
                    y0 = std::min(y0, v.pos[1]); y1 = std::max(y1, v.pos[1]);
                    if (v.rgba[3] > maxA)
                        maxA = v.rgba[3];
                }
                if (x1 < x0 || y1 < y0)
                    continue;
                x0 = std::max(0.0f, std::min(x0, vpW)); x1 = std::max(0.0f, std::min(x1, vpW));
                y0 = std::max(0.0f, std::min(y0, vpH)); y1 = std::max(0.0f, std::min(y1, vpH));
                if ((x1 - x0) * (y1 - y0) < 0.90f * vpW * vpH)
                    continue;
                if (!abeOff && maxA < 0x80u)
                    continue;
                n2d = k;
                ++s_tailClearsDropped;
                break;
            }
        }
        if (g_rdZBias != 0.0f && rdgfx::hasUniform(rdgfx::Uniform::ZBias) && prevZBias != 0.0f) // PROBE-RD59 // RDG1
        {
            prevZBias = 0.0f;
            rdSetUniformFloat(rdgfx::Uniform::ZBias, prevZBias); // RDG1
        }
        if (!deferred2d.empty()) // FIX-RD63
        {
            Pace2Scope pace2B4DefScope(g_pace2Rd.b4, pace2On); // PROBE-PACE2
            mode2d = 1;
            if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
            rdDepthTest(false);
            rdDepthMask(false);
            rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c
            rdgfx::bindVertexArray(g_rdGpu.vao2d); // RDG1
            for (size_t idx : deferred2d)
            {
                if (!allow2dIdx(idx)) // PROBE-RD83
                    continue;
                const Rd2DBatch &b0 = g_rdGpu.local2dBatches[idx];
                if (!applyScissor(b0.scissor, true))
                    continue;
                emit2dBatch(idx, b0, b0.count, idx + 1, true, false);
            }
#ifdef _WIN32
            if (g_rdAlwaysFunc)
            {
                rdgfx::setDepthFunc(rdgfx::DepthFunc::Lequal); // RDG1
                g_rdAlwaysFunc = false;
            }
#endif
            mode2d = 0;
            if (rdgfx::hasUniform(rdgfx::Uniform::Mode2d)) // RDG1
                rdSetUniformInt(rdgfx::Uniform::Mode2d, mode2d); // RDG1
            curVao = 0u;
        }
        draw2dUntil(0u, true); // FEAT-RD8
        if (rd2dReasonDump) // PROBE-RD62
        {
            const char *remReason = (!s_draw2d || g_rdGpu.gpu2dVertCount == 0 || g_rdGpu.vao2d == 0) ? "skip-2dDisabled" : "skip-tailClearDrop";
            for (size_t k = i2d; k < g_rdGpu.local2dBatches.size(); ++k)
                log2dReason(k, g_rdGpu.local2dBatches[k], remReason);
            ps2xSafeLog("[rd2dr] summary total2d=%zu n2d=%zu drawn2d=%llu drawn3d=%u 3dSerial=%x..%x activeGMin=%u\n",
                        g_rdGpu.local2dBatches.size(), n2d, static_cast<unsigned long long>(draws2dThisFrame),
                        drawn3dThisFrame, drawn3dThisFrame ? min3dSerial : 0u, drawn3dThisFrame ? max3dSerial : 0u,
                        g_rdGpu.activeGMin);
        }
        if (rdbDump) // PROBE-RD29
            g_rdbCensus.dump();
        if (camDump) // PROBE-RD26
            g_camTl.finish();
        if (scisOn) // FEAT-RD22
        {
            if (boxClipped) // FIX-R7
            {
                rdgfx::setScissorTest(true);
                rdgfx::setScissorRect(vx, vy, vw, vh);
            }
            else
                rdgfx::setScissorTest(false); // RDG1
            scisOn = false;
        }
        rdgfx::bindTexture(0u, 0u); // FEAT-RD3 // RDG1
        g_rdc.bound = 0u; // FEAT-RD15
        rdgfx::bindVertexArray(0u); // RDG1
        rdgfx::disableShader(); // RDG1
    }

    rdgfx::setScissorTest(false); // FEAT-RD34b // RDG1
    rdgfx::setColorMask(true, true, true, true); // FEAT-RD9 // RDG1
    g_rdMaskCur = 15;
    (void)partialMask;
    rdgfx::setDepthMask(true); // RDG1
    rdgfx::setBlendAlpha(); // FEAT-RD2b // RDG1
    rdgfx::setDepthTest(false); // RDG1
    rdgfx::setBackfaceCulling(true); // RDG1
    rdgfx::setViewport(0, 0, rw, rh); // RDG1
    rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c

    if (rdFbTexOn() && vw > 0 && vh > 0) // FIX-RD79 // FIX-RD80 // FIX-RD81
    {
        Pace2Scope pace2B5Scope(g_pace2Rd.b5, pace2On); // PROBE-PACE2
        static int s_lastW = 0, s_lastH = 0; // FIX-RD85 // FXC1
        const int wantW = vw;
        const int wantH = static_cast<int>((static_cast<int64_t>(vh) * 512 + 224) / 448);
        static unsigned int s_prevFbo = 0u, s_lastSceneFbo = 0u; // FIX-RD85
        rdgfx::BackbufferCopyState copyState{}; // RDG1
        rdgfx::beginBackbufferCopy(copyState); // RDG1
        if (rdSnapOn()) // FIX-RD81: capture finished frame before RD80 blend
        {
            const uint64_t curSnapFrame = ++s_snapFrameSeq; // FIX-RD87
            if (s_snapNeedReset.exchange(false, std::memory_order_acquire))
            {
                for (size_t si = 0u; si < kRdSnapSlots; ++si)
                {
                    s_snapSlots[si].valid = false;
                    s_snapSlots[si].destFbp = 0xFFFFFFFFu;
                }
            }
            for (size_t si = 0u; si < kRdSnapSlots; ++si)
            {
                if (s_snapSlots[si].valid && (curSnapFrame - s_snapSlots[si].lastUsedFrame) >= 600ull)
                {
                    s_snapSlots[si].valid = false;
                    s_snapSlots[si].destFbp = 0xFFFFFFFFu;
                }
            }
            std::vector<RdSnapReq> snapReqs;
            {
                std::lock_guard<std::mutex> snapLock(s_snapMutex);
                snapReqs.swap(s_snapPending);
                copyBufFbps.erase( // FIX-RD87
                    std::remove_if(copyBufFbps.begin(), copyBufFbps.end(),
                                   [curSnapFrame](const RdCopyBufEntry &cb) { return (curSnapFrame - cb.lastSeenFrame) >= 900ull; }),
                    copyBufFbps.end());
            }
            if (!snapReqs.empty())
            {
                if (drawn3dThisFrame == 0u || palDrawnThisFrame) // FIX-RD81b // FIX-RD86
                {
                    if (drawn3dThisFrame == 0u)
                        s_snapSkippedNoGeom += static_cast<uint32_t>(snapReqs.size());
                }
                else if (copyState.valid) // RDG1
                {
                    for (const RdSnapReq &req : snapReqs)
                    {
                        size_t chosen = kRdSnapSlots;
                        for (size_t si = 0u; si < kRdSnapSlots; ++si)
                        {
                            if (s_snapSlots[si].valid && s_snapSlots[si].destFbp == req.destFbp)
                            {
                                chosen = si;
                                break;
                            }
                        }
                        if (chosen == kRdSnapSlots)
                        {
                            for (size_t si = 0u; si < kRdSnapSlots; ++si)
                            {
                                if (!s_snapSlots[si].valid)
                                {
                                    chosen = si;
                                    break;
                                }
                            }
                        }
                        if (chosen == kRdSnapSlots)
                        {
                            chosen = 0u;
                            for (size_t si = 1u; si < kRdSnapSlots; ++si)
                            {
                                if (s_snapSlots[si].lastUsedFrame < s_snapSlots[chosen].lastUsedFrame)
                                    chosen = si;
                            }
                        }
                        RdSnapSlot &slot = s_snapSlots[chosen];
                        const int dstW = std::max(1, std::min(wantW, static_cast<int>((static_cast<int64_t>(wantW) * req.w + 256) / 512)));
                        const int dstH = std::max(1, std::min(wantH, static_cast<int>((static_cast<int64_t>(vh) * req.h + 224) / 448)));
                        if (rdgfx::copyBackbufferToTexture(copyState, slot.texId, slot.fbo, slot.w, slot.h,
                                                           wantW, wantH, vx, vy, vw, vh, dstW, dstH, true)) // RDG1
                        {
                            slot.destFbp = req.destFbp;
                            slot.valid = true;
                            slot.lastUsedFrame = s_snapFrameSeq;
                            slot.reqW = req.w; // PROBE-V3A
                            slot.reqH = req.h; // PROBE-V3A
                            slot.dstW = dstW; // PROBE-V3A
                            slot.dstH = dstH; // PROBE-V3A
                            ++s_snapCount;
                        }
                    }
                    rdgfx::endBackbufferCopy(copyState); // RDG1
                    copyState.msaaResolved = false; // RDG1
                }
            }
        }
        const float dispF = rdGetDisplayBlend(); // FIX-RD80
        s_fbTexLastF = dispF; // FIX-RD80
        if (dispF < 0.999f && s_prevFilled && s_prevTexId != 0u && s_prevW == wantW && s_prevH == wantH) // FIX-RD80
        {
            const float clampedF = (dispF < 0.0f) ? 0.0f : dispF;
            const int aInt = static_cast<int>((1.0f - clampedF) * 255.0f + 0.5f);
            const unsigned char alphaByte = static_cast<unsigned char>(aInt < 0 ? 0 : (aInt > 255 ? 255 : aInt));
            rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c
            rdgfx::drawTextureOverlay(s_prevTexId, s_prevW, s_prevH,
                                      0.0f, 0.0f, static_cast<float>(s_prevW), static_cast<float>(s_prevH) * (448.0f / 512.0f),
                                      x, y, w, h, alphaByte); // RDG1
            ++s_fbTexBlendCount;
        }
        static const bool s_snapShowOn = []() { // PROBE-V3A
            const char *e = std::getenv("PS2X_RD_SNAPSHOW"); // PROBE-V3A
            return e && e[0] == '1'; // PROBE-V3A
        }(); // PROBE-V3A
        if (s_snapShowOn) // PROBE-V3A
        {
            static uint32_t s_snapShowFrames = 0u; // PROBE-V3A
            const bool logThisFrame = (++s_snapShowFrames % 60u) == 1u; // PROBE-V3A
            for (size_t si = 0u; si < kRdSnapSlots; ++si) // PROBE-V3A
            {
                const RdSnapSlot &slot = s_snapSlots[si]; // PROBE-V3A
                if (!slot.valid || slot.texId == 0u) // PROBE-V3A
                    continue; // PROBE-V3A
                rdSetFog(0.0f, 0.0f, 0.0f, 0.0f); // FIX-E1c
                rdgfx::drawTextureOverlay(slot.texId, slot.w, slot.h, // PROBE-V3A
                                          0.0f, 0.0f, static_cast<float>(slot.w), static_cast<float>(slot.h), // PROBE-V3A
                                          x + static_cast<float>(si) * (w / 4.0f), y, w / 4.0f, h / 4.0f, 255); // PROBE-V3A
                if (logThisFrame) // PROBE-V3A
                {
                    ps2xSafeLog("[snapshow] si=%u fbp=0x%x texId=%u slot=%dx%d want=%dx%d dst=%dx%d reqWH=%dx%d\n", // PROBE-V3A
                                static_cast<unsigned int>(si), slot.destFbp, slot.texId, // PROBE-V3A
                                slot.w, slot.h, wantW, wantH, slot.dstW, slot.dstH, slot.reqW, slot.reqH); // PROBE-V3A
                }
            }
        }
        if (copyState.valid) // RDG1
        {
            auto copyToFbTarget = [&](unsigned int &texId, unsigned int &fbo, int &texW, int &texH, bool &filled, uint32_t &copies) { // FIX-RD85B
                const bool willResize = (texId == 0u || texW != wantW || texH != wantH);
                if (willResize)
                {
                    if (texId != 0u)
                        g_rdc.invalidateTex(texId);
                    filled = false;
                }
                if (rdgfx::copyBackbufferToTexture(copyState, texId, fbo, texW, texH,
                                                   wantW, wantH, vx, vy, vw, vh, vw, vh, false)) // RDG1
                {
                    if (willResize && texId != 0u)
                        g_rdc.invalidateTex(texId);
                    ++copies;
                    filled = true;
                }
                else if (willResize && texId != 0u)
                {
                    g_rdc.invalidateTex(texId);
                }
                rdgfx::endBackbufferCopy(copyState); // RDG1
            };
            if (!palDrawnThisFrame) // FIX-RD86
                copyToFbTarget(s_prevTexId, s_prevFbo, s_prevW, s_prevH, s_prevFilled, s_fbTexCopies); // FIX-RD85B
            else // FIX-RD86
                ++s_fbTexFrozen; // FIX-RD86
            if (rdFbTexLastOn() && drawn3dThisFrame >= rdFbTexLast3dMin()) // FIX-RD85 // FIX-RD85B
                copyToFbTarget(s_lastSceneTexId, s_lastSceneFbo, s_lastW, s_lastH, s_lastSceneFilled, s_fbTexLastCopies); // FIX-RD85B
        }
    }

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

    g_rd35.n3d = drawn3dThisFrame; // PROBE-RD35
    g_rd35.usedNew = usedNewComposition;
    g_rd35.chosen2d = chosen2dIdx;
    g_rd35.draws2d = draws2dThisFrame; // PROBE-RD35
    g_rd35.mainSerials = 0u;
    if (chosen2dIdx >= 0)
        for (const auto &mr : g_rdGpu.frames2d[chosen2dIdx].mainRanges)
            g_rd35.mainSerials += mr.last - mr.first + 1u;
    g_rd35.glErr = rdgfx::getLastError(); // RDG1
    g_rd35.valid = true;
    ++g_rdGpu.frames;
    if (drawn3dThisFrame > 0u) // FEAT-RD34
    {
        g_rdGpu.prevDrawn3dCount = drawn3dThisFrame;
        if (usedNewComposition)
            ++g_rdGpu.drawsNew;
        else
            ++g_rdGpu.drawsPrev;
    }
    else
    {
        ++g_rdGpu.drawsEmpty;
        if (g_rdGpu.lastEmptyReason == 1) ++g_rdGpu.drawsEmptySempty;
        else if (g_rdGpu.lastEmptyReason == 2) ++g_rdGpu.drawsEmptyMissing;
        else if (g_rdGpu.lastEmptyReason == 3) ++g_rdGpu.drawsEmptyOverwritten;
        else if (g_rdGpu.lastEmptyReason == 4) ++g_rdGpu.drawsEmptySkipped;
        else ++g_rdGpu.drawsEmptySempty;
    }
    if (usedNewComposition && chosen2dIdx >= 0) // FEAT-RD33 // FEAT-RD34
    {
        ++g_rdGpu.composedFrames;
        if (chosen2dIdx == 1)
            ++g_rdGpu.usedPrev2dFrames;
    }
    else
    {
        ++g_rdGpu.redrawPrevFrames;
    }
    g_rdGpu.totalMainSerials += static_cast<uint64_t>(g_rdGpu.lastMainSerialCount); // FEAT-RD33
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
    // PROBE-E3a: periodic 1-second ABE filter logging
    if (s_abeFilter.active)
    {
        static auto s_abeLastLog = std::chrono::steady_clock::now();
        if (now - s_abeLastLog >= std::chrono::seconds(1))
        {
            ps2xSafeLog("[abefilter] drawn3d=%llu drawn2d=%llu skipped=%llu\n",
                        static_cast<unsigned long long>(s_abeDrawn3dSec),
                        static_cast<unsigned long long>(s_abeDrawn2dSec),
                        static_cast<unsigned long long>(s_abeSkippedSec));
            s_abeDrawn3dSec = 0;
            s_abeDrawn2dSec = 0;
            s_abeSkippedSec = 0;
            s_abeLastLog = now;
        }
    }
    // PROBE-E3d: periodic 1-second TEX filter logging
    if (s_texFilter.active)
    {
        static auto s_texLastLog = std::chrono::steady_clock::now();
        if (now - s_texLastLog >= std::chrono::seconds(1))
        {
            ps2xSafeLog("[texfilter] drawn3d=%llu drawn2d=%llu skipped=%llu\n",
                        static_cast<unsigned long long>(s_texDrawn3dSec),
                        static_cast<unsigned long long>(s_texDrawn2dSec),
                        static_cast<unsigned long long>(s_texSkippedSec));
            s_texDrawn3dSec = 0;
            s_texDrawn2dSec = 0;
            s_texSkippedSec = 0;
            s_texLastLog = now;
        }
    }
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
        ps2xSafeLog("[rd] glcalls/frame=%.0f idx32=%d uploadMs=%.2f\n", static_cast<double>(g_rdc.totalCalls) / fr, rdIdx32On() ? 1 : 0, g_rdGpu.totalUploadMs / fr); // FEAT-RD15 // FIX-RD90
        ps2xSafeLog("[rd] afail2pass/frame=%.1f afailNever/frame=%.1f\n", static_cast<double>(g_rdAfail2Pass) / fr, static_cast<double>(g_rdAfailNever) / fr); // FEAT-RD30
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
        ps2xSafeLog("[rd] composed=%u redrawPrev=%u usedPrev2d=%u mainSerials/frame=%.0f noMainFrames=%u repeatFrames=%u classicFrames=%u tailClears=%u partial=%u\n",
                    g_rdGpu.composedFrames,
                    g_rdGpu.redrawPrevFrames,
                    g_rdGpu.usedPrev2dFrames,
                    static_cast<double>(g_rdGpu.totalMainSerials) / fr,
                    g_rdGpu.noMainFrames,
                    g_rdRepeatFrames.load(std::memory_order_relaxed),
                    g_rdClassicFramesTotal.load(std::memory_order_relaxed),
                    s_tailClearsDropped,
                    s_partialFrames); // FEAT-RD33 // FEAT-RD33b // FEAT-RD33c // FEAT-RD36 // FEAT-RD37
        ps2xSafeLog("[rd] draws: new=%u prev=%u empty=%u (Sempty=%u missing=%u overwritten=%u skipped=%u)\n",
                    g_rdGpu.drawsNew,
                    g_rdGpu.drawsPrev,
                    g_rdGpu.drawsEmpty,
                    g_rdGpu.drawsEmptySempty,
                    g_rdGpu.drawsEmptyMissing,
                    g_rdGpu.drawsEmptyOverwritten,
                    g_rdGpu.drawsEmptySkipped); // FEAT-RD34
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
    if (rdFbTexOn()) // FIX-RD79
    {
        static auto s_fbTexLastLog = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_fbTexLastLog).count() >= 2000)
        {
            s_fbTexLastLog = now;
            ps2xSafeLog("[rdfb] used=%u copies=%u frozen=%u last=%u blend=%u lastf=%.3f snaps=%u snapUsed=%u snapSkippedNoGeom=%u copyskip=%u pal=%u palFail=%u palFb=%u a1=%u psm27why=%u/%u/%u/%u/%u/%u/ok=%u lastValid=%d lastId=%u prevId=%u nftrim=%u nfskip=%u\n", s_fbTexUsed, s_fbTexCopies, s_fbTexFrozen, s_fbTexLastCopies, s_fbTexBlendCount, s_fbTexLastF, s_snapCount, s_snapUsedCount, s_snapSkippedNoGeom, s_fbCopySkip, s_fbTexPal, s_fbTexPalFail, s_fbTexPalFb, s_fbTexAlphaFix, s_psm27WhyOff, s_psm27WhyNoPrev, s_psm27WhyNotFb, s_psm27WhyDispFbp, s_psm27WhyBadPsm, s_psm27WhyNoRange, s_psm27Ok, s_lastSceneFilled ? 1 : 0, s_lastSceneTexId, s_prevTexId, s_nextFrameTrim, s_nextFrameSkip); // FIX-RD79 // FIX-RD80 // FIX-RD81 // FIX-RD81b // FIX-RD82B // PROBE-RD82C // FIX-RD82J // FIX-RD84 // FIX-RD85 // FIX-RD85B // FIX-RD86 // FIX-RD87 // FIX-RD88 // FIX-RD88B
        }
    }
    s_v3nLast3dSeq = g_rdGpu.last3dSeq; // PROBE-V3N
    s_v3nLast2dSeq = g_rdGpu.last2dSeq; // PROBE-V3N
    uint32_t v3nFlags = 0; // PROBE-V3N
    if (v3nNew3d) v3nFlags |= (1u << 0); // PROBE-V3N
    if (v3nNew2d) v3nFlags |= (1u << 1); // PROBE-V3N
    if (!v3nNew3d && !v3nNew2d) v3nFlags |= (1u << 2); // PROBE-V3N
    s_v3nLastFlags = v3nFlags;       // PROBE-V3N
    s_v3oLastChosen2d = chosen2dIdx; // PROBE-V3O
    s_v3oLastCoverFail = coverFail;  // PROBE-V3O
    return true;
}

void ps2xRd35Describe(char *buf, size_t cap) // PROBE-RD35
{
    int o = std::snprintf(buf, cap, "valid=%d drawn3d=%u drawn2d=%u n2d=%u usedNewComp=%d redrawPrev=%d chosen2d=%d mainSerials=%u glErr=0x%x",
                          g_rd35.valid ? 1 : 0, g_rd35.n3d, g_rd35.draws2d, g_rd35.n2d, g_rd35.usedNew ? 1 : 0, g_rd35.usedNew ? 0 : 1, g_rd35.chosen2d,
                          g_rd35.mainSerials, g_rd35.glErr);
    const uint32_t cnt = g_rd35.n2d < 3u ? g_rd35.n2d : 3u;
    for (uint32_t k = 0; k < cnt && o > 0 && static_cast<size_t>(o) < cap; ++k)
    {
        const Rd35Rec &r = g_rd35.last[(g_rd35.n2d - 1u - k) % 3u];
        o += std::snprintf(buf + o, cap - static_cast<size_t>(o), " | 2d[-%u] prim=%llx alpha=%llx frame=%llx tex0=%llx bb=%.0f,%.0f-%.0f,%.0f",
                           k, static_cast<unsigned long long>(r.prim), static_cast<unsigned long long>(r.alpha),
                           static_cast<unsigned long long>(r.frame), static_cast<unsigned long long>(r.tex0),
                           static_cast<double>(r.bb[0]), static_cast<double>(r.bb[1]), static_cast<double>(r.bb[2]), static_cast<double>(r.bb[3]));
    }
}
