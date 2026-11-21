// FEAT-RD1: Direct geometry view from the VU1 scene buffer via OpenGL/rlgl.
#include <unordered_map>
#include <cstring>
#include "raylib.h"
#include "rlgl.h"
#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall glDepthFunc(unsigned int func); // FEAT-RD20 (opengl32 export; rlgl.h does not expose glad here)
extern "C" __declspec(dllimport) void * __stdcall wglGetProcAddress(const char *name); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glGetIntegerv(unsigned int pname, int *data); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glClearColor(float r, float g, float b, float a); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glClear(unsigned int mask); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glGenTextures(int n, unsigned int *textures); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glBindTexture(unsigned int target, unsigned int texture); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *pixels); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexParameteri(unsigned int target, unsigned int pname, int param); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices); // FIX-RD90
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
#include <mutex>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD1
extern unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa); // FEAT-RD5

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
    int palTexLoc = -1;  // FIX-RD82B
    int palModeLoc = -1; // FIX-RD82B
    int palFbLoc = -1;   // FIX-RD84
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
    int fixLoc = -1; // FEAT-RD7
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

// FEAT-RD30: AFAIL (TEST bits 12-13) write rules for fragments failing the alpha test
uint64_t g_rdAfail2Pass = 0, g_rdAfailNever = 0; // FEAT-RD30
int g_rdAtInvLoc = -1;                           // FEAT-RD30
void rdSetMaskPacked(int packed)                 // FEAT-RD30
{
    if (packed == g_rdMaskCur)
        return;
    rlColorMask((packed & 1) != 0, (packed & 2) != 0, (packed & 4) != 0, (packed & 8) != 0);
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
        rlDisableColorBlend();
    else
    {
        const int code = blendState - 1;
        const int sk = code / 128, dk = (code / 16) & 7, ek = code & 3; // FEAT-RD9
        auto glf = [](int k) { return k == 0 ? RL_ZERO : k == 1 ? RL_ONE : k == 2 ? RL_SRC_ALPHA : k == 3 ? RL_ONE_MINUS_SRC_ALPHA : k == 4 ? RL_DST_ALPHA : RL_ONE_MINUS_DST_ALPHA; };
        rlEnableColorBlend();
        if (sk >= 6) // FEAT-RD25: DATE approximation
            rlSetBlendFactorsSeparate(sk == 6 ? RL_ONE_MINUS_DST_ALPHA : RL_DST_ALPHA, sk == 6 ? RL_DST_ALPHA : RL_ONE_MINUS_DST_ALPHA, RL_ONE, RL_ZERO, RL_FUNC_ADD, RL_FUNC_ADD); // FEAT-RD31
        else
        rlSetBlendFactorsSeparate(glf(sk), glf(dk), RL_ONE, RL_ZERO, ek == 0 ? RL_FUNC_ADD : ek == 1 ? RL_FUNC_SUBTRACT : RL_FUNC_REVERSE_SUBTRACT, RL_FUNC_ADD); // FEAT-RD31: GS writes source alpha
        rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
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
        slot.ebo = rlLoadVertexBufferElement(nullptr, static_cast<int>(newCap * idxElemSize), true); // FIX-RD90
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
        rlUpdateVertexBufferElements(slot.ebo, idxPtr, static_cast<int>(nIdx * idxElemSize), 0); // FIX-RD90
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

static bool rdRingCoversRanges(uint32_t ringMin, uint32_t ringMax, const std::vector<RdSerialRange> &ranges) // FEAT-RD33
{
    if (ranges.empty())
        return true;
    if (ringMin == 0u)
        return false;
    return ringMin <= ranges.front().first && ranges.back().last <= ringMax;
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
static bool rdIdx32On() // FIX-RD90
{
    static const bool s_on = []() {
        if (g_rd24On || g_rdCamOn || g_rdBlendOn || g_rdReasonOn)
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
#ifdef _WIN32
    if (rdIdx32On())
    {
        glDrawElements(0x0004u /*GL_TRIANGLES*/, static_cast<int>(count), 0x1405u /*GL_UNSIGNED_INT*/,
                       reinterpret_cast<const void *>(static_cast<size_t>(first) * sizeof(uint32_t)));
        return;
    }
#endif
    rlDrawVertexArrayElements(static_cast<int>(first), static_cast<int>(count), nullptr);
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
static int g_rdZBiasLoc = -1; // PROBE-RD59
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
extern "C" __declspec(dllimport) unsigned int __stdcall glGetError(void); // PROBE-RD35
struct Rd35Stats { Rd35Rec last[3]; uint32_t n2d = 0, n3d = 0; bool usedNew = false, valid = false; int chosen2d = -1; uint32_t draws2d = 0, mainSerials = 0, glErr = 0; }; // PROBE-RD35
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

bool ps2xRdDraw(float x, float y, float w, float h) // FEAT-RD1
{
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
    bool pendingUpload[kRdSlotCount] = {}; // FEAT-RD14 // FEAT-RD34
    uint32_t evictedMaxSnap = 0u; // FEAT-RD34
    {
        Pace2Scope pace2B1Scope(g_pace2Rd.b1, pace2On); // PROBE-PACE2
        std::lock_guard<std::mutex> lock(g_rdMutex);
        if (!g_rdEverPublished && !g_rdGpu.everUploaded)
        {
            ++g_rdGpu.drawsEmpty;
            ++g_rdGpu.drawsEmptySempty;
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
            if (g_rd2dFrames[0].valid && rdRingCoversRanges(ringMin, ringMax, g_rd2dFrames[0].mainRanges))
                chosen2dIdx = 0;
            else if (g_rd2dFrames[1].valid && rdRingCoversRanges(ringMin, ringMax, g_rd2dFrames[1].mainRanges))
                chosen2dIdx = 1;

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
                }
                if (g_rd2dSeq != g_rdGpu.last2dSeq) // FEAT-RD13
                {
                    g_rdGpu.frames2d[0] = g_rd2dFrames[0];
                    g_rdGpu.frames2d[1] = g_rd2dFrames[1];
                    g_rdGpu.last2dSeq = g_rd2dSeq;
                }
                needRealign = true;
            }
            else if (!g_rdGpu.hasCompleteComposition)
            {
                ++g_rdGpu.drawsEmpty;
                ++g_rdGpu.drawsEmptyMissing;
                return ps2xRdTakeoverOn();
            }
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
            "uniform float uZBias;\n" // PROBE-RD59
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
            "    if (uMode2d == 0 && uZBias != 0.0) gl_Position.z -= uZBias * gl_Position.w;\n" // PROBE-RD59
            "    vCol = aCol;\n"
            "    vUv = aUv;\n" // FEAT-RD3
            "}\n";
        static const char *kFs =
            "#version 330\n"
            "in vec4 vCol;\n"
            "in vec2 vUv;\n" // FEAT-RD3
            "uniform sampler2D uSampler;\n" // FEAT-RD3
            "uniform sampler2D uPalTex;\n" // FIX-RD82B
            "uniform int uPalMode;\n" // FIX-RD82B
            "uniform int uPalFallback;\n" // FIX-RD84
            "uniform int uTex;\n" // FEAT-RD3
            "uniform int uTfx;\n" // FEAT-RD3
            "uniform int uTcc;\n" // FEAT-RD3
            "uniform int uAtst;\n"
            "uniform int uAref;\n"
            "uniform int uAtInvert;\n" // FEAT-RD30
            "uniform float uFixAlpha;\n" // FEAT-RD7
            "out vec4 finalColor;\n"
            "void main() {\n"
            "    vec3 rgb = vCol.rgb;\n"
            "    float a_ps2 = vCol.a * 255.0;\n"
            "    if (uTex != 0) {\n" // FEAT-RD3
            "        vec4 t = texture(uSampler, vUv);\n"
            "        if (uPalMode == 1) {\n" // FIX-RD82B
            "            float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));\n" // FIX-RD82B
            "            if (uPalFallback != 0)\n" // FIX-RD84
            "                t.rgb = vec3(0.40, 0.46, 0.80) * l + vec3(0.0, 0.01, 0.05);\n" // FIX-RD84
            "            else\n" // FIX-RD84
            "                t = texture(uPalTex, vec2((floor(l * 255.0) + 0.5) / 256.0, 0.5));\n" // FIX-RD82B
            "        }\n" // FIX-RD82B
            "        if (uPalMode != 0)\n" // FIX-RD82J
            "            t.a = 128.0 / 255.0;\n" // FIX-RD82J
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
            "        if (uAtInvert != 0) pass = !pass;\n" // FEAT-RD30
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
        g_rdAtInvLoc = GetShaderLocation(g_rdGpu.shader, "uAtInvert"); // FEAT-RD30
        g_rdGpu.fixLoc = GetShaderLocation(g_rdGpu.shader, "uFixAlpha"); // FEAT-RD7
        g_rdGpu.samplerLoc = GetShaderLocation(g_rdGpu.shader, "uSampler"); // FEAT-RD3
        g_rdGpu.texLoc = GetShaderLocation(g_rdGpu.shader, "uTex");         // FEAT-RD3
        g_rdGpu.tfxLoc = GetShaderLocation(g_rdGpu.shader, "uTfx");         // FEAT-RD3
        g_rdGpu.tccLoc = GetShaderLocation(g_rdGpu.shader, "uTcc");         // FEAT-RD3
        g_rdGpu.palTexLoc = GetShaderLocation(g_rdGpu.shader, "uPalTex");   // FIX-RD82B
        g_rdGpu.palModeLoc = GetShaderLocation(g_rdGpu.shader, "uPalMode"); // FIX-RD82B
        g_rdGpu.palFbLoc = GetShaderLocation(g_rdGpu.shader, "uPalFallback"); // FIX-RD84
        g_rdZBiasLoc = GetShaderLocation(g_rdGpu.shader, "uZBias");         // PROBE-RD59
        if (g_rdZBiasLoc >= 0) // PROBE-RD59
        {
            const float zb0 = 0.0f;
            SetShaderValue(g_rdGpu.shader, g_rdZBiasLoc, &zb0, SHADER_UNIFORM_FLOAT);
        }
        if (g_rdGpu.samplerLoc >= 0) // FEAT-RD3
        {
            int samplerUnit = 0;
            SetShaderValue(g_rdGpu.shader, g_rdGpu.samplerLoc, &samplerUnit, SHADER_UNIFORM_INT);
        }
        // FIX-RD82I: uPalTex=1 set lazily before first palette draw; do not touch uTex/uSampler/uPalMode here
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        g_rdGpu.flip[0] = 1.0f;
        g_rdGpu.flip[1] = (flipBits & 1) ? 1.0f : -1.0f; // FEAT-RD2: y flip default; bit 1 = do NOT flip y
        g_rdGpu.flip[2] = (flipBits & 2) ? -1.0f : 1.0f;
        if (g_rdGpu.flipLoc >= 0)
            SetShaderValue(g_rdGpu.shader, g_rdGpu.flipLoc, g_rdGpu.flip, SHADER_UNIFORM_VEC3);
        for (size_t i = 0; i < kRdSlotCount; ++i) // FEAT-RD14 // FEAT-RD34
            g_rdGpu.slots[i].vao = rlLoadVertexArray();
        g_rdGpu.vao2d = rlLoadVertexArray(); // FEAT-RD8
    }

    { // PROBE-PACE2
        Pace2Scope pace2B2Scope(g_pace2Rd.b2, pace2On); // PROBE-PACE2
        for (size_t idx = 0; idx < kRdSlotCount; ++idx) // FEAT-RD14 // FEAT-RD34
        {
            if (pendingUpload[idx])
            {
                g_rdGpu.everUploaded = true;
                uploadRdSlot(g_rdGpu.slots[idx], g_rdGpu.pendingVerts[idx], g_rdGpu.pendingIndices[idx], g_rdGpu.slots[idx].batches);
                if (g_rd24On || g_rdCamOn || g_rdBlendOn || g_rdReasonOn) // PROBE-RD24 // PROBE-RD29 // PROBE-RD26 // PROBE-RD59: CPU copy of uploaded verts + final 16-bit indices (relative to batch.baseVertex)
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

    static const bool s_draw2d = []{ const char *e = std::getenv("PS2X_RD_2D"); return !(e && e[0] == '0'); }(); // FEAT-RD9
    const bool hasDrawable3d = usedNewComposition ? (candDrawable3d > 0u) : (g_rdGpu.prevDrawn3dCount > 0u); // FEAT-RD34
    const bool hasDrawable2d = (s_draw2d && g_rdGpu.gpu2dVertCount > 0 && g_rdGpu.vao2d != 0 && !g_rdGpu.local2dBatches.empty()); // FEAT-RD34
    if (g_rdGpu.shader.id == 0 || (!hasDrawable3d && !hasDrawable2d)) // FEAT-RD34(c): never call the clear without drawing something
    {
        ++g_rdGpu.drawsEmpty;
        if (g_rdGpu.lastEmptyReason == 1) ++g_rdGpu.drawsEmptySempty;
        else if (g_rdGpu.lastEmptyReason == 2) ++g_rdGpu.drawsEmptyMissing;
        else if (g_rdGpu.lastEmptyReason == 3) ++g_rdGpu.drawsEmptyOverwritten;
        else if (g_rdGpu.lastEmptyReason == 4) ++g_rdGpu.drawsEmptySkipped;
        else ++g_rdGpu.drawsEmptySempty;
        return ps2xRdTakeoverOn();
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
    // FEAT-RD34b: force known GL state (raylib/ImGui may have changed it since our last frame)
    rlDisableScissorTest();
    rlColorMask(true, true, true, true);
    g_rdMaskCur = 15;
    rlEnableDepthMask();
    rlEnableColorBlend();
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlDisableVertexArray();
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
    g_rdc.reset(); g_rdc.calls = 0; g_rdc.depthTest = 1; g_rdc.depthMask = 1; // FEAT-RD15 // FEAT-RD34b
    rlDisableBackfaceCulling();

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
                on = true;
            }
            if (on != scisOn || (on && std::memcmp(r, scisRect, sizeof(r)) != 0))
            {
                if (on)
                {
                    if (!scisOn)
                        rlEnableScissorTest();
                    // FEAT-RD34b: always issue the rectangle
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
            int atstVal = (atst == 0u) ? 1 : static_cast<int>(atst); // FEAT-RD30
            int arefVal = static_cast<int>((b0.test >> 4) & 0xFFull);
            if (g_rdGpu.atstLoc >= 0)
                rdSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.arefLoc >= 0)
                rdSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.fixLoc >= 0)
                rdSetUniform(g_rdGpu.fixLoc, &fixAlpha, RL_SHADER_UNIFORM_FLOAT, 1);

            int texVal = 0, tfxVal = 0, tccVal = 0;
            int palMode = 0; // FIX-RD82B
            int palFallback = 0; // FIX-RD84
#ifdef _WIN32
            using PFN_glActiveTexture = void(__stdcall *)(unsigned int); // FIX-RD82G
            static auto pActiveTex = reinterpret_cast<PFN_glActiveTexture>(wglGetProcAddress("glActiveTexture")); // FIX-RD82G
#endif
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
                        if (pActiveTex && g_rdGpu.palModeLoc >= 0 && g_rdGpu.palTexLoc >= 0 && ps2xRdGetClutRgba256(b0.tex0, b0.texa, palRgba))
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
                            static unsigned int s_palTexId = 0u;
                            static uint32_t s_palLastRgba[256] = {};
                            static bool s_palHasData = false;
                            pActiveTex(0x84C0u); // GL_TEXTURE0
                            int prevTex0 = 0;
                            glGetIntegerv(0x8069u, &prevTex0); // GL_TEXTURE_BINDING_2D
                            pActiveTex(0x84C1u); // GL_TEXTURE1
                            if (s_palTexId == 0u)
                            {
                                glGenTextures(1, &s_palTexId);
                                if (s_palTexId != 0u)
                                {
                                    glBindTexture(0x0DE1u, s_palTexId); // GL_TEXTURE_2D
                                    glTexParameteri(0x0DE1u, 0x2802u, 0x812Fu); // GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE
                                    glTexParameteri(0x0DE1u, 0x2803u, 0x812Fu); // GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE
                                    glTexParameteri(0x0DE1u, 0x2800u, 0x2600u); // GL_TEXTURE_MAG_FILTER, GL_NEAREST
                                    glTexParameteri(0x0DE1u, 0x2801u, 0x2600u); // GL_TEXTURE_MIN_FILTER, GL_NEAREST
                                    glTexImage2D(0x0DE1u, 0, 0x8058, 256, 1, 0, 0x1908u, 0x1401u, palRgba); // GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE
                                    std::memcpy(s_palLastRgba, palRgba, sizeof(palRgba));
                                    s_palHasData = true;
                                }
                            }
                            else
                            {
                                glBindTexture(0x0DE1u, s_palTexId); // GL_TEXTURE_2D
                                if (!s_palHasData || std::memcmp(palRgba, s_palLastRgba, sizeof(palRgba)) != 0)
                                {
                                    glTexSubImage2D(0x0DE1u, 0, 0, 0, 256, 1, 0x1908u, 0x1401u, palRgba); // GL_RGBA, GL_UNSIGNED_BYTE
                                    std::memcpy(s_palLastRgba, palRgba, sizeof(palRgba));
                                    s_palHasData = true;
                                }
                            }
                            pActiveTex(0x84C0u); // GL_TEXTURE0
                            glBindTexture(0x0DE1u, static_cast<unsigned int>(prevTex0));
                            g_rdc.bound = 0u;
                            if (s_palTexId != 0u)
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
            if (g_rdGpu.texLoc >= 0)
                rdSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tfxLoc >= 0)
                rdSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tccLoc >= 0)
                rdSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);
            if (palMode != 0 && g_rdGpu.palModeLoc >= 0) // FIX-RD82I
            {
                static bool s_palTexSet = false;
                if (!s_palTexSet && g_rdGpu.palTexLoc >= 0)
                {
                    const int palUnit = 1;
                    rdSetUniform(g_rdGpu.palTexLoc, &palUnit, RL_SHADER_UNIFORM_INT, 1);
                    s_palTexSet = true;
                }
                rdSetUniform(g_rdGpu.palModeLoc, &palMode, RL_SHADER_UNIFORM_INT, 1);
                if (palMode == 1 && g_rdGpu.palFbLoc >= 0) // FIX-RD84
                    rdSetUniform(g_rdGpu.palFbLoc, &palFallback, RL_SHADER_UNIFORM_INT, 1);
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
                    rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
                    ++g_rdAfailNever;
                }
                else
                {
                    rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
                    if (g_rdAtInvLoc >= 0)
                        rdSetUniform(g_rdAtInvLoc, &one, RL_SHADER_UNIFORM_INT, 1);
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
                    if (g_rdAtInvLoc >= 0)
                        rdSetUniform(g_rdAtInvLoc, &zero, RL_SHADER_UNIFORM_INT, 1);
                    ++g_rdAfail2Pass;
                }
                rdSetMaskPacked(sMask);
                rdDepthMask(sDepth != 0);
            }
            else
                rlDrawVertexArray(static_cast<int>(b0.first), static_cast<int>(totalCount));
            if (palMode != 0) // FIX-RD82B // FIX-RD82G // FIX-RD82I
            {
                if (g_rdGpu.palModeLoc >= 0)
                {
                    const int palZero = 0;
                    rdSetUniform(g_rdGpu.palModeLoc, &palZero, RL_SHADER_UNIFORM_INT, 1);
                }
                if (palMode == 1 && palFallback != 0 && g_rdGpu.palFbLoc >= 0) // FIX-RD84
                {
                    const int palFbZero = 0;
                    rdSetUniform(g_rdGpu.palFbLoc, &palFbZero, RL_SHADER_UNIFORM_INT, 1);
                }
#ifdef _WIN32
                if (pActiveTex)
                {
                    pActiveTex(0x84C1u); // GL_TEXTURE1
                    glBindTexture(0x0DE1u, 0u); // GL_TEXTURE_2D
                    pActiveTex(0x84C0u); // GL_TEXTURE0
                }
#endif
                g_rdc.bound = 0u;
            }
            ++draws2dThisFrame;
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
            if (g_rdGpu.mode2dLoc >= 0)
                rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            rdDepthTest(false);
            rdDepthMask(false);
            rlEnableVertexArray(g_rdGpu.vao2d);

            while (i2d < n2d && (drawAll || (g_rdGpu.local2dBatches[i2d].afterSerial & 0x3FFFFFFFu) < maxS))
            {
                const Rd2DBatch &b0 = g_rdGpu.local2dBatches[i2d];
                if (!allow2dIdx(i2d)) // PROBE-RD83
                {
                    ++i2d;
                    continue;
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
            if (((prim >> 4) & 1ull) && ps2xRdIsFbFeedback(batch.kickSerial)) // FEAT-RD28b
            {
                if (rdbDump) rdb3d(1); // PROBE-RD29
                if (reasonDump && reasonPrinted < 30) rdReasonLog(1); // PROBE-RD59
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
            if (g_rdZBias != 0.0f && g_rdZBiasLoc >= 0) // PROBE-RD59
            {
                const float zb = (ztst == 2u) ? g_rdZBias : 0.0f;
                if (zb != prevZBias)
                {
                    rdSetUniform(g_rdZBiasLoc, &zb, RL_SHADER_UNIFORM_FLOAT, 1);
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
            int atstVal = (atst == 0u) ? 1 : static_cast<int>(atst); // FEAT-RD30: NEVER+AFAIL = single pass without discard
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
                    if (g_rdAtInvLoc >= 0)
                        rdSetUniform(g_rdAtInvLoc, &one, RL_SHADER_UNIFORM_INT, 1);
                    rdApplyAfailWrites(afail, zOff, sMask);
                    rdDrawElems(batch.first, batch.count); // FIX-RD90
                    if (g_rdAtInvLoc >= 0)
                        rdSetUniform(g_rdAtInvLoc, &zero, RL_SHADER_UNIFORM_INT, 1);
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
        if (g_rdZBias != 0.0f && g_rdZBiasLoc >= 0 && prevZBias != 0.0f) // PROBE-RD59
        {
            prevZBias = 0.0f;
            rdSetUniform(g_rdZBiasLoc, &prevZBias, RL_SHADER_UNIFORM_FLOAT, 1);
        }
        if (!deferred2d.empty()) // FIX-RD63
        {
            Pace2Scope pace2B4DefScope(g_pace2Rd.b4, pace2On); // PROBE-PACE2
            mode2d = 1;
            if (g_rdGpu.mode2dLoc >= 0)
                rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
            rdDepthTest(false);
            rdDepthMask(false);
            rlEnableVertexArray(g_rdGpu.vao2d);
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
                glDepthFunc(0x0203u);
                g_rdAlwaysFunc = false;
            }
#endif
            mode2d = 0;
            if (g_rdGpu.mode2dLoc >= 0)
                rdSetUniform(g_rdGpu.mode2dLoc, &mode2d, RL_SHADER_UNIFORM_INT, 1);
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
            rlDisableScissorTest();
            scisOn = false;
        }
        rlActiveTextureSlot(0); // FEAT-RD3
        rlDisableTexture();     // FEAT-RD3
        g_rdc.bound = 0u; // FEAT-RD15
        rlDisableVertexArray();
        rlDisableShader();
    }

    rlDisableScissorTest(); // FEAT-RD34b
    rlColorMask(true, true, true, true); // FEAT-RD9
    g_rdMaskCur = 15;
    (void)partialMask;
    rlEnableDepthMask();
    rlEnableColorBlend(); // FEAT-RD2b
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    rlViewport(0, 0, rw, rh);

#ifdef _WIN32
    if (rdFbTexOn() && vw > 0 && vh > 0) // FIX-RD79 // FIX-RD80 // FIX-RD81
    {
        Pace2Scope pace2B5Scope(g_pace2Rd.b5, pace2On); // PROBE-PACE2
        static int s_prevW = 0, s_prevH = 0, s_resW = 0, s_resH = 0, s_lastW = 0, s_lastH = 0; // FIX-RD85
        static bool s_prevFilled = false; // FIX-RD80
        const int wantW = vw;
        const int wantH = static_cast<int>((static_cast<int64_t>(vh) * 512 + 224) / 448);
        using PFN_glGenFramebuffers = void(__stdcall *)(int, unsigned int *);
        using PFN_glBindFramebuffer = void(__stdcall *)(unsigned int, unsigned int);
        using PFN_glFramebufferTexture2D = void(__stdcall *)(unsigned int, unsigned int, unsigned int, unsigned int, int);
        using PFN_glBlitFramebuffer = void(__stdcall *)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
        static auto pGenFbo = reinterpret_cast<PFN_glGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
        static auto pBindFbo = reinterpret_cast<PFN_glBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
        static auto pFbTex2D = reinterpret_cast<PFN_glFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
        static auto pBlitFbo = reinterpret_cast<PFN_glBlitFramebuffer>(wglGetProcAddress("glBlitFramebuffer"));
        static unsigned int s_prevFbo = 0u, s_resFbo = 0u, s_resTex = 0u, s_lastSceneFbo = 0u; // FIX-RD85
        int prevReadFbo = 0, prevDrawFbo = 0, sampleBufs = 0;
        if (pGenFbo && pBindFbo && pFbTex2D && pBlitFbo)
        {
            glGetIntegerv(0x8CAAu, &prevReadFbo); // GL_READ_FRAMEBUFFER_BINDING
            glGetIntegerv(0x8CA6u, &prevDrawFbo); // GL_DRAW_FRAMEBUFFER_BINDING
            glGetIntegerv(0x80A8u, &sampleBufs);  // GL_SAMPLE_BUFFERS
        }
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
                else if (pGenFbo && pBindFbo && pFbTex2D && pBlitFbo)
                {
                bool resolved = false;
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
                    if (slot.texId == 0u || slot.w != wantW || slot.h != wantH)
                    {
                        if (slot.texId != 0u)
                            rlUnloadTexture(slot.texId);
                        slot.texId = rlLoadTexture(nullptr, wantW, wantH, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                        slot.w = wantW;
                        slot.h = wantH;
                        if (slot.texId != 0u)
                        {
                            rlTextureParameters(slot.texId, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
                            rlTextureParameters(slot.texId, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
                            rlTextureParameters(slot.texId, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                            rlTextureParameters(slot.texId, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                            if (slot.fbo == 0u)
                                pGenFbo(1, &slot.fbo);
                            pBindFbo(0x8CA9u, slot.fbo);
                            pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, slot.texId, 0);
                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
                            glClear(0x00004000u);
                        }
                    }
                    if (slot.texId != 0u && slot.fbo != 0u)
                    {
                        const int dstW = std::max(1, std::min(wantW, static_cast<int>((static_cast<int64_t>(wantW) * req.w + 256) / 512)));
                        const int dstH = std::max(1, std::min(wantH, static_cast<int>((static_cast<int64_t>(vh) * req.h + 224) / 448)));
                        if (sampleBufs > 0)
                        {
                            if (!resolved)
                            {
                                if (s_resTex == 0u || s_resW != vw || s_resH != vh)
                                {
                                    if (s_resTex != 0u)
                                        rlUnloadTexture(s_resTex);
                                    s_resTex = rlLoadTexture(nullptr, vw, vh, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                                    s_resW = vw;
                                    s_resH = vh;
                                    if (s_resFbo == 0u)
                                        pGenFbo(1, &s_resFbo);
                                    pBindFbo(0x8CA9u, s_resFbo);
                                    pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, s_resTex, 0);
                                }
                                pBindFbo(0x8CA8u, static_cast<unsigned int>(prevDrawFbo));
                                pBindFbo(0x8CA9u, s_resFbo);
                                pBlitFbo(vx, vy, vx + vw, vy + vh, 0, 0, vw, vh, 0x00004000u, 0x2600u);
                                resolved = true;
                            }
                            pBindFbo(0x8CA8u, s_resFbo);
                            pBindFbo(0x8CA9u, slot.fbo);
                            pBlitFbo(0, vh, vw, 0, 0, 0, dstW, dstH, 0x00004000u, 0x2601u);
                        }
                        else
                        {
                            pBindFbo(0x8CA8u, static_cast<unsigned int>(prevDrawFbo));
                            pBindFbo(0x8CA9u, slot.fbo);
                            pBlitFbo(vx, vy + vh, vx + vw, vy, 0, 0, dstW, dstH, 0x00004000u, 0x2601u);
                        }
                        rlColorMask(false, false, false, true); // FIX-RD82J
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
                        glClear(0x00004000u); // FIX-RD82J
                        rlColorMask(true, true, true, true); // FIX-RD82J
                        slot.destFbp = req.destFbp;
                        slot.valid = true;
                        slot.lastUsedFrame = s_snapFrameSeq;
                        ++s_snapCount;
                    }
                }
                    pBindFbo(0x8CA8u, static_cast<unsigned int>(prevReadFbo));
                    pBindFbo(0x8CA9u, static_cast<unsigned int>(prevDrawFbo));
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
            Texture2D prevTex{};
            prevTex.id = s_prevTexId;
            prevTex.width = s_prevW;
            prevTex.height = s_prevH;
            prevTex.mipmaps = 1;
            prevTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            const Rectangle srcRec{0.0f, 0.0f, static_cast<float>(s_prevW), static_cast<float>(s_prevH) * (448.0f / 512.0f)};
            const Rectangle dstRec{x, y, w, h};
            const Color tint{255, 255, 255, alphaByte};
            rlDisableBackfaceCulling();
            DrawTexturePro(prevTex, srcRec, dstRec, Vector2{0.0f, 0.0f}, 0.0f, tint);
            rlDrawRenderBatchActive();
            rlEnableBackfaceCulling();
            ++s_fbTexBlendCount;
        }
        if (pGenFbo && pBindFbo && pFbTex2D && pBlitFbo)
        {
            bool resResolved = false; // FIX-RD85B
            auto copyToFbTarget = [&](unsigned int &texId, unsigned int &fbo, int &texW, int &texH, bool &filled, uint32_t &copies) { // FIX-RD85B
                if (texId == 0u || texW != wantW || texH != wantH)
                {
                    if (texId != 0u)
                    {
                        g_rdc.invalidateTex(texId);
                        rlUnloadTexture(texId);
                    }
                    texId = rlLoadTexture(nullptr, wantW, wantH, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                    texW = wantW;
                    texH = wantH;
                    filled = false;
                    if (texId != 0u)
                    {
                        g_rdc.invalidateTex(texId);
                        rlTextureParameters(texId, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
                        rlTextureParameters(texId, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
                        rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                        rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                        if (fbo == 0u)
                            pGenFbo(1, &fbo);
                        pBindFbo(0x8CA9u, fbo); // GL_DRAW_FRAMEBUFFER
                        pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, texId, 0);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
                        glClear(0x00004000u);
                    }
                }
                if (texId != 0u && fbo != 0u)
                {
                    if (sampleBufs > 0)
                    {
                        if (!resResolved)
                        {
                            if (s_resTex == 0u || s_resW != vw || s_resH != vh)
                            {
                                if (s_resTex != 0u)
                                    rlUnloadTexture(s_resTex);
                                s_resTex = rlLoadTexture(nullptr, vw, vh, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                                s_resW = vw;
                                s_resH = vh;
                                if (s_resFbo == 0u)
                                    pGenFbo(1, &s_resFbo);
                                pBindFbo(0x8CA9u, s_resFbo);
                                pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, s_resTex, 0);
                            }
                            pBindFbo(0x8CA8u, static_cast<unsigned int>(prevDrawFbo));
                            pBindFbo(0x8CA9u, s_resFbo);
                            pBlitFbo(vx, vy, vx + vw, vy + vh, 0, 0, vw, vh, 0x00004000u, 0x2600u);
                            resResolved = true;
                        }
                        pBindFbo(0x8CA8u, s_resFbo);
                        pBindFbo(0x8CA9u, fbo);
                        pBlitFbo(0, vh, vw, 0, 0, 0, vw, vh, 0x00004000u, 0x2600u);
                    }
                    else
                    {
                        pBindFbo(0x8CA8u, static_cast<unsigned int>(prevDrawFbo));
                        pBindFbo(0x8CA9u, fbo);
                        pBlitFbo(vx, vy + vh, vx + vw, vy, 0, 0, vw, vh, 0x00004000u, 0x2600u);
                    }
                    rlColorMask(false, false, false, true); // FIX-RD82J
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
                    glClear(0x00004000u); // FIX-RD82J
                    rlColorMask(true, true, true, true); // FIX-RD82J
                    ++copies;
                    filled = true;
                }
                pBindFbo(0x8CA8u, static_cast<unsigned int>(prevReadFbo));
                pBindFbo(0x8CA9u, static_cast<unsigned int>(prevDrawFbo));
            };
            if (!palDrawnThisFrame) // FIX-RD86
                copyToFbTarget(s_prevTexId, s_prevFbo, s_prevW, s_prevH, s_prevFilled, s_fbTexCopies); // FIX-RD85B
            else // FIX-RD86
                ++s_fbTexFrozen; // FIX-RD86
            if (rdFbTexLastOn() && drawn3dThisFrame >= rdFbTexLast3dMin()) // FIX-RD85 // FIX-RD85B
                copyToFbTarget(s_lastSceneTexId, s_lastSceneFbo, s_lastW, s_lastH, s_lastSceneFilled, s_fbTexLastCopies); // FIX-RD85B
        }
    }
#endif

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
    g_rd35.glErr = glGetError();
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
