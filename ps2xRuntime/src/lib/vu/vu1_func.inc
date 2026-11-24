// vu1_func.inc -- Task F1: functional VU1 engine for microcode image 86503d3905e35276 (PS2X_VU1_FUNC=1, default OFF).
// Included by ps2_vu1_core.cpp right before VU1Interpreter::execute (needs s_vfyCapture, GifPathId, PS2Memory, GS). No header is touched.
// Program output (VU memory + GIF bytes) is a pure function of VU memory at MSCAL (+ startPC/TOP/ITOP), so only memory writes and GIF packets are reproduced;
// cycles, flags and exit registers are not. The dispatcher (0008..0040) and the handlers 0000 0050 0060 0070 0088 0C80 00A8 are evaluated in closed form;
// any other handler / malformed GIF tag / unexpected state restores the VU memory snapshot and returns false (the caller then runs the exact interpreter).
// Float math: the FMAC primitive below is a value-only mirror of lbFmac / vuhot::fmacSlow (vu1_native_hand.inc / vu1_hot_ops.h): operands normalised
// (N4), fused single-rounding multiply-add in float, and the exact-double re-classification (zero / underflow -> signed 0, overflow -> signed FLT_MAX)
// whenever a lane of the result or of the product leaves the exponent window [2,253]. (vu1_hot_ops.h itself cannot be included in this TU: its fmacR
// touches private VU1Interpreter members and needs `#define private public`.)
#include <cfloat>
#include <cstdarg> // PROBE-M9
#include <cmath> // FEAT-RD40
#include <cstdlib> // PERF-V1G
#include <filesystem> // PERF-SA
#include <chrono> // FEAT-RD32
#include <intrin.h> // FEAT-RD32
#include <unordered_map>
#include <utility>

extern void ps2xSafeLog(const char *fmt, ...);
#include <atomic>
#include <mutex> // PROBE-M5
extern std::atomic<uint64_t> g_hostPresentSeq; // PERF-SA: per presented frame (gs_frontend.cpp)

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2]; // FEAT-RD3
    float fog;   // FIX-E1b
};
static_assert(sizeof(RdVert) == 32, "RdVert size"); // FIX-E1b
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
extern bool ps2xRdViewOn(); // FEAT-RD1
extern bool ps2xRdActive(); // FEAT-RD49
extern void ps2xRdPublish(const RdVert *v, size_t n, const RdBatch *b, size_t nb, uint32_t skippedDraws, uint32_t maskedDraws, float vpScaleZ, float vpOffsetZ); // FEAT-RD2 // FEAT-RD11
extern void ps2xRdPublishLean(std::vector<RdVert> &verts, std::vector<uint32_t> &indices, std::vector<RdBatch> &batches, uint32_t skippedDraws, uint32_t maskedDraws, float vpScaleZ, float vpOffsetZ); // FEAT-RD14
extern std::atomic<uint32_t> g_rdKickSerial; // FEAT-RD5
extern std::atomic<uint32_t> g_rdMergedSpan; // FEAT-RD17
extern uint64_t ps2xRdLookupContentId(uint32_t serial); // FEAT-RD5
extern bool ps2xRdLookupState(uint32_t serial, uint64_t &prim, uint64_t &alpha, uint64_t &test, uint64_t &frame,
                              uint64_t &zbuf, uint64_t &tex0, uint64_t &tex1, uint64_t &clamp, uint64_t &texa, uint64_t &fba); // FEAT-RD17
extern bool ps2xRdTakeoverOn(); // FEAT-RD40
extern bool ps2xRdFastGeomOn(); // FEAT-RD40
extern bool ps2xRdIsDisplayFbp(uint32_t fbp); // FEAT-RD40
extern bool ps2xRdHasDisplayFbp(); // FEAT-RD40
extern bool ps2xRdForceClassic(); // FEAT-RD48
extern void ps2xRdSpikeNoteVu1Bad(uint32_t kickSerial, uint32_t nVerts); // PROBE-M7

static inline bool rdSerialsCanMerge(uint32_t s0, uint32_t s1) // FEAT-RD17
{
    if (s0 == s1)
        return true;
    if (s0 == 0u || s1 == 0u)
        return false;
    const uint64_t c0 = ::ps2xRdLookupContentId(s0);
    const uint64_t c1 = ::ps2xRdLookupContentId(s1);
    if (c0 == 0ull || c0 != c1)
        return false;
    uint64_t p0, a0, t0, f0, z0, tx0, tx10, cl0, txa0, fb0;
    uint64_t p1, a1, t1, f1, z1, tx1, tx11, cl1, txa1, fb1;
    if (::ps2xRdLookupState(s0, p0, a0, t0, f0, z0, tx0, tx10, cl0, txa0, fb0) &&
        ::ps2xRdLookupState(s1, p1, a1, t1, f1, z1, tx1, tx11, cl1, txa1, fb1))
    {
        constexpr uint64_t kAbeTme = (1ull << 6) | (1ull << 4);
        if ((p0 & kAbeTme) != (p1 & kAbeTme) || a0 != a1 || t0 != t1 || f0 != f1 ||
            z0 != z1 || tx10 != tx11 || cl0 != cl1 || fb0 != fb1)
            return false;
    }
    return true;
}

namespace vu1func
{
constexpr uint64_t kImageHash = 0x86503d3905e35276ull; // FNV-1a of the 16 KB VU1 code image == s_hcCurHash (set by rebuildDecodedCodeCache)
constexpr uint32_t kMaxKickBytes = 0x10000u;           // == XgkickPipeline::kBufferSize

// PERF-BE: per-handler code signatures for multi-image functional VU1
struct HSig { uint16_t pc; uint8_t n; uint16_t r[9][2]; uint64_t h; }; // PERF-BE
static constexpr HSig kHandlerSigs[36] = { // PERF-BE // PERF-BF // PERF-BI
    {0x0000u, 1, {{0x0000u, 0x0048u}}, 0x9a470ad35b2fb298ull}, // PERF-BE
    {0x0050u, 1, {{0x0050u, 0x0068u}}, 0xd1238c8e19fb0c28ull}, // PERF-BE
    {0x0060u, 1, {{0x0060u, 0x0068u}}, 0x276777db3da0ce7dull}, // PERF-BE
    {0x0070u, 2, {{0x0070u, 0x0080u}, {0x0188u, 0x0190u}}, 0x182f8fc8cbe144adull}, // PERF-BE
    {0x0088u, 1, {{0x0088u, 0x00A0u}}, 0xe1d264bedbdf4018ull}, // PERF-BE
    {0x00A8u, 1, {{0x00A8u, 0x0190u}}, 0xb42910c677d880d9ull}, // PERF-BE
    {0x0198u, 1, {{0x0198u, 0x02D8u}}, 0x3a3412efa47f6308ull}, // PERF-BE
    {0x02E0u, 1, {{0x02E0u, 0x0558u}}, 0xd01cb535a3a5ea1eull}, // PERF-BE
    {0x0560u, 1, {{0x0560u, 0x0620u}}, 0x1f89f4bd4fa428daull}, // PERF-BE
    {0x0628u, 1, {{0x0628u, 0x0838u}}, 0x6a3767c3ac00a980ull}, // PERF-BE
    {0x0840u, 1, {{0x0840u, 0x0AD8u}}, 0xcf3832fc516675d2ull}, // PERF-BE
    {0x0AE0u, 1, {{0x0AE0u, 0x0B30u}}, 0x70738a21ff72c8f9ull}, // PERF-BE
    {0x0AF0u, 1, {{0x0AF0u, 0x0B30u}}, 0xadb6b0883764841cull}, // PERF-BE
    {0x0B38u, 1, {{0x0B38u, 0x0C30u}}, 0x59f48768f25f8591ull}, // PERF-BE
    {0x0B40u, 1, {{0x0B40u, 0x0C30u}}, 0x8d3a867942246c9dull}, // PERF-BE
    {0x0C38u, 1, {{0x0C38u, 0x0CB8u}}, 0x90c1db5943198cbcull}, // PERF-BE
    {0x0C80u, 1, {{0x0C80u, 0x0CB8u}}, 0x4f6e5991c0e08f11ull}, // PERF-BE
    {0x0CC0u, 1, {{0x0CC0u, 0x1058u}}, 0x8578087ce78d7452ull}, // PERF-BE
    {0x1090u, 1, {{0x1090u, 0x1530u}}, 0x713febc9f3ffe43bull}, // PERF-BE
    {0x15E0u, 1, {{0x15E0u, 0x1710u}}, 0xf697ac66790259a4ull}, // PERF-BE
    {0x1720u, 1, {{0x1720u, 0x18F0u}}, 0x2bd578cd4cb2acf0ull}, // PERF-BE
    {0x1900u, 2, {{0x1900u, 0x1908u}, {0x1970u, 0x1A40u}}, 0xefa6f02f67fb107eull}, // PERF-BE
    {0x1910u, 2, {{0x1910u, 0x1968u}, {0x19B0u, 0x1A40u}}, 0xc75180296fafa399ull}, // PERF-BE
    {0x1A68u, 1, {{0x1A68u, 0x1C20u}}, 0x4b79cd7cb3651352ull}, // PERF-BE
    {0x1C48u, 1, {{0x1C48u, 0x1CB8u}}, 0x9e6185d439761fbfull}, // PERF-BE
    {0x1CC0u, 2, {{0x1CC0u, 0x1CD0u}, {0x1CF0u, 0x1E38u}}, 0xb20321232f75beebull}, // PERF-BE
    {0x1CD8u, 1, {{0x1CD8u, 0x1E38u}}, 0x27ac1ae939a38d6full}, // PERF-BE
    {0x1E50u, 1, {{0x1E50u, 0x1EC8u}}, 0x28ff0ad08ea31dfeull}, // PERF-BE
    {0x1EE0u, 9, {{0x1EE0u, 0x1EE8u}, {0x1EF8u, 0x1FA0u}, {0x1FE0u, 0x2200u}, {0x2210u, 0x2430u}, {0x2440u, 0x25F0u}, {0x2670u, 0x2818u}, {0x28A0u, 0x2E18u}, {0x2E78u, 0x2E80u}, {0x2EC8u, 0x2F68u}}, 0x5000ff6d326f5ee6ull}, // PERF-BE
    {0x1EF0u, 8, {{0x1EF0u, 0x1FA0u}, {0x1FE0u, 0x2200u}, {0x2210u, 0x2430u}, {0x2440u, 0x25F0u}, {0x2670u, 0x2818u}, {0x28A0u, 0x2E18u}, {0x2E78u, 0x2E80u}, {0x2EC8u, 0x2F68u}}, 0x891459df4abaf8e4ull}, // PERF-BE
    {0x2E20u, 2, {{0x2E20u, 0x2E80u}, {0x2EC8u, 0x2F68u}}, 0xf246fb92d7a7dd5cull}, // PERF-BE
    {0x2E88u, 2, {{0x2D50u, 0x2DB0u}, {0x2E88u, 0x2EC0u}}, 0x8673a1e3ebf7bb01ull}, // PERF-BE
    {0x3160u, 1, {{0x3160u, 0x3270u}}, 0x91c21fb050fcc384ull}, // PERF-BE
    {0x32A0u, 1, {{0x32A0u, 0x3450u}}, 0x958591742b5184bfull}, // PERF-BE
    {0x01C8u, 1, {{0x01C8u, 0x02D8u}}, 0x7e4251d07ffd694dull}, // PERF-BF
    {0x0640u, 1, {{0x0640u, 0x0838u}}, 0x6fadae7b531ddfc6ull}, // PERF-BI
}; // PERF-BE

struct ImgSlot // PERF-BE
{ // PERF-BE
    uint64_t hash = 0; // PERF-BE
    uint64_t mask = 0; // PERF-BE
    uint64_t runs = 0; // PERF-BE
    uint64_t taken = 0; // PERF-BE
    uint64_t fb = 0; // PERF-BE
    bool used = false; // PERF-BE
}; // PERF-BE
static ImgSlot s_imgCache[8]; // PERF-BE
static thread_local int t_curSlot = -1; // PERF-BE

inline const uint8_t *pcToRow() // PERF-BE
{ // PERF-BE
    static uint8_t tbl[2048]; // PERF-BE
    static const bool init = []() { // PERF-BE
        std::memset(tbl, 0xFF, sizeof(tbl)); // PERF-BE
        for (size_t i = 0; i < 36u; ++i) // PERF-BE // PERF-BF // PERF-BI
            tbl[kHandlerSigs[i].pc >> 3] = static_cast<uint8_t>(i); // PERF-BE
        return true; // PERF-BE
    }(); // PERF-BE
    (void)init; // PERF-BE
    return tbl; // PERF-BE
} // PERF-BE

inline bool imageSelect(uint64_t hash, const uint8_t *code) // PERF-BE
{ // PERF-BE
    if (t_curSlot >= 0 && s_imgCache[t_curSlot].used && s_imgCache[t_curSlot].hash == hash) // PERF-BE
    { // PERF-BE
        ImgSlot &s = s_imgCache[t_curSlot]; // PERF-BE
        ++s.runs; // PERF-BE
        return (s.mask & 0x8000000000000001ull) != 0ull; // PERF-BE // PERF-BK
    } // PERF-BE
    int slot = -1; // PERF-BE
    for (int i = 0; i < 8; ++i) // PERF-BE
    { // PERF-BE
        if (s_imgCache[i].used && s_imgCache[i].hash == hash) // PERF-BE
        { // PERF-BE
            slot = i; // PERF-BE
            break; // PERF-BE
        } // PERF-BE
    } // PERF-BE
    if (slot >= 0) // PERF-BE
    { // PERF-BE
        t_curSlot = slot; // PERF-BE
        ++s_imgCache[slot].runs; // PERF-BE
        return (s_imgCache[slot].mask & 0x8000000000000001ull) != 0ull; // PERF-BE // PERF-BK
    } // PERF-BE
    int evict = 0; // PERF-BE
    for (int i = 0; i < 8; ++i) // PERF-BE
    { // PERF-BE
        if (!s_imgCache[i].used) { evict = i; break; } // PERF-BE
        if (s_imgCache[i].runs < s_imgCache[evict].runs) evict = i; // PERF-BE
    } // PERF-BE
    uint64_t mask = 0; // PERF-BE
    if (code) // PERF-BE
    { // PERF-BE
        for (size_t row = 0; row < 36u; ++row) // PERF-BE // PERF-BF // PERF-BI
        { // PERF-BE
            const HSig &sig = kHandlerSigs[row]; // PERF-BE
            uint64_t hv = 1469598103934665603ull; // PERF-BE
            for (uint8_t j = 0; j < sig.n; ++j) // PERF-BE
                for (uint32_t off = sig.r[j][0]; off <= static_cast<uint32_t>(sig.r[j][1]) + 7u; ++off) // PERF-BE
                    hv = (hv ^ code[off]) * 1099511628211ull; // PERF-BE
            if (hv == sig.h) // PERF-BE
                mask |= (1ull << row); // PERF-BE
        } // PERF-BE
        uint64_t hvFx = 1469598103934665603ull; // PERF-BK
        for (uint32_t off = 0u; off <= 0x0E7Fu; ++off) // PERF-BK
            hvFx = (hvFx ^ code[off]) * 1099511628211ull; // PERF-BK
        if (hvFx == 0xf64cba610d7fe263ull) // PERF-BK
            mask |= (1ull << 63); // PERF-BK
    } // PERF-BE
    s_imgCache[evict] = ImgSlot{hash, mask, 1ull, 0ull, 0ull, true}; // PERF-BE
    t_curSlot = evict; // PERF-BE
    return (mask & 0x8000000000000001ull) != 0ull; // PERF-BE // PERF-BK
} // PERF-BE

// PROBE-VX: Vu1Exec time split (PS2X_VX=1). Counters are plain: only the thread that called ps2xVxMarkThread() writes them.
namespace vx // PROBE-VX
{
enum { kH0050, kH0060, kH0070, kH0088, kH0C80, kH0AF0, kH0B40, kH15E0, kH1900, kH1C48, kH1EE0S, kH1EE0R, kH3160, kH00A8, kHClip, kHOther, kHN };
static const char *const kHNames[kHN] = {"0050", "0060", "0070", "0088", "0c80", "0af0", "0b40", "15e0", "1900", "1c48", "1ee0s", "1ee0r", "3160", "00a8", "clip", "other"}; // PROBE-VX
inline bool on() // PROBE-VX
{
    static const bool v = []() { const char *e = std::getenv("PS2X_VX"); return e && e[0] == '1'; }();
    return v;
}
static thread_local bool t_exec = false; // PROBE-VX
inline bool act() { return on() && t_exec; } // PROBE-VX
struct St // PROBE-VX
{
    uint64_t evT[8] = {}, evN[8] = {}, idleT = 0, compT = 0, gifT = 0, fbT = 0, fbN = 0, hT[kHN] = {}, hN[kHN] = {}, frames = 0, tsc0 = 0;
    std::chrono::steady_clock::time_point t0{};
    uint32_t tick = 0;
};
static St g_vx; // PROBE-VX
inline uint32_t slotOf(uint32_t h, const uint8_t *vu) // PROBE-VX
{
    switch (h)
    {
    case 0x0050u: return kH0050;
    case 0x0060u: return kH0060;
    case 0x0070u: return kH0070;
    case 0x0088u: return kH0088;
    case 0x0C80u: return kH0C80;
    case 0x0AF0u: return kH0AF0;
    case 0x0B40u: return kH0B40;
    case 0x15E0u: return kH15E0;
    case 0x1900u: return kH1900;
    case 0x1C48u: return kH1C48;
    case 0x1EE0u:
    case 0x1EF0u: { const uint8_t *p = vu + 1011u * 16u + 4u; return (static_cast<int16_t>(p[0] | (p[1] << 8)) < 0) ? kH1EE0R : kH1EE0S; } // ILW.y 1011 < 0 = rigid
    case 0x3160u: return kH3160;
    case 0x00A8u: return kH00A8;
    default: return kHOther;
    }
}
struct HScope // PROBE-VX: per-handler timing at the dispatch site; clip time is carved out of the rigid handler
{
    uint64_t t = 0, clip0 = 0;
    uint32_t slot = 0;
    void end(uint64_t now)
    {
        if (!t)
            return;
        uint64_t d = now - t;
        if (slot == kH1EE0R)
            d -= (g_vx.hT[kHClip] - clip0);
        g_vx.hT[slot] += d;
        ++g_vx.hN[slot];
        t = 0;
    }
    void begin(uint32_t s, uint64_t now)
    {
        end(now);
        slot = s;
        t = now;
        clip0 = g_vx.hT[kHClip];
    }
    ~HScope() { end(__rdtsc()); }
};
} // namespace vx

namespace hprof // PROBE-RD39
{
inline bool on() // PROBE-RD39
{
    static const bool v = []() { const char *e = std::getenv("PS2X_VU1_HPROF"); return e && e[0] == '1'; }();
    return v;
}
inline double msPerTick() // PROBE-RD39
{
    static const double v = []() {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = __rdtsc();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 20) {}
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t c1 = __rdtsc();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return (c1 > c0) ? (ms / static_cast<double>(c1 - c0)) : 0.0;
    }();
    return v;
}
struct Entry // PROBE-RD39
{
    uint16_t pc = 0;
    uint64_t ticks = 0;
    uint64_t calls = 0;
};
struct St // PROBE-RD39
{
    uint64_t totalT = 0, takenT = 0, fallbackT = 0, recT = 0, undoT = 0, frames = 0;
    uint64_t eeHdr = 0, eeVerts = 0, eeRec = 0, eeKick = 0, eeRest = 0, eeNSum = 0, eeCalls = 0; // PROBE-RD41
    Entry entries[64] = {};
    uint32_t nEntries = 0;
    uint32_t tick = 0;
    std::chrono::steady_clock::time_point lastPrint{};
    bool init = false;
};
static St g_st; // PROBE-RD39
inline void addHandler(uint16_t pc, uint64_t ticks) // PROBE-RD39
{
    for (uint32_t i = 0; i < g_st.nEntries; ++i)
    {
        if (g_st.entries[i].pc == pc)
        {
            g_st.entries[i].ticks += ticks;
            ++g_st.entries[i].calls;
            return;
        }
    }
    if (g_st.nEntries < 64u)
    {
        g_st.entries[g_st.nEntries++] = Entry{pc, ticks, 1ull};
    }
}
inline void subtractHandler(uint16_t pc, uint64_t ticks) // PROBE-RD39
{
    for (uint32_t i = 0; i < g_st.nEntries; ++i)
    {
        if (g_st.entries[i].pc == pc)
        {
            g_st.entries[i].ticks = (g_st.entries[i].ticks > ticks) ? (g_st.entries[i].ticks - ticks) : 0ull;
            return;
        }
    }
}
struct HScope // PROBE-RD39
{
    uint64_t t0 = 0;
    uint16_t pc = 0;
    void end()
    {
        if (t0)
        {
            addHandler(pc, __rdtsc() - t0);
            t0 = 0;
        }
    }
    void begin(uint16_t p)
    {
        end();
        pc = p;
        t0 = __rdtsc();
    }
    ~HScope() { end(); }
};
inline void maybeReport() // PROBE-RD39
{
    if ((++g_st.tick & 63u) != 0u)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!g_st.init)
    {
        g_st.init = true;
        g_st.lastPrint = now;
        (void)msPerTick();
        return;
    }
    const double sec = std::chrono::duration<double>(now - g_st.lastPrint).count();
    if (sec < 5.0)
        return;
    const double fr = sec * 60.0; // PROBE-RD39: always ms per 1/60 s
    const double k = msPerTick() / fr;
    Entry sorted[64];
    const uint32_t n = g_st.nEntries;
    for (uint32_t i = 0; i < n; ++i)
        sorted[i] = g_st.entries[i];
    std::sort(sorted, sorted + n, [](const Entry &a, const Entry &b) { return a.ticks > b.ticks; });
    char topBuf[512];
    int pos = 0;
    topBuf[0] = '\0';
    for (uint32_t i = 0; i < n && i < 10u && pos < 460; ++i)
    {
        pos += std::snprintf(topBuf + pos, sizeof(topBuf) - static_cast<size_t>(pos), "%s%04X:%.2f/%.1f",
                             i ? " " : "", sorted[i].pc, sorted[i].ticks * k, static_cast<double>(sorted[i].calls) / fr);
    }
    const double avgN = g_st.eeCalls ? (static_cast<double>(g_st.eeNSum) / static_cast<double>(g_st.eeCalls)) : 0.0; // PROBE-RD41
    ::ps2xSafeLog("[vu1h] total=%.2f taken=%.2f fallback=%.2f rec=%.2f undo=%.2f 1ee0split hdr=%.2f verts=%.2f rec=%.2f kick=%.2f rest=%.2f avgN=%.1f | top: %s\n",
                  g_st.totalT * k, g_st.takenT * k, g_st.fallbackT * k, g_st.recT * k, g_st.undoT * k,
                  g_st.eeHdr * k, g_st.eeVerts * k, g_st.eeRec * k, g_st.eeKick * k, g_st.eeRest * k, avgN, topBuf); // PROBE-RD41
    g_st.totalT = g_st.takenT = g_st.fallbackT = g_st.recT = g_st.undoT = g_st.frames = 0;
    g_st.eeHdr = g_st.eeVerts = g_st.eeRec = g_st.eeKick = g_st.eeRest = g_st.eeNSum = g_st.eeCalls = 0; // PROBE-RD41
    g_st.nEntries = 0;
    g_st.lastPrint = now;
}
} // namespace hprof

inline __m128 norm4(__m128 v) // vuhot::N4 (normalizeOperand on 4 lanes)
{
    const __m128i bits = _mm_castps_si128(v);
    const __m128i expo = _mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(0xFF));
    const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i isZero = _mm_cmpeq_epi32(expo, _mm_setzero_si128());
    const __m128i isFF = _mm_cmpeq_epi32(expo, _mm_set1_epi32(0xFF));
    __m128i r = bits;
    r = _mm_blendv_epi8(r, sign, isZero);
    r = _mm_blendv_epi8(r, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), isFF);
    return _mm_castsi128_ps(r);
}

inline uint32_t outOfWindow(__m128 v) // bit c set: lane c exponent outside [2,253]
{
    const __m128i ev = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF));
    return static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmplt_epi32(ev, _mm_set1_epi32(2)), _mm_cmpgt_epi32(ev, _mm_set1_epi32(253))))));
}

// KIND: 0 = ADD (a+b), 1 = SUB (a-b), 2 = MADD (ac+a*b), 3 = MSUB (ac-a*b), 4 = MUL (a*b). All four lanes (DEST = xyzw).
template <int KIND>
inline __m128 fmacV(__m128 a, __m128 b, __m128 ac)
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    __m128 ac4 = _mm_setzero_ps(), val, prodf = _mm_setzero_ps();
    uint32_t bad;
    // PERF-F2Z: lanes with an exactly-zero operand are exact in the fast path
    const __m128i absMask = _mm_set1_epi32(0x7FFFFFFF);
    const __m128i za = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a4), absMask), _mm_setzero_si128());
    const __m128i zb = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(b4), absMask), _mm_setzero_si128());
    const uint32_t zmask = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(za, zb))));
    if constexpr (KIND == 0 || KIND == 1)
    {
        val = (KIND == 0) ? _mm_add_ps(a4, b4) : _mm_sub_ps(a4, b4);
        const uint32_t bothZ = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(za, zb))));
        bad = outOfWindow(val) & ~bothZ;
    }
    else if constexpr (KIND == 4)
    {
        val = _mm_mul_ps(a4, b4);
        bad = outOfWindow(val) & ~zmask;
    }
    else
    {
        ac4 = norm4(ac);
        val = (KIND == 2) ? _mm_fmadd_ps(a4, b4, ac4) : _mm_fnmadd_ps(a4, b4, ac4);
        prodf = _mm_mul_ps(a4, b4);
        const __m128i acz = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(ac4), absMask), _mm_setzero_si128());
        const uint32_t aczm = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(acz)));
        const uint32_t acin = ~outOfWindow(ac4) & 0xFu;
        bad = (outOfWindow(val) & ~(zmask & (aczm | acin))) | (outOfWindow(prodf) & ~zmask);
    }
    if (bad == 0u)
        return val;
    alignas(16) float A[4], B[4], C[4], V[4];
    _mm_store_ps(A, a4);
    _mm_store_ps(B, b4);
    _mm_store_ps(C, ac4);
    _mm_store_ps(V, val);
    alignas(16) uint32_t out[4];
    for (int l = 0; l < 4; ++l)
    {
        const double ad = static_cast<double>(A[l]), bd = static_cast<double>(B[l]);
        double ex;
        if constexpr (KIND == 0)
            ex = ad + bd;
        else if constexpr (KIND == 1)
            ex = ad - bd;
        else if constexpr (KIND == 2)
            ex = static_cast<double>(C[l]) + ad * bd;
        else if constexpr (KIND == 3)
            ex = static_cast<double>(C[l]) - ad * bd;
        else
            ex = ad * bd;
        const double mag = std::fabs(ex);
        const bool zero = mag == 0.0;
        const bool over = mag > static_cast<double>(FLT_MAX);
        const bool under = (mag < static_cast<double>(FLT_MIN)) && !zero;
        uint64_t exb; // PERF-F2Z
        std::memcpy(&exb, &ex, 8);
        const uint32_t sg = static_cast<uint32_t>(exb >> 63) << 31;
        if (zero || under)
            out[l] = sg;
        else if (over)
            out[l] = sg | 0x7F7FFFFFu;
        else
            std::memcpy(&out[l], &V[l], 4);
    }
    return _mm_load_ps(reinterpret_cast<const float *>(out));
}

template <int L>
inline __m128 bc(__m128 v) // broadcast lane L (operand normalisation happens in fmacV)
{
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L));
}

struct Regs
{
    __m128 vf[32];
    __m128 acc;
    uint16_t vi[16];
    uint32_t vfValid = 0u; // PERF-V1P: bit i = R.vf[i] is exact
    uint16_t viValid = 0u; // PERF-V1P: bit i = R.vi[i] is exact
    float I = 0.0f;        // PERF-V1Q: VU1 I register
    bool iValid = false;   // PERF-V1Q: R.I is exact
};

inline __m128 ldq(const uint8_t *vu, uint32_t q) { return _mm_loadu_ps(reinterpret_cast<const float *>(vu + ((q & 0x3FFu) << 4))); }
struct UndoEnt
{
    uint16_t qword;
    uint8_t old[16];
};
struct UndoLog // PERF-V1G: flat buffer; clear() is size=0, alloc() skips zero-fill
{
    UndoEnt *data = nullptr;
    uint32_t size = 0;
    uint32_t cap = 0;
    UndoLog()
    {
        cap = 2048u;
        data = static_cast<UndoEnt *>(std::malloc(cap * sizeof(UndoEnt)));
    }
    ~UndoLog() { std::free(data); }
    void clear() { size = 0; }
    void ensure(uint32_t need)
    {
        if (size + need <= cap)
            return;
        uint32_t nc = cap ? cap * 2u : 2048u;
        while (nc < size + need)
            nc *= 2u;
        data = static_cast<UndoEnt *>(std::realloc(data, nc * sizeof(UndoEnt)));
        cap = nc;
    }
    void push(uint32_t qi, const uint8_t *vu)
    {
        ensure(1u);
        UndoEnt &e = data[size++];
        e.qword = static_cast<uint16_t>(qi);
        std::memcpy(e.old, vu + (qi << 4), 16);
    }
    UndoEnt *alloc(uint32_t count)
    {
        ensure(count);
        UndoEnt *p = data + size;
        size += count;
        return p;
    }
};
inline UndoLog &undoLog()
{
    static thread_local UndoLog log;
    return log;
}
// The ONLY function that writes VU memory inside the engine: logs the old qword first (undo log replaces the 16 KB snapshot).
inline void stq(uint8_t *vu, uint32_t q, __m128 v)
{
    const uint32_t qi = q & 0x3FFu;
    undoLog().push(qi, vu); // PERF-V1G
    _mm_storeu_ps(reinterpret_cast<float *>(vu + (qi << 4)), v);
}
// PERF: bulk-log `count` qwords starting at q (one resize + memcpys instead of a push_back per store); the stores into that range may then use stqRaw.
inline void logRange(const uint8_t *vu, uint32_t q, uint32_t count)
{
    UndoEnt *e = undoLog().alloc(count); // PERF-V1G
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t qi = (q + i) & 0x3FFu;
        e[i].qword = static_cast<uint16_t>(qi);
        std::memcpy(e[i].old, vu + (qi << 4), 16);
    }
}
inline void stqRaw(uint8_t *vu, uint32_t q, __m128 v) { _mm_storeu_ps(reinterpret_cast<float *>(vu + ((q & 0x3FFu) << 4)), v); } // range already logged
inline void stqiRaw(uint8_t *vu, uint32_t q, __m128i v) { stqRaw(vu, q, _mm_castsi128_ps(v)); } // PERF-V1G
inline uint16_t ilw(const uint8_t *vu, uint32_t q, uint32_t f) // ILW.f: low 16 bits of word f
{
    uint32_t w;
    std::memcpy(&w, vu + ((q & 0x3FFu) << 4) + 4u * f, 4);
    return static_cast<uint16_t>(w & 0xFFFFu);
}

// PERF-SVF: 8-wide struct-of-arrays AVX2 vertex chain, copied from lbVertex8 / lb8Op / lb8FromRows / lb8ToRows (vu1_native_hand.inc lines ~601-812; flags/sticky/mac
// bookkeeping dropped, only the value + exactness guard kept). Exact whenever no result lane of any op leaves the exponent window [2,253] (except certain zeros);
// otherwise the caller runs the scalar fmacV chain for that group. (PERF-V1G: placed before handler00A8 so both 00A8 and 1EE0 share it.)
struct Fv8V
{
    __m256 c[4];
};
inline __m256 fv8N(__m256 v) // == lb8N == vuhot::N4 on 8 lanes
{
    const __m256i bits = _mm256_castps_si256(v);
    const __m256i expo = _mm256_and_si256(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(0xFF));
    const __m256i sign = _mm256_and_si256(bits, _mm256_set1_epi32(static_cast<int>(0x80000000u)));
    const __m256i isZero = _mm256_cmpeq_epi32(expo, _mm256_setzero_si256());
    const __m256i isFF = _mm256_cmpeq_epi32(expo, _mm256_set1_epi32(0xFF));
    __m256i r = _mm256_blendv_epi8(bits, sign, isZero);
    r = _mm256_blendv_epi8(r, _mm256_or_si256(sign, _mm256_set1_epi32(0x7F7FFFFF)), isFF);
    return _mm256_castsi256_ps(r);
}
inline __m256i fv8OutWin(__m256 v)
{
    const __m256i ev = _mm256_and_si256(_mm256_srli_epi32(_mm256_castps_si256(v), 23), _mm256_set1_epi32(0xFF));
    return _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(2), ev), _mm256_cmpgt_epi32(ev, _mm256_set1_epi32(253)));
}
inline __m256i fv8IsZ(__m256 v) { return _mm256_castps_si256(_mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_EQ_OQ)); }
inline bool fv8Any(__m256i m) { return !_mm256_testz_si256(m, m); }

template <int DEST, bool MADD>
inline Fv8V fv8Op(const Fv8V &a, const Fv8V &b, const Fv8V &ac, __m256i &bad)
{
    Fv8V r;
    for (int c = 0; c < 4; ++c)
    {
        if (((DEST >> (3 - c)) & 1) == 0)
        {
            r.c[c] = _mm256_setzero_ps();
            continue;
        }
        const __m256 av = a.c[c], bv = b.c[c];
        const __m256 val = MADD ? _mm256_fmadd_ps(av, bv, ac.c[c]) : _mm256_mul_ps(av, bv);
        r.c[c] = val;
        const __m256i za = _mm256_or_si256(fv8IsZ(av), fv8IsZ(bv));
        __m256i zok = _mm256_and_si256(fv8IsZ(val), za);
        if (MADD)
            zok = _mm256_and_si256(zok, fv8IsZ(ac.c[c]));
        bad = _mm256_or_si256(bad, _mm256_andnot_si256(zok, fv8OutWin(val)));
        if (MADD)
            bad = _mm256_or_si256(bad, _mm256_andnot_si256(za, fv8OutWin(_mm256_mul_ps(av, bv))));
    }
    return r;
}
inline Fv8V fv8Const(__m128 v)
{
    v = norm4(v);
    Fv8V r;
    r.c[0] = _mm256_broadcastss_ps(v);
    r.c[1] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0x55));
    r.c[2] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xAA));
    r.c[3] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xFF));
    return r;
}
inline Fv8V fv8FromRows(const __m128 rows[8])
{
    const __m256 r0 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[0]), rows[4], 1);
    const __m256 r1 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[1]), rows[5], 1);
    const __m256 r2 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[2]), rows[6], 1);
    const __m256 r3 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[3]), rows[7], 1);
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    Fv8V r;
    r.c[0] = _mm256_shuffle_ps(t0, t1, 0x44);
    r.c[1] = _mm256_shuffle_ps(t0, t1, 0xEE);
    r.c[2] = _mm256_shuffle_ps(t2, t3, 0x44);
    r.c[3] = _mm256_shuffle_ps(t2, t3, 0xEE);
    return r;
}
inline void fv8ToRows(const Fv8V &v, __m128 rows[8])
{
    const __m256 t0 = _mm256_unpacklo_ps(v.c[0], v.c[1]), t1 = _mm256_unpacklo_ps(v.c[2], v.c[3]);
    const __m256 t2 = _mm256_unpackhi_ps(v.c[0], v.c[1]), t3 = _mm256_unpackhi_ps(v.c[2], v.c[3]);
    const __m256 r0 = _mm256_shuffle_ps(t0, t1, 0x44), r1 = _mm256_shuffle_ps(t0, t1, 0xEE);
    const __m256 r2 = _mm256_shuffle_ps(t2, t3, 0x44), r3 = _mm256_shuffle_ps(t2, t3, 0xEE);
    rows[0] = _mm256_castps256_ps128(r0);
    rows[4] = _mm256_extractf128_ps(r0, 1);
    rows[1] = _mm256_castps256_ps128(r1);
    rows[5] = _mm256_extractf128_ps(r1, 1);
    rows[2] = _mm256_castps256_ps128(r2);
    rows[6] = _mm256_extractf128_ps(r2, 1);
    rows[3] = _mm256_castps256_ps128(r3);
    rows[7] = _mm256_extractf128_ps(r3, 1);
}
// PERF-V1R: 8-wide clampI (0..255) and ftoiS matching clampI / ftoiS<SCALE>
inline __m256 fv8ClampI(__m256 x)
{
    return _mm256_min_ps(_mm256_max_ps(fv8N(x), _mm256_setzero_ps()), _mm256_set1_ps(255.0f));
}
template <int SCALE>
inline __m256i fv8FtoiS(__m256 a)
{
    const __m256 sc = _mm256_mul_ps(fv8N(a), _mm256_set1_ps(static_cast<float>(SCALE)));
    const __m256i t = _mm256_cvttps_epi32(sc);
    const __m256 ge = _mm256_cmp_ps(sc, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ);
    return _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(t), _mm256_castsi256_ps(_mm256_set1_epi32(INT32_MAX)), ge));
}

// PERF-SA: semantic scene buffer (PS2X_SCENE=1, default OFF)
namespace scene
{
inline bool on()
{
    static const bool v = []() { const char *e = std::getenv("PS2X_SCENE"); return e && e[0] == '1'; }();
    return v || ::ps2xRdActive(); // FEAT-RD1 // FEAT-RD49
}
inline bool checkOn()
{
    static const bool v = []() { const char *e = std::getenv("PS2X_SCENE_CHECK"); return e && e[0] == '1'; }();
    return v;
}
inline int64_t dumpFrame()
{
    static const int64_t v = []() -> int64_t {
        const char *e = std::getenv("PS2X_SCENE_DUMP");
        if (!e || !e[0])
            return -1;
        char *end = nullptr;
        long long n = std::strtoll(e, &end, 10);
        return (end != e && n >= 0) ? static_cast<int64_t>(n) : -1;
    }();
    return v;
}
inline bool leanRdOn() // FEAT-RD14
{
    static const bool v = ::ps2xRdViewOn() && !checkOn() && dumpFrame() < 0;
    return v && ::ps2xRdActive(); // FEAT-RD49
}
inline bool leanSkinOn() // FEAT-RD40
{
    static const bool v = []() {
        const char *e = std::getenv("PS2X_RD_LEANSKIN");
        return !(e && e[0] == '0') && ::ps2xRdTakeoverOn() && leanRdOn() && ::ps2xRdFastGeomOn();
    }();
    return v && !::ps2xRdForceClassic(); // FEAT-RD48
}
inline bool leanSkinCheckOn() // FEAT-RD40
{
    static const bool v = []() {
        const char *e = std::getenv("PS2X_RD_LEANSKIN_CHECK");
        return e && e[0] == '1' && ::ps2xRdTakeoverOn() && leanRdOn() && ::ps2xRdFastGeomOn();
    }();
    return v && !::ps2xRdForceClassic(); // FEAT-RD48
}
inline bool leanKickOn() // FEAT-RD42
{
    static const bool v = []() {
        const char *e = std::getenv("PS2X_RD_LEANKICK");
        return !(e && e[0] == '0');
    }();
    return v && ::ps2xRdActive(); // FEAT-RD48 // FEAT-RD49
}
inline void leanSkinNoteDraw(bool isLean, bool mismatch, bool isLeanKick = false) // FEAT-RD40 // FEAT-RD42
{
    struct Stat
    {
        uint32_t draws = 0, lean = 0, exact = 0, mism = 0, leanKicks = 0; // FEAT-RD42
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    };
    static thread_local Stat s;
    ++s.draws;
    if (isLean) ++s.lean; else ++s.exact;
    if (mismatch) ++s.mism;
    if (isLeanKick) ++s.leanKicks; // FEAT-RD42
    if ((s.draws & 0xFFu) == 0u)
    {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - s.t0).count() >= 5.0)
        {
            ::ps2xSafeLog("[leanskin] draws=%u lean=%u exact=%u mism=%u leanKicks=%u\n", s.draws, s.lean, s.exact, s.mism, s.leanKicks); // FEAT-RD42
            s.draws = s.lean = s.exact = s.mism = s.leanKicks = 0u; // FEAT-RD42
            s.t0 = now;
        }
    }
}

// FEAT-RD32: recording timing probe (PS2X_RD_RECPROF=1), rdtsc per section
inline bool recProfOn()
{
    static const bool v = []() { const char *e = std::getenv("PS2X_RD_RECPROF"); return e && e[0] == '1'; }();
    return v;
}
enum RecSlot { kRpParse = 0, kRpMarker, kRpBegin, kRpRigid, kRpSkinned, kRpClip, kRpPush, kRpPublish, kRpCount };
struct RecProf
{
    uint64_t cyc[kRpCount] = {};
    uint64_t cnt[kRpCount] = {};
    uint64_t verts = 0;
    uint64_t frames = 0;
    uint64_t t0Cyc = 0;
    std::chrono::steady_clock::time_point t0{};
};
inline RecProf &recProf()
{
    static thread_local RecProf p;
    return p;
}
struct RecScope
{
    int slot;
    uint64_t start;
    explicit RecScope(int s) : slot(s), start(recProfOn() ? __rdtsc() : 0ull) {}
    ~RecScope()
    {
        if (start)
        {
            RecProf &p = recProf();
            p.cyc[slot] += __rdtsc() - start;
            ++p.cnt[slot];
        }
    }
};
inline void recProfFrame(uint64_t verts)
{
    RecProf &p = recProf();
    ++p.frames;
    p.verts += verts;
    const auto now = std::chrono::steady_clock::now();
    if (p.t0.time_since_epoch().count() == 0)
    {
        p.t0 = now;
        p.t0Cyc = __rdtsc();
        return;
    }
    const double sec = std::chrono::duration<double>(now - p.t0).count();
    if (sec < 5.0)
        return;
    const double cps = static_cast<double>(__rdtsc() - p.t0Cyc) / sec;
    const double fr = p.frames ? static_cast<double>(p.frames) : 1.0;
    static const char *kNames[kRpCount] = {"parse", "marker", "begin", "rigid", "skinned", "clip", "push", "publish"};
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), "[rdrec] frames=%llu verts/frame=%.0f ms/frame:", static_cast<unsigned long long>(p.frames), static_cast<double>(p.verts) / fr);
    for (int i = 0; i < kRpCount && n > 0 && n < 440; ++i)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s=%.3f(n%.0f)", kNames[i], static_cast<double>(p.cyc[i]) / cps * 1000.0 / fr, static_cast<double>(p.cnt[i]) / fr);
    ::ps2xSafeLog("%s\n", buf);
    p = RecProf{};
}

struct SceneVertex // 80 bytes
{
    float pos[4];
    float normal[4];
    float stq[4];
    uint32_t rgba[4];
    int32_t screenXyz[3];
    uint32_t adc;
};
static_assert(sizeof(SceneVertex) == 80u, "SceneVertex size");

struct SceneDraw // 144 bytes // FEAT-V2
{
    uint32_t handlerPc;
    uint32_t mscalIndex;
    uint32_t gifOffset;
    uint32_t gifLength;
    uint32_t firstVertex;
    uint32_t vertexCount;
    uint32_t boneCount;
    uint32_t flags;
    float objMatrix[16];
    float vpScale[4];
    float vpOffset[4];
    uint32_t firstLight;   // FEAT-V2
    uint32_t lightCount;   // FEAT-V2
    uint32_t globalsIndex; // FEAT-V2
    uint32_t firstBone;    // FEAT-V2
};
static_assert(sizeof(SceneDraw) == 144u, "SceneDraw size"); // FEAT-V2

struct SceneLight // 160 bytes // FEAT-V2
{
    uint32_t type;
    uint32_t pad[3];
    float dirOrPos[4][4];
    float colour[4][4];
    float params[4];
};
static_assert(sizeof(SceneLight) == 160u, "SceneLight size"); // FEAT-V2

struct SceneGlobals // 144 bytes // FEAT-V2
{
    float viewProj[16];
    float vpScale[4];
    float vpOffset[4];
    float ambient[4];
    float lightScale[4];
    float fogParams[4];
};
static_assert(sizeof(SceneGlobals) == 144u, "SceneGlobals size"); // FEAT-V2

struct SceneBinSectionHeader // 12 bytes // FEAT-V2
{
    uint32_t magic;
    uint32_t count;
    uint32_t stride;
};
static_assert(sizeof(SceneBinSectionHeader) == 12u, "SceneBinSectionHeader size"); // FEAT-V2

struct SceneBinHeader // 32 bytes
{
    uint32_t magic;
    uint32_t version;
    uint32_t frameIndex;
    uint32_t drawCount;
    uint32_t vertexCount;
    uint32_t reserved[3];
};
static_assert(sizeof(SceneBinHeader) == 32u, "SceneBinHeader size");

struct SceneGs // FEAT-RD2
{
    uint64_t prim;
    uint64_t tex0;
    uint64_t clamp;
    uint64_t tex1;
    uint64_t alpha;
    uint64_t test;
    uint64_t frame;
    uint64_t zbuf;
    uint64_t texa;
    uint64_t texclut;
    uint32_t fba;
    uint32_t ownTex0;
    uint32_t kickSerial; // FEAT-RD5
    uint32_t fogcol; // FIX-E1c
};

struct GsShadow // FEAT-RD2
{
    uint64_t prim = 0;
    uint64_t tex0[2] = {};
    uint64_t clamp[2] = {};
    uint64_t tex1[2] = {};
    uint64_t alpha[2] = {};
    uint64_t test[2] = {};
    uint64_t frame[2] = {};
    uint64_t zbuf[2] = {};
    uint64_t fba[2] = {};
    uint64_t scissor[2] = {}; // FEAT-RD40
    uint64_t xyoffset[2] = {}; // FEAT-RD40
    uint64_t texa = 0;
    uint64_t texclut = 0;
    uint32_t fogcol = 0; // FIX-E1c
};

struct SceneFrame
{
    uint64_t frameIndex = 0;
    uint32_t mscalCount = 0;
    uint32_t gifBytes = 0;
    std::vector<SceneDraw> draws;
    std::vector<SceneGs> gs; // FEAT-RD2
    std::vector<SceneVertex> vertices;
    std::vector<SceneLight> lights;     // FEAT-V2
    std::vector<SceneGlobals> globals;  // FEAT-V2
    std::vector<::RdVert> rdVerts;    // FEAT-RD14
    std::vector<uint32_t> rdIndices;  // FEAT-RD14
    std::vector<::RdBatch> rdBatches; // FEAT-RD14
    uint32_t leanDraws = 0;           // FEAT-RD14
    uint32_t maskedDraws = 0;         // FEAT-RD14
    uint32_t bestVpVerts = 0;         // FEAT-RD14
    float vpScaleZ = 0.0f;            // FEAT-RD14
    float vpOffsetZ = 0.0f;           // FEAT-RD14
    float curVpS[2] = {0.0f, 0.0f};   // FEAT-RD21
    float curVpO[2] = {0.0f, 0.0f};   // FEAT-RD21
    uint32_t curCamHash = 0u;         // PROBE-RD26
    size_t hwVerts = 0, hwIdx = 0, hwBatches = 0; // FEAT-RD32: high-water marks so capacity survives swaps with the publish ring
    void clear()
    {
        frameIndex = 0;
        mscalCount = 0;
        gifBytes = 0;
        draws.clear();
        gs.clear(); // FEAT-RD2
        vertices.clear();
        lights.clear();   // FEAT-V2
        globals.clear();  // FEAT-V2
        rdVerts.clear();   // FEAT-RD14
        rdIndices.clear(); // FEAT-RD14
        rdBatches.clear(); // FEAT-RD14
        if (hwVerts > rdVerts.capacity()) rdVerts.reserve(hwVerts + hwVerts / 8u); // FEAT-RD32
        if (hwIdx > rdIndices.capacity()) rdIndices.reserve(hwIdx + hwIdx / 8u);   // FEAT-RD32
        if (hwBatches > rdBatches.capacity()) rdBatches.reserve(hwBatches + hwBatches / 8u); // FEAT-RD32
        leanDraws = 0;     // FEAT-RD14
        maskedDraws = 0;   // FEAT-RD14
        bestVpVerts = 0;   // FEAT-RD14
        vpScaleZ = 0.0f;   // FEAT-RD14
        vpOffsetZ = 0.0f;  // FEAT-RD14
    }
};

struct SceneState
{
    SceneFrame bufs[2];
    uint32_t writeIdx = 0;
    size_t mscalIdx0 = 0; // FEAT-RD14
    uint64_t frameSeq = 0;
    uint64_t lastPresentSeq = 0; // PERF-SA
    uint32_t mscalIndex = 0;
    uint32_t frameGifBytes = 0;
    uint32_t curBoneCount = 0;
    float curObjMatrix[16] = {};
    GsShadow gs{}; // FEAT-RD2
    bool curOwnTex0 = false; // FEAT-RD2
    uint32_t curKickSerial = 0u; // FEAT-RD5
    uint64_t totalFrames = 0;
    uint64_t totalDraws = 0;
    uint64_t totalVerts = 0;
    uint64_t totalMismatch = 0;
    uint64_t totalOwnTex0 = 0; // FEAT-RD2
    uint64_t totalTme = 0; // FEAT-RD2
    uint64_t totalAbe = 0; // FEAT-RD2
    uint64_t totalDistinctTex0 = 0; // FEAT-RD2
    uint64_t totalFallbacks = 0; // FEAT-RD2
    uint64_t mismatchByHandler[4] = {}; // 0:1EE0, 1:1EF0, 2:2440, 3:2900
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastLightLog = std::chrono::steady_clock::now(); // FEAT-V2
    uint32_t nextDrawLight = 0u;           // FEAT-V2
    uint32_t mscalGlobalsIdx = UINT32_MAX; // FEAT-V2
    size_t mscalLight0 = 0;                // FEAT-V2
    size_t mscalGlob0 = 0;                 // FEAT-V2
    uint32_t slDraws = 0u;                 // FEAT-V2
    uint32_t slLights = 0u;                // FEAT-V2
    uint32_t slMaxLights = 0u;             // FEAT-V2
    bool slHasFirst = false;               // FEAT-V2
    float slAmb0[3] = {};                  // FEAT-V2
    float slScale0[3] = {};                // FEAT-V2
    bool dumped = false;
};

static thread_local SceneState t_scene;

inline void applyAD(uint64_t data, uint8_t reg) // FEAT-RD2
{
    GsShadow &g = t_scene.gs;
    switch (reg)
    {
    case 0x00u: g.prim = data & 0x7FFull; break;
    case 0x06u: case 0x07u: g.tex0[reg - 0x06u] = data; t_scene.curOwnTex0 = true; break;
    case 0x08u: case 0x09u: g.clamp[reg - 0x08u] = data; break;
    case 0x14u: case 0x15u: g.tex1[reg - 0x14u] = data; break;
    case 0x18u: case 0x19u: g.xyoffset[reg - 0x18u] = data; break; // FEAT-RD40
    case 0x16u: case 0x17u:
    {
        constexpr uint64_t kTex2Mask = 0xFFFFFFE003F00000ull; // PSM, CBP, CPSM, CSM, CSA, CLD
        const uint32_t c = reg - 0x16u;
        g.tex0[c] = (g.tex0[c] & ~kTex2Mask) | (data & kTex2Mask);
        t_scene.curOwnTex0 = true;
        break;
    }
    case 0x1Cu: g.texclut = data; break;
    case 0x3Bu: g.texa = data; break;
    case 0x3Du: g.fogcol = static_cast<uint32_t>(data & 0x00FFFFFFull); break; // FIX-E1c
    case 0x40u: case 0x41u: g.scissor[reg - 0x40u] = data; break; // FEAT-RD40
    case 0x42u: case 0x43u: g.alpha[reg - 0x42u] = data; break;
    case 0x47u: case 0x48u: g.test[reg - 0x47u] = data; break;
    case 0x4Au: case 0x4Bu: g.fba[reg - 0x4Au] = data; break;
    case 0x4Cu: case 0x4Du: g.frame[reg - 0x4Cu] = data; break;
    case 0x4Eu: case 0x4Fu: g.zbuf[reg - 0x4Eu] = data; break;
    default: break;
    }
}

inline void parseChain(const uint8_t *p, uint32_t len) // FEAT-RD2
{
    t_scene.curOwnTex0 = false;
    uint32_t off = 0u;
    while (off + 16u <= len)
    {
        uint64_t tagLo, tagHi;
        std::memcpy(&tagLo, p + off, 8);
        std::memcpy(&tagHi, p + off + 8u, 8);
        off += 16u;
        const uint64_t nloop = tagLo & 0x7FFFu;
        const uint32_t eop = static_cast<uint32_t>((tagLo >> 15) & 1u);
        const uint32_t pre = static_cast<uint32_t>((tagLo >> 46) & 1u);
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint64_t nreg = (tagLo >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;
        if (pre)
            t_scene.gs.prim = (tagLo >> 47) & 0x7FFull;
        if (flg == 0u)
        {
            const uint64_t nq = nloop * nreg;
            if (nq * 16u > len - off)
                break;
            for (uint64_t i = 0; i < nq; ++i)
            {
                const uint32_t r = static_cast<uint32_t>(i % nreg);
                const uint32_t desc = static_cast<uint32_t>((tagHi >> (r * 4u)) & 0xFu);
                if (desc == 0xEu)
                {
                    uint64_t data;
                    std::memcpy(&data, p + off + i * 16u, 8);
                    applyAD(data, p[off + i * 16u + 8u]);
                }
                else if (desc == 0x0u)
                {
                    uint64_t data;
                    std::memcpy(&data, p + off + i * 16u, 8);
                    t_scene.gs.prim = data & 0x7FFull;
                }
            }
            off += static_cast<uint32_t>(nq * 16u);
        }
        else if (flg == 1u)
        {
            const uint64_t nd = nloop * nreg;
            const uint64_t bytes = ((nd + 1u) & ~1ull) * 8u;
            if (bytes > len - off)
                break;
            for (uint64_t i = 0; i < nd; ++i)
            {
                const uint32_t r = static_cast<uint32_t>(i % nreg);
                const uint32_t desc = static_cast<uint32_t>((tagHi >> (r * 4u)) & 0xFu);
                if (desc == 0x0u)
                {
                    uint64_t data;
                    std::memcpy(&data, p + off + i * 8u, 8);
                    t_scene.gs.prim = data & 0x7FFull;
                }
            }
            off += static_cast<uint32_t>(bytes);
        }
        else if (flg == 2u)
        {
            const uint64_t bytes = nloop * 16u;
            if (bytes > len - off)
                break;
            off += static_cast<uint32_t>(bytes);
        }
        else
            break;
        if (eop)
            break;
    }
}

inline int handlerSlot(uint32_t pc)
{
    switch (pc)
    {
    case 0x1EE0u: return 0;
    case 0x1EF0u: return 1;
    case 0x2440u: return 2;
    case 0x2900u: return 3;
    default: return 0;
    }
}

inline void setObjMatrix(__m128 r0, __m128 r1, __m128 r2, __m128 r3)
{
    _mm_storeu_ps(t_scene.curObjMatrix + 0, r0);
    _mm_storeu_ps(t_scene.curObjMatrix + 4, r1);
    _mm_storeu_ps(t_scene.curObjMatrix + 8, r2);
    _mm_storeu_ps(t_scene.curObjMatrix + 12, r3);
    ++t_scene.curBoneCount;
}

inline void beginMscal(const uint8_t *vu, size_t &d0, size_t &v0)
{
    const SceneFrame &f = t_scene.bufs[t_scene.writeIdx];
    t_scene.curBoneCount = 0;
    t_scene.mscalGlobalsIdx = UINT32_MAX; // FEAT-V2
    t_scene.mscalLight0 = f.lights.size(); // FEAT-V2
    t_scene.mscalGlob0 = f.globals.size(); // FEAT-V2
    if (leanRdOn()) // FEAT-RD14
    {
        d0 = f.rdBatches.size();
        v0 = f.rdVerts.size();
        t_scene.mscalIdx0 = f.rdIndices.size();
        return;
    }
    d0 = f.draws.size();
    v0 = f.vertices.size();
    const __m128 zero = _mm_setzero_ps();
    const __m128 m998 = ldq(vu, 998u), m999 = ldq(vu, 999u), m1000 = ldq(vu, 1000u), m1001 = ldq(vu, 1001u);
    const auto mulRow = [&](__m128 row, bool hasW) {
        __m128 a = fmacV<4>(m998, bc<0>(row), zero);
        a = fmacV<2>(m999, bc<1>(row), a);
        a = fmacV<2>(m1000, bc<2>(row), a);
        return hasW ? fmacV<2>(m1001, bc<3>(row), a) : a;
    };
    _mm_storeu_ps(t_scene.curObjMatrix + 0, mulRow(ldq(vu, 978u), false));
    _mm_storeu_ps(t_scene.curObjMatrix + 4, mulRow(ldq(vu, 979u), false));
    _mm_storeu_ps(t_scene.curObjMatrix + 8, mulRow(ldq(vu, 980u), false));
    _mm_storeu_ps(t_scene.curObjMatrix + 12, mulRow(ldq(vu, 981u), true));
}

inline void commitMscal(uint32_t gifBytes)
{
    ++t_scene.mscalIndex;
    t_scene.frameGifBytes += gifBytes;
}

inline void rollbackMscal(size_t d0, size_t v0)
{
    SceneFrame &f = t_scene.bufs[t_scene.writeIdx];
    f.lights.resize(t_scene.mscalLight0);  // FEAT-V2
    f.globals.resize(t_scene.mscalGlob0);  // FEAT-V2
    if (t_scene.nextDrawLight > static_cast<uint32_t>(t_scene.mscalLight0)) // FEAT-V2
        t_scene.nextDrawLight = static_cast<uint32_t>(t_scene.mscalLight0); // FEAT-V2
    if (leanRdOn()) // FEAT-RD14
    {
        f.rdBatches.resize(d0);
        f.rdVerts.resize(v0);
        f.rdIndices.resize(t_scene.mscalIdx0);
        return;
    }
    f.draws.resize(d0);
    f.gs.resize(d0); // FEAT-RD2
    f.vertices.resize(v0);
}
} // namespace scene

namespace watch // PROBE-M5
{
inline bool spikeOn() // PROBE-M7
{
    static const bool s_spikeOn = []() {
        const char *e = std::getenv("PS2X_SPIKE_WATCH");
        return e && e[0] == '1';
    }();
    return s_spikeOn;
}

inline bool on()
{
    static const bool s_on = []() {
        const char *e = std::getenv("PS2X_VU1_WATCH");
        return e && e[0] == '1';
    }();
    return s_on || spikeOn(); // PROBE-M7
}

static std::atomic<uint64_t> s_vbl{0}; // local host counter, no externs that may not link

struct BoneTrace
{
    static constexpr int kMaxBones = 64;
    static constexpr int kMaxVerts = 512;
    int boneCount = 0;
    float bones[kMaxBones][16];
    int16_t vertBone[kMaxVerts];
    void reset()
    {
        boneCount = 0;
        std::memset(vertBone, -1, sizeof(vertBone));
    }
};
static thread_local BoneTrace t_bones;

inline void recordBone(const uint8_t *vu, uint16_t vi3, uint16_t vi2)
{
    const int bi = t_bones.boneCount;
    if (bi < BoneTrace::kMaxBones)
    {
        for (int r = 0; r < 4; ++r)
            _mm_storeu_ps(t_bones.bones[bi] + r * 4, ldq(vu, 978u + static_cast<uint32_t>(r)));
        ++t_bones.boneCount;
    }
    const int16_t tag = (bi < BoneTrace::kMaxBones) ? static_cast<int16_t>(bi) : static_cast<int16_t>(-1);
    for (uint16_t k = 0; k < vi2; ++k)
    {
        const uint32_t idx = static_cast<uint32_t>(vi3) + k;
        if (idx < BoneTrace::kMaxVerts)
            t_bones.vertBone[idx] = tag;
    }
}

struct SumKey
{
    uint16_t handler = 0;
    uint16_t mscal = 0;
    uint32_t badDraws = 0;
};

struct RingEntry
{
    uint16_t handler = 0;
    uint16_t mscal = 0;
    uint32_t gifOffset = 0;
    uint32_t nVerts = 0;
    uint32_t badVerts = 0;
    float maxVal = 0.0f;
    uint32_t firstBadIdx = 0;
    bool used = false;
};

struct WatchState
{
    std::mutex mtx;
    uint32_t printedLines = 0;
    uint32_t totalBadDraws = 0;
    bool printedRing100 = false;
    uint32_t draws = 0;
    uint32_t badDraws = 0;
    uint32_t badVerts = 0;
    static constexpr size_t kMaxKeys = 64;
    SumKey keys[kMaxKeys] = {};
    size_t numKeys = 0;
    static constexpr size_t kRingSize = 8;
    RingEntry ring[kRingSize] = {};
    size_t ringHead = 0;
    std::chrono::steady_clock::time_point lastSum = std::chrono::steady_clock::now();
};
static WatchState s_ws;

inline void checkDraw(const uint8_t *vu, uint16_t handler, uint16_t mscal, uint32_t gifOffset,
                      uint32_t nVerts, bool skinned, uint32_t s2base, uint32_t s3base,
                      const int32_t *idxs, const __m128 *rigidPs, const int32_t *rigidWd)
{
    // Unit: clip-space homogeneous XYZ(W) in VU memory (636+idx) before perspective divide (1/w) and before viewport scaling (1002/1003).
    uint32_t bad = 0;
    uint32_t firstBad = 0;
    float maxAbs = 0.0f;
    bool hasNonFinite = false;
    float firstPos[4] = {};

    for (uint32_t v = 0; v < nVerts; ++v)
    {
        __m128 vp;
        if (skinned)
            vp = ldq(vu, static_cast<uint32_t>(636 + idxs[v]) & 0x3FFu);
        else
            vp = rigidPs[v];
        alignas(16) float p[4];
        _mm_store_ps(p, vp);

        bool vBad = false;
        for (int c = 0; c < 4; ++c)
        {
            if (!std::isfinite(p[c]))
            {
                vBad = true;
                hasNonFinite = true;
            }
            else
            {
                const float av = std::fabs(p[c]);
                if (av > maxAbs)
                    maxAbs = av;
                if (c < 3 && av > 1.0e5f)
                    vBad = true;
            }
        }
        if (vBad)
        {
            if (bad == 0)
            {
                firstBad = v;
                std::memcpy(firstPos, p, sizeof(firstPos));
            }
            ++bad;
        }
    }

    const float reportMax = hasNonFinite ? INFINITY : maxAbs;
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(s_ws.mtx);
    ++s_ws.draws;

    if (bad > 0)
    {
        if (spikeOn()) // PROBE-M7
        {
            const uint32_t ks = scene::t_scene.curKickSerial & 0x3FFFFFFFu;
            if (ks != 0u)
                ::ps2xRdSpikeNoteVu1Bad(ks, nVerts);
        }
        ++s_ws.badDraws;
        s_ws.badVerts += bad;
        ++s_ws.totalBadDraws;

        bool foundKey = false;
        for (size_t i = 0; i < s_ws.numKeys; ++i)
        {
            if (s_ws.keys[i].handler == handler && s_ws.keys[i].mscal == mscal)
            {
                ++s_ws.keys[i].badDraws;
                foundKey = true;
                break;
            }
        }
        if (!foundKey && s_ws.numKeys < WatchState::kMaxKeys)
            s_ws.keys[s_ws.numKeys++] = SumKey{handler, mscal, 1u};

        bool distinctInRing = true;
        for (size_t i = 0; i < WatchState::kRingSize; ++i)
        {
            if (s_ws.ring[i].used && s_ws.ring[i].handler == handler &&
                s_ws.ring[i].mscal == mscal && s_ws.ring[i].gifOffset == gifOffset)
            {
                distinctInRing = false;
                break;
            }
        }
        if (distinctInRing)
        {
            s_ws.ring[s_ws.ringHead] = RingEntry{handler, mscal, gifOffset, nVerts, bad, reportMax, firstBad, true};
            s_ws.ringHead = (s_ws.ringHead + 1u) % WatchState::kRingSize;
        }

        if (s_ws.printedLines < 200u)
        {
            ++s_ws.printedLines;
            alignas(16) float obj[16];
            for (int r = 0; r < 4; ++r)
                _mm_store_ps(obj + r * 4, ldq(vu, 978u + static_cast<uint32_t>(r)));

            int boneIdx = -1;
            if (skinned)
            {
                const int32_t vIdx = idxs[firstBad];
                if (vIdx >= 0 && vIdx < BoneTrace::kMaxVerts)
                    boneIdx = t_bones.vertBone[vIdx];
            }

            char boneStr[256];
            if (skinned && boneIdx >= 0 && boneIdx < t_bones.boneCount)
            {
                const float *bm = t_bones.bones[boneIdx];
                std::snprintf(boneStr, sizeof(boneStr),
                              "%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g",
                              bm[0], bm[1], bm[2], bm[3], bm[4], bm[5],
                              bm[6], bm[7], bm[8], bm[9], bm[10], bm[11]);
            }
            else
            {
                std::snprintf(boneStr, sizeof(boneStr), "-");
            }

            const uint64_t vbl = s_vbl.load(std::memory_order_relaxed);
            const uint32_t kickSerial = scene::t_scene.curKickSerial & 0x3FFFFFFFu; // PROBE-M7: common 30-bit serial matching RdBatch.kickSerial
            ::ps2xSafeLog("[vu1bad] handler=0x%04x mscal=0x%04x gifOffset=%u n=%u bad=%u max=%g firstBadIdx=%u pos=(%g,%g,%g,%g) "
                          "obj=[%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g] bone0=[%s] boneIdx=%d vbl=%llu kickSerial=%u\n",
                          handler, mscal, gifOffset, nVerts, bad, reportMax, firstBad,
                          firstPos[0], firstPos[1], firstPos[2], firstPos[3],
                          obj[0], obj[1], obj[2], obj[3], obj[4], obj[5], obj[6], obj[7],
                          obj[8], obj[9], obj[10], obj[11], obj[12], obj[13], obj[14], obj[15],
                          boneStr, boneIdx, static_cast<unsigned long long>(vbl), kickSerial);

            if (skinned)
            {
                const int32_t idx = idxs[firstBad];
                alignas(16) float q636[4], q716[4], q796[4], q556[4], qs2[4], qs3[4];
                _mm_store_ps(q636, ldq(vu, static_cast<uint32_t>(636 + idx) & 0x3FFu));
                _mm_store_ps(q716, ldq(vu, static_cast<uint32_t>(716 + idx) & 0x3FFu));
                _mm_store_ps(q796, ldq(vu, static_cast<uint32_t>(796 + idx) & 0x3FFu));
                _mm_store_ps(q556, ldq(vu, static_cast<uint32_t>(556 + idx) & 0x3FFu));
                _mm_store_ps(qs2, ldq(vu, (s2base + firstBad) & 0x3FFu));
                _mm_store_ps(qs3, ldq(vu, (s3base + firstBad) & 0x3FFu));
                ::ps2xSafeLog("[vu1bad-skin] idx=%d 636=(%g,%g,%g,%g) 716=(%g,%g,%g,%g) 796=(%g,%g,%g,%g) 556=(%g,%g,%g,%g) s2=(%g,%g,%g,%g) s3=(%g,%g,%g,%g)\n",
                              idx,
                              q636[0], q636[1], q636[2], q636[3],
                              q716[0], q716[1], q716[2], q716[3],
                              q796[0], q796[1], q796[2], q796[3],
                              q556[0], q556[1], q556[2], q556[3],
                              qs2[0], qs2[1], qs2[2], qs2[3],
                              qs3[0], qs3[1], qs3[2], qs3[3]);
                if (boneIdx >= 0 && boneIdx < t_bones.boneCount)
                {
                    const float *bm = t_bones.bones[boneIdx];
                    ::ps2xSafeLog("[vu1bad-bone] boneIdx=%d r0=(%g,%g,%g,%g) r1=(%g,%g,%g,%g) r2=(%g,%g,%g,%g) r3=(%g,%g,%g,%g)\n",
                                  boneIdx,
                                  bm[0], bm[1], bm[2], bm[3], bm[4], bm[5], bm[6], bm[7],
                                  bm[8], bm[9], bm[10], bm[11], bm[12], bm[13], bm[14], bm[15]);
                }
            }
            (void)rigidWd;
        }

        if (!s_ws.printedRing100 && s_ws.totalBadDraws >= 100u)
        {
            s_ws.printedRing100 = true;
            ::ps2xSafeLog("[vu1bad-ring100] last 8 distinct bad draws:\n");
            for (size_t k = 0; k < WatchState::kRingSize; ++k)
            {
                const RingEntry &e = s_ws.ring[(s_ws.ringHead + k) % WatchState::kRingSize];
                if (!e.used)
                    continue;
                ::ps2xSafeLog("  [%zu] handler=0x%04x mscal=0x%04x gifOffset=%u n=%u bad=%u max=%g firstBadIdx=%u\n",
                              k, e.handler, e.mscal, e.gifOffset, e.nVerts, e.badVerts, e.maxVal, e.firstBadIdx);
            }
        }
    }

    if (std::chrono::duration<double>(now - s_ws.lastSum).count() >= 5.0)
    {
        char topBuf[512];
        int pos = 0;
        bool used[WatchState::kMaxKeys] = {};
        for (int t = 0; t < 6; ++t)
        {
            size_t best = WatchState::kMaxKeys;
            for (size_t i = 0; i < s_ws.numKeys; ++i)
            {
                if (!used[i] && (best == WatchState::kMaxKeys || s_ws.keys[i].badDraws > s_ws.keys[best].badDraws))
                    best = i;
            }
            if (best == WatchState::kMaxKeys)
                break;
            used[best] = true;
            pos += std::snprintf(topBuf + pos, sizeof(topBuf) - static_cast<size_t>(pos),
                                 "%shandler=0x%04x mscal=0x%04x n=%u",
                                 t == 0 ? "" : " ; ",
                                 s_ws.keys[best].handler, s_ws.keys[best].mscal, s_ws.keys[best].badDraws);
        }
        if (pos == 0)
            std::snprintf(topBuf, sizeof(topBuf), "none");
        ::ps2xSafeLog("[vu1bad-sum] draws=%u badDraws=%u badVerts=%u top: %s\n",
                      s_ws.draws, s_ws.badDraws, s_ws.badVerts, topBuf);
        s_ws.draws = 0;
        s_ws.badDraws = 0;
        s_ws.badVerts = 0;
        s_ws.numKeys = 0;
        s_ws.lastSum = now;
    }
}

struct M8DrawEntry // PROBE-M8
{
    uint32_t serial = 0u;
    uint16_t handlerPc = 0u;
    uint16_t mscal = 0u;
    uint32_t gifOffset = 0u;
    uint8_t clipRan = 0u;
    uint32_t nVertsIn = 0u;
    uint32_t nVertsOut = 0u;
    float minW = 0.0f;
    float maxW = 0.0f;
    float maxAbsXY = 0.0f;
    static constexpr uint32_t kMaxGifVerts = 256u;
    uint32_t nGifVerts = 0u;
    int16_t gifX[kMaxGifVerts] = {};
    int16_t gifY[kMaxGifVerts] = {};
};

static constexpr size_t kM8RingSize = 4096u; // PROBE-M8
static M8DrawEntry s_m8Ring[kM8RingSize] = {}; // PROBE-M8
static std::mutex s_m8Mtx; // PROBE-M8
static thread_local uint16_t t_m8Mscal = 0u; // PROBE-M8
static thread_local uint16_t t_m8Handler = 0u; // PROBE-M8

inline void m8ParseGifXyz(const uint8_t *gifData, uint32_t gifBytes, M8DrawEntry &e) // PROBE-M8
{
    e.nGifVerts = 0u;
    if (!gifData || gifBytes < 16u)
        return;
    uint32_t off = 0u;
    while (off + 16u <= gifBytes)
    {
        uint64_t tagLo = 0ull, tagHi = 0ull;
        std::memcpy(&tagLo, gifData + off, 8);
        std::memcpy(&tagHi, gifData + off + 8u, 8);
        off += 16u;
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFull);
        const bool eop = ((tagLo >> 15) & 1ull) != 0ull;
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3ull);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFull);
        if (nreg == 0u)
            nreg = 16u;
        if (flg == 0u) // PACKED
        {
            for (uint32_t l = 0u; l < nloop; ++l)
            {
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (off + 16u > gifBytes)
                        return;
                    const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                    if ((reg == 0x04u || reg == 0x05u) && e.nGifVerts < M8DrawEntry::kMaxGifVerts)
                    {
                        uint32_t xw = 0u, yw = 0u;
                        std::memcpy(&xw, gifData + off + 0u, 4);
                        std::memcpy(&yw, gifData + off + 4u, 4);
                        e.gifX[e.nGifVerts] = static_cast<int16_t>(xw & 0xFFFFu);
                        e.gifY[e.nGifVerts] = static_cast<int16_t>(yw & 0xFFFFu);
                        ++e.nGifVerts;
                    }
                    off += 16u;
                }
            }
        }
        else if (flg == 1u) // REGLIST
        {
            for (uint32_t l = 0u; l < nloop; ++l)
            {
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (off + 8u > gifBytes)
                        return;
                    const uint8_t reg = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
                    if ((reg == 0x04u || reg == 0x05u) && e.nGifVerts < M8DrawEntry::kMaxGifVerts)
                    {
                        uint64_t qw = 0ull;
                        std::memcpy(&qw, gifData + off, 8);
                        e.gifX[e.nGifVerts] = static_cast<int16_t>(qw & 0xFFFFull);
                        e.gifY[e.nGifVerts] = static_cast<int16_t>((qw >> 16) & 0xFFFFull);
                        ++e.nGifVerts;
                    }
                    off += 8u;
                }
            }
            if ((nloop * nreg) & 1u)
                off += 8u;
        }
        else if (flg == 2u) // IMAGE
        {
            off += nloop * 16u;
        }
        else
        {
            break;
        }
        if (eop)
            break;
    }
}

inline void m8RecordKick(uint32_t serial, uint16_t handlerPc, uint16_t mscal, uint32_t gifOffset,
                         const uint8_t *gifData, uint32_t gifBytes) // PROBE-M8
{
    if (!spikeOn() || serial == 0u)
        return;
    M8DrawEntry e{};
    e.serial = serial;
    e.handlerPc = handlerPc;
    e.mscal = mscal;
    e.gifOffset = gifOffset;
    m8ParseGifXyz(gifData, gifBytes, e);
    std::lock_guard<std::mutex> lk(s_m8Mtx);
    s_m8Ring[serial & (kM8RingSize - 1u)] = e;
}

inline void m8EnrichDraw(uint32_t serial, uint16_t handlerPc, uint8_t clipRan, uint32_t nVertsIn,
                         uint32_t nVertsOut, float minW, float maxW, float maxAbsXY) // PROBE-M8
{
    if (!spikeOn() || serial == 0u)
        return;
    std::lock_guard<std::mutex> lk(s_m8Mtx);
    M8DrawEntry &e = s_m8Ring[serial & (kM8RingSize - 1u)];
    if (e.serial != serial)
        return;
    e.handlerPc = handlerPc;
    e.clipRan = clipRan;
    e.nVertsIn = nVertsIn;
    e.nVertsOut = nVertsOut;
    e.minW = minW;
    e.maxW = maxW;
    e.maxAbsXY = maxAbsXY;
}

inline void m8AccumVert(__m128 vp, bool &first, float &minW, float &maxW, float &maxAbsXY) // PROBE-M8
{
    alignas(16) float p[4];
    _mm_store_ps(p, vp);
    if (first)
    {
        minW = maxW = p[3];
        first = false;
    }
    else
    {
        if (p[3] < minW) minW = p[3];
        if (p[3] > maxW) maxW = p[3];
    }
    const float ax = std::fabs(p[0]);
    const float ay = std::fabs(p[1]);
    if (ax > maxAbsXY) maxAbsXY = ax;
    if (ay > maxAbsXY) maxAbsXY = ay;
}

struct M9PlaneTrace // PROBE-M9
{
    int planeIdx = 0;
    int32_t vin = 0;
    int32_t vout = 0;
    char sides[20] = {};
    char tList[128] = {};
};

struct M9State // PROBE-M9
{
    std::mutex mtx;
    uint32_t printedLines = 0u;
    uint32_t clipCalls = 0u;
    uint32_t badCalls = 0u;
    uint32_t byForm[4] = {};
    bool init = false;
    std::chrono::steady_clock::time_point lastSum{};
};

inline M9State &m9State() // PROBE-M9
{
    static M9State s_m9;
    return s_m9;
}

inline const char *m9FormName(uint32_t clipMode, bool copyS) // PROBE-M9
{
    static constexpr const char *kForms[4] = {
        "m0_c0_-Z+Z-X+X-Y+Y",
        "m0_c1_-Z+Z-X+X-Y+Y",
        "m1_c0_-Z+Z-X+X-Y+Y",
        "m1_c1_-Z+Z-X+X-Y+Y",
    };
    return kForms[((clipMode & 1u) << 1) | (copyS ? 1u : 0u)];
}

inline void m9RecordAndMaybeSummary(uint32_t clipMode, bool copyS, bool isBad) // PROBE-M9
{
    M9State &st = m9State();
    std::lock_guard<std::mutex> lk(st.mtx);
    ++st.clipCalls;
    if (isBad)
    {
        ++st.badCalls;
        ++st.byForm[((clipMode & 1u) << 1) | (copyS ? 1u : 0u)];
    }
    const auto now = std::chrono::steady_clock::now();
    if (!st.init)
    {
        st.init = true;
        st.lastSum = now;
        return;
    }
    if (std::chrono::duration<double>(now - st.lastSum).count() < 5.0)
        return;
    char formBuf[256];
    int pos = 0;
    formBuf[0] = '\0';
    for (uint32_t i = 0; i < 4u; ++i)
    {
        if (st.byForm[i] == 0u)
            continue;
        pos += std::snprintf(formBuf + pos, sizeof(formBuf) - static_cast<size_t>(pos),
                             "%s%s=%u", pos ? " " : "", m9FormName(i >> 1, (i & 1u) != 0u), st.byForm[i]);
    }
    if (pos == 0)
        std::snprintf(formBuf, sizeof(formBuf), "none=0");
    ::ps2xSafeLog("[clipbad-sum] clipCalls=%u badCalls=%u byForm: %s\n", st.clipCalls, st.badCalls, formBuf);
    st.lastSum = now;
}

inline void m9DumpBad(uint32_t serial, uint16_t mscal, uint32_t clipMode, bool copyS, int32_t vi7, int32_t vout, // PROBE-M9
                      const __m128 inPos[3], const __m128 inCol[3], const __m128 inTex[3], const uint32_t inFlags[3], const float inFog[3],
                      const __m128 outPos[16], const __m128 outCol[16], const __m128 outTex[16], const float outFog[16],
                      const char outSrc[16][64], int32_t unsetSlot, const M9PlaneTrace planes[6], int nPlanes)
{
    static constexpr const char *kPlaneNames[6] = {"-Z", "+Z", "-X", "+X", "-Y", "+Y"};
    M9State &st = m9State();
    std::lock_guard<std::mutex> lk(st.mtx);
    auto emit = [&](const char *fmt, ...) {
        if (st.printedLines >= 40u)
            return;
        ++st.printedLines;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ::ps2xSafeLog("%s\n", buf);
    };
    emit("[clipbad] serial=%u mscal=0x%04x form=%s vin=3 vout=%u",
         serial, static_cast<unsigned>(mscal), m9FormName(clipMode, copyS), static_cast<unsigned>(vout));
    for (int i = 0; i < 3; ++i)
    {
        alignas(16) float p[4], c[4], t[4];
        _mm_store_ps(p, inPos[i]);
        _mm_store_ps(c, inCol[i]);
        _mm_store_ps(t, inTex[i]);
        const int adc = (i == 2) ? ((vi7 >> 15) & 1) : 0;
        emit("  in[%d] pos=(%g,%g,%g,%g) clipflags=0x%x adc=%d tex=(%g,%g,%g) col=(%g,%g,%g,%g) fog=%g",
             i, p[0], p[1], p[2], p[3], inFlags[i], adc, t[0], t[1], t[2], c[0], c[1], c[2], c[3], inFog[i]);
    }
    for (int32_t j = 0; j < vout && j < 16; ++j)
    {
        alignas(16) float p[4], c[4], t[4];
        _mm_store_ps(p, outPos[j]);
        _mm_store_ps(c, outCol[j]);
        _mm_store_ps(t, outTex[j]);
        emit("  out[%d] pos=(%g,%g,%g,%g) tex=(%g,%g,%g) col=(%g,%g,%g,%g) fog=%g src=%s",
             j, p[0], p[1], p[2], p[3], t[0], t[1], t[2], c[0], c[1], c[2], c[3], outFog[j], outSrc[j]);
    }
    for (int p = 0; p < nPlanes; ++p)
    {
        emit("  plane[%d](%s): vin=%d vout=%d sides=%s t=[%s]",
             planes[p].planeIdx, kPlaneNames[planes[p].planeIdx], planes[p].vin, planes[p].vout, planes[p].sides, planes[p].tList);
    }
    if (unsetSlot >= 0)
        emit("  note: unset-output-slot=%d", unsetSlot);
}
} // namespace watch

// Handler 00A8: matrix setup (00A8..0150) + table transform loop (0158..0180), docs/VU1_HANDLER_FORMS.md sections 1, 2 (verified bit-exact there).
// Loop memory order inside one pass: SQI vf31 (0158), LQ.xy (0160), SQI vf6 (0168), LQI.zw (0170); the vf5 that pass k multiplies holds the xy/zw
// loaded by pass k-1 (pass 1: the LQI of 0138), and the vf6 stored by pass k is the MADDw result of pass k-1 (pass 1: the prologue MADDw at 0150).
static bool handler00A8(Regs &R, uint8_t *vu)
{
    const __m128 zero = _mm_setzero_ps();
    __m128 vf3 = ldq(vu, 980), vf1 = ldq(vu, 998), vf5 = ldq(vu, 999), vf7 = ldq(vu, 1000), vf4 = ldq(vu, 981), vf6 = ldq(vu, 1001);
    R.vf[31] = zero; // SUB.xyzw vf31, vf0, vf0
    __m128 acc = fmacV<4>(vf1, bc<0>(vf3), zero);
    acc = fmacV<2>(vf5, bc<1>(vf3), acc);
    vf3 = fmacV<2>(vf7, bc<2>(vf3), acc); // R2
    const uint16_t vi3 = ilw(vu, 1011, 0), vi2 = ilw(vu, 1011, 3);
    acc = fmacV<4>(vf1, bc<0>(vf4), zero);
    acc = fmacV<2>(vf5, bc<1>(vf4), acc);
    acc = fmacV<2>(vf7, bc<2>(vf4), acc);
    vf4 = fmacV<2>(vf6, bc<3>(vf4), acc); // R3
    __m128 vf2 = ldq(vu, 979);
    const __m128 vf8 = ldq(vu, 978);
    acc = fmacV<4>(vf1, bc<0>(vf2), zero);
    acc = fmacV<2>(vf5, bc<1>(vf2), acc);
    vf2 = fmacV<2>(vf7, bc<2>(vf2), acc); // R1
    acc = fmacV<4>(vf1, bc<0>(vf8), zero);
    acc = fmacV<2>(vf5, bc<1>(vf8), acc);
    vf1 = fmacV<2>(vf7, bc<2>(vf8), acc); // R0
    if (scene::on()) // PERF-SA
        scene::setObjMatrix(vf1, vf2, vf3, vf4);
    if (watch::on()) // PROBE-M5
        watch::recordBone(vu, vi3, vi2);
    uint16_t vi5 = static_cast<uint16_t>(vi3 + 636u), vi1 = static_cast<uint16_t>(vi3 + 476u);
    const __m128 vf25 = ldq(vu, vi1++);
    vf5 = ldq(vu, vi1++);
    acc = fmacV<4>(vf1, bc<0>(vf25), zero);
    acc = fmacV<2>(vf2, bc<1>(vf25), acc);
    acc = fmacV<2>(vf3, bc<2>(vf25), acc);
    vf6 = fmacV<2>(vf4, bc<3>(vf25), acc);
    const uint16_t vi6 = static_cast<uint16_t>(vi5 + vi2);
    uint16_t vi7 = static_cast<uint16_t>(vi3 + 716u);
    if (vi2 == 0u || vi2 > 512u) // 0 would be 65536 passes; anything this large is not a real table transform
        return false;
    if (vi2 <= 80u) // PERF-V1G: vi1 (477..477+vi2), vi5 (636..636+vi2-1), vi7 (716..716+vi2-1) are disjoint; 8-wide exact compute + bulk logRange/stqRaw
    {
        __m128 in[80], out[80];
        in[0] = vf5;
        for (uint16_t k = 1u; k < vi2; ++k)
            in[k] = ldq(vu, static_cast<uint32_t>(vi1) + (k - 1u));
        const Fv8V k1 = fv8Const(vf1), k2 = fv8Const(vf2), k3 = fv8Const(vf3), k4 = fv8Const(vf4);
        Fv8V z8;
        z8.c[0] = z8.c[1] = z8.c[2] = z8.c[3] = _mm256_set_m128(zero, zero);
        for (uint16_t k0 = 0; k0 < vi2; k0 = static_cast<uint16_t>(k0 + 8u))
        {
            const uint16_t n = (vi2 - k0 < 8u) ? static_cast<uint16_t>(vi2 - k0) : 8u;
            __m128 rows[8];
            for (uint16_t j = 0; j < 8u; ++j)
                rows[j] = (j < n) ? in[k0 + j] : zero;
            Fv8V C = fv8FromRows(rows);
            for (int c = 0; c < 4; ++c)
                C.c[c] = fv8N(C.c[c]);
            __m256i bad = _mm256_setzero_si256();
            const Fv8V a0 = fv8Op<15, false>(k1, Fv8V{{C.c[0], C.c[0], C.c[0], C.c[0]}}, z8, bad);
            const Fv8V a1 = fv8Op<15, true>(k2, Fv8V{{C.c[1], C.c[1], C.c[1], C.c[1]}}, a0, bad);
            const Fv8V a2 = fv8Op<15, true>(k3, Fv8V{{C.c[2], C.c[2], C.c[2], C.c[2]}}, a1, bad);
            const Fv8V r6 = fv8Op<15, true>(k4, Fv8V{{C.c[3], C.c[3], C.c[3], C.c[3]}}, a2, bad);
            if (!fv8Any(bad))
            {
                __m128 oRows[8];
                fv8ToRows(r6, oRows);
                for (uint16_t j = 0; j < n; ++j)
                    out[k0 + j] = oRows[j];
                if (k0 + n == vi2)
                {
                    __m128 aRows[8];
                    fv8ToRows(a2, aRows);
                    acc = aRows[n - 1u];
                }
            }
            else
            {
                for (uint16_t j = 0; j < n; ++j)
                {
                    const __m128 cur = in[k0 + j];
                    acc = fmacV<4>(vf1, bc<0>(cur), zero);
                    acc = fmacV<2>(vf2, bc<1>(cur), acc);
                    acc = fmacV<2>(vf3, bc<2>(cur), acc);
                    out[k0 + j] = fmacV<2>(vf4, bc<3>(cur), acc);
                }
            }
        }
        logRange(vu, vi7, vi2);
        for (uint16_t k = 0; k < vi2; ++k)
            stqRaw(vu, static_cast<uint32_t>(vi7) + k, zero);
        logRange(vu, vi5, vi2);
        stqRaw(vu, vi5, vf6);
        for (uint16_t k = 1u; k < vi2; ++k)
            stqRaw(vu, static_cast<uint32_t>(vi5) + k, out[k - 1u]);
        vi1 = static_cast<uint16_t>(vi1 + vi2);
        vi5 = vi6;
        vi7 = static_cast<uint16_t>(vi7 + vi2);
        vf5 = ldq(vu, static_cast<uint32_t>(vi1) - 1u);
        vf6 = out[vi2 - 1u];
    }
    else
    {
        do
        {
            const __m128 cur = vf5;
            stq(vu, vi7++, R.vf[31]);                         // 0158 SQI vf31
            acc = fmacV<4>(vf1, bc<0>(cur), zero);            // 0158 MULAx
            acc = fmacV<2>(vf2, bc<1>(cur), acc);             // 0160 MADDAy
            const __m128 xy = ldq(vu, vi1);                   // 0160 LQ.xy vf5, 0(vi1)
            acc = fmacV<2>(vf3, bc<2>(cur), acc);             // 0168 MADDAz
            stq(vu, vi5++, vf6);                              // 0168 SQI vf6 (value of the previous pass)
            const __m128 nv6 = fmacV<2>(vf4, bc<3>(cur), acc); // 0170 MADDw vf6
            const __m128 zw = ldq(vu, vi1++);                 // 0170 LQI.zw vf5
            vf6 = nv6;
            vf5 = _mm_shuffle_ps(xy, zw, _MM_SHUFFLE(3, 2, 1, 0)); // x,y from xy ; z,w from zw
        } while (vi5 != vi6);                                 // 0178 IBNE vi6, vi5
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[5] = vf5; R.vf[6] = vf6; R.vf[7] = vf7; R.vf[8] = vf8; R.vf[25] = vf25;
    R.acc = acc;
    R.vi[1] = vi1; R.vi[2] = vi2; R.vi[3] = vi3; R.vi[5] = vi5; R.vi[6] = vi6; R.vi[7] = vi7;
    return true;
}

static uint64_t s_taken = 0, s_fallback = 0;
static std::unordered_map<uint32_t, uint64_t> s_startPcs; // startPC of 0xF001 fallbacks -> count
static std::unordered_map<uint32_t, uint64_t> s_fbPcs; // handler pc (or 0xF000 + reason) -> count
static uint32_t s_fbPc = 0;

// 1EE0 fallback reasons (counted just before returning false; printed in the periodic [vfunc] line as 1ee0_why)
enum Fb1EE0Reason : int { kFbRigidN = 0, kFbSkinN, kFbSkinAlias13, kFbSkinGatherAlias, kFbSkinDivZero, kFbSkinKick, kFbRigidClip, kFbRigidKick, kFbClipKick, kFbCount };
static const char *const kFb1EE0Names[kFbCount] = {"rigidN_or_vi13", "skinN_range", "skin_alias_1012_1013_1023_vi13", "skin_gather_alias", "skin_div_zero", "skin_kick", "rigid_clipPolygon", "rigid_kick", "clip_kick"};
static uint64_t s_fb1EE0[kFbCount] = {};
static constexpr uint32_t kMaxSkinN = 255;        // 4N+1 output qwords must fit in 1024
static uint64_t s_skinNHist[7] = {};              // N hitting the skinN guard: 0,1,2,3,<100,100..199,200+
static bool fb1EE0(int reason)
{
    ++s_fb1EE0[reason];
    return false;
}

static bool kick(const uint8_t *vu, uint32_t addr, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif, bool leanTag = false) // FEAT-RD42
{
    const uint32_t q0 = addr & 0x3FFu;
    uint32_t total = 0;
    if (leanTag) // FEAT-RD42
    {
        uint64_t tagLo;
        std::memcpy(&tagLo, vu + (q0 << 4), 8);
        if (((tagLo >> 58) & 0x3u) == 3u)
        {
            s_fbPc = 0xF010u;
            return false;
        }
        total = 16u;
    }
    else
    {
        for (;;)
        {
            uint64_t tagLo;
            std::memcpy(&tagLo, vu + (((q0 + total / 16u) & 0x3FFu) << 4), 8);
            const uint64_t nloop = tagLo & 0x7FFFu;
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint64_t nreg = (tagLo >> 60) & 0xFu;
            if (nreg == 0u)
                nreg = 16u;
            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += nloop * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((nloop * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += nloop * 16u;
            else
            {
                s_fbPc = 0xF010u;
                return false;
            }
            if ((tagBytes & 15u) != 0u || tagBytes > kMaxKickBytes - total)
            {
                s_fbPc = 0xF011u;
                return false;
            }
            total += static_cast<uint32_t>(tagBytes);
            if (((tagLo >> 15) & 1u) != 0u) // EOP
                break;
        }
    }
    uint32_t start = static_cast<uint32_t>(gif.size());
    if (scene::on() && ::ps2xRdActive()) // FEAT-RD5 // FEAT-RD49
    {
        uint32_t serial = (g_rdKickSerial.fetch_add(1u, std::memory_order_relaxed) + 1u) & 0x3FFFFFFFu; // FEAT-RD10
        if (serial == 0u)
            serial = (g_rdKickSerial.fetch_add(1u, std::memory_order_relaxed) + 1u) & 0x3FFFFFFFu;
        scene::t_scene.curKickSerial = serial;
        gif.resize(start + 32u + total);
        const uint64_t mLo = 1ull | (1ull << 60); // NLOOP=1, EOP=0, FLG=0 (PACKED), NREG=1
        const uint64_t mHi = 0xEull;              // REGS[0] = A+D
        const uint64_t dLo = static_cast<uint64_t>(serial);
        const uint64_t dHi = 0x7Full;             // byte 8 = 0x7F (GS NOP)
        std::memcpy(gif.data() + start + 0u, &mLo, 8);
        std::memcpy(gif.data() + start + 8u, &mHi, 8);
        std::memcpy(gif.data() + start + 16u, &dLo, 8);
        std::memcpy(gif.data() + start + 24u, &dHi, 8);
        start += 32u;
    }
    else
    {
        gif.resize(start + total);
    }
    if (leanTag) // FEAT-RD42
    {
        std::memcpy(gif.data() + start, vu + (q0 << 4), 16);
        uint64_t tagLo;
        std::memcpy(&tagLo, gif.data() + start, 8);
        tagLo &= ~0x7FFFull; // NLOOP = 0
        std::memcpy(gif.data() + start, &tagLo, 8);
    }
    else
    {
        for (uint32_t i = 0; i < total / 16u; ++i)
            std::memcpy(gif.data() + start + i * 16u, vu + (((q0 + i) & 0x3FFu) << 4), 16);
    }
    kicks.emplace_back(start, total);
    if (watch::spikeOn() && scene::on() && ::ps2xRdActive()) // PROBE-M8
    {
        const uint32_t ks = scene::t_scene.curKickSerial & 0x3FFFFFFFu;
        watch::m8RecordKick(ks, watch::t_m8Handler, watch::t_m8Mscal, start,
                            leanTag ? nullptr : (gif.data() + start), leanTag ? 0u : total);
    }
    if (scene::on()) // FEAT-RD2
    {
        scene::RecScope rs(scene::kRpParse); // FEAT-RD32
        scene::parseChain(gif.data() + start, total);
    }
    return true;
}

// ---- Task F2: handler 1EE0 (mesh transform: skinned path 1FE0..21F8, rigid path 2440.. below) ----
// Value helpers mirroring vuhot::N / normResult / lbClamp / lbFtoi (vu1_hot_ops.h, vu1_native_hand.inc), which cannot be included in this TU.
inline float nrm1(float v)
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t e = (b >> 23) & 0xFFu;
    if (e == 0u)
        b &= 0x80000000u;
    else if (e == 0xFFu)
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&v, &b, 4);
    return v;
}
inline float normRes(float v) // vuhot::normResult
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t sign = b & 0x80000000u, mag = b & 0x7FFFFFFFu, e = (b >> 23) & 0xFFu;
    if (mag != 0u)
    {
        if (e == 0u)
            b = sign;
        else if (e == 0xFFu)
            b = sign | 0x7F7FFFFFu;
    }
    std::memcpy(&v, &b, 4);
    return v;
}
inline __m128 clampI(__m128 x) // lbClamp with I = 255.0f: MAXx.xyzw (vf0.x = 0) then MINIi.xyzw
{
    return _mm_min_ps(_mm_max_ps(norm4(x), _mm_setzero_ps()), _mm_set1_ps(255.0f));
}
template <int SCALE>
inline __m128i ftoiS(__m128 a) // lbFtoi
{
    const __m128 sc = _mm_mul_ps(norm4(a), _mm_set1_ps(static_cast<float>(SCALE)));
    const __m128i t = _mm_cvttps_epi32(sc);
    const __m128 ge = _mm_cmpge_ps(sc, _mm_set1_ps(2147483648.0f));
    return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(t), _mm_castsi128_ps(_mm_set1_epi32(INT32_MAX)), ge));
}
inline uint32_t ldw(const uint8_t *vu, uint32_t q, uint32_t lane) // raw 32-bit word
{
    uint32_t w;
    std::memcpy(&w, vu + ((q & 0x3FFu) << 4) + 4u * lane, 4);
    return w;
}
inline __m128i ldqi(const uint8_t *vu, uint32_t q) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu + ((q & 0x3FFu) << 4))); }
inline void stqi(uint8_t *vu, uint32_t q, __m128i v) { stq(vu, q, _mm_castsi128_ps(v)); }

inline uint32_t clipBitsV(__m128 v) // lbClipBits (CLIP.xyz against |w|)
{
    uint32_t w[4];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(w), _mm_castps_si128(v));
    const int32_t limit = (w[3] & 0x7F800000u) != 0u ? static_cast<int32_t>(w[3] & 0x7FFFFFFFu) : 0x007FFFFF;
    uint32_t fl = 0u;
    for (int k = 0; k < 3; ++k)
    {
        const int32_t o0 = static_cast<int32_t>(w[k]), o1 = static_cast<int32_t>(w[k] ^ 0x80000000u);
        if (o0 > limit)
            fl |= 1u << (2 * k);
        if (o1 > limit)
            fl |= 1u << (2 * k + 1);
    }
    return fl;
}
inline float divQ(__m128 t) // lbDivQ without the DI flag (flags are not reproduced): DIV Q, vf0.w, t.w
{
    const float den = nrm1(_mm_cvtss_f32(bc<3>(t)));
    float q;
    if (den == 0.0f)
        q = std::signbit(den) ? -FLT_MAX : FLT_MAX;
    else
        q = normRes(1.0f / den);
    return normRes(q);
}
inline float divQ2(float numRaw, float denRaw) // mirrors VU1Interpreter::execLower case 0x38 (DIV)
{
    const float num = nrm1(numRaw), den = nrm1(denRaw);
    float q;
    if (den == 0.0f)
        q = (std::signbit(num) != std::signbit(den)) ? -FLT_MAX : FLT_MAX;
    else
        q = num / den;
    return normRes(q);
}
inline float rsqrtQ(float numRaw, float radRaw) // PERF-V1M: mirrors VU1Interpreter::execLower case 0x3A (RSQRT)
{
    const float num = nrm1(numRaw), rad = nrm1(radRaw);
    const float den = std::sqrt(std::fabs(rad));
    float q;
    if (den != 0.0f)
        q = num / den;
    else
        q = std::signbit(num) ? -FLT_MAX : FLT_MAX;
    return normRes(q);
}
inline float sqrtQ(float radRaw) // PERF-V1N: mirrors VU1Interpreter::execLower case 0x39 (SQRT)
{
    return normRes(std::sqrt(std::fabs(nrm1(radRaw))));
}
inline float erlenP(__m128 v) // PERF-V1M: mirrors VU1Interpreter::execLower case 0x73 (ERLENG) + queueP
{
    alignas(16) float f[4];
    _mm_store_ps(f, norm4(v));
    const float len = std::sqrt(std::fma(f[2], f[2], std::fma(f[1], f[1], f[0] * f[0]))); // contraction as the interpreter TU compiles it
    return normRes(len != 0.0f ? 1.0f / len : len);
}
inline __m128 maxV(__m128 a, __m128 b) // mirrors VU1Interpreter::execUpper MAX/MAXbc/MAXi
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    return _mm_blendv_ps(b4, a4, _mm_cmpgt_ps(a4, b4));
}
inline __m128 miniV(__m128 a, __m128 b) // mirrors VU1Interpreter::execUpper MINI/MINIbc/MINIi
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    return _mm_blendv_ps(b4, a4, _mm_cmplt_ps(a4, b4));
}
inline __m128 absV(__m128 v) // mirrors VU1Interpreter::execUpper case 0x1D (ABS)
{
    return _mm_and_ps(norm4(v), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
}
inline __m128 mr32(__m128 v) // mirrors VU1Interpreter::execLower case 0x31 (MR32: xyzw -> yzwx)
{
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1));
}

namespace scene // PERF-SA
{
inline void checkDraw(const SceneDraw &d, const SceneVertex *verts)
{
    const __m128 zero = _mm_setzero_ps();
    const __m128 c1 = _mm_loadu_ps(d.vpScale);
    const __m128 c2 = _mm_loadu_ps(d.vpOffset);
    const int slot = handlerSlot(d.handlerPc);
    for (uint32_t i = 0; i < d.vertexCount; ++i)
    {
        const SceneVertex &v = verts[i];
        const __m128 p = _mm_loadu_ps(v.pos);
        const __m128 mula = fmacV<4>(c1, p, zero);
        const __m128 tv = fmacV<2>(c2, bc<3>(p), mula);
        const float q = divQ(tv);
        const __m128 scr = fmacV<4>(tv, _mm_set1_ps(q), zero);
        const __m128i xyz = ftoiS<16>(scr);
        alignas(16) int32_t out[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(out), xyz);
        if (out[0] != v.screenXyz[0] || out[1] != v.screenXyz[1] || out[2] != v.screenXyz[2])
        {
            ++t_scene.totalMismatch;
            ++t_scene.mismatchByHandler[slot];
        }
    }
}

inline void dumpSceneFrame(const SceneFrame &f)
{
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    char binPath[128], txtPath[128];
    std::snprintf(binPath, sizeof(binPath), "logs/scene_%llu.bin", static_cast<unsigned long long>(f.frameIndex));
    std::snprintf(txtPath, sizeof(txtPath), "logs/scene_%llu.txt", static_cast<unsigned long long>(f.frameIndex));

    if (FILE *fb = std::fopen(binPath, "wb"))
    {
        SceneBinHeader hdr{};
        hdr.magic = 0x314E4353u; // 'SCN1'
        hdr.version = 2u; // FEAT-V2
        hdr.frameIndex = static_cast<uint32_t>(f.frameIndex);
        hdr.drawCount = static_cast<uint32_t>(f.draws.size());
        hdr.vertexCount = static_cast<uint32_t>(f.vertices.size());
        std::fwrite(&hdr, sizeof(hdr), 1u, fb);
        if (!f.draws.empty())
            std::fwrite(f.draws.data(), sizeof(SceneDraw), f.draws.size(), fb);
        if (!f.vertices.empty())
            std::fwrite(f.vertices.data(), sizeof(SceneVertex), f.vertices.size(), fb);
        const SceneBinSectionHeader lightSec{0x5448474Cu, static_cast<uint32_t>(f.lights.size()), sizeof(SceneLight)}; // 'LGHT' // FEAT-V2
        std::fwrite(&lightSec, sizeof(lightSec), 1u, fb); // FEAT-V2
        if (!f.lights.empty()) // FEAT-V2
            std::fwrite(f.lights.data(), sizeof(SceneLight), f.lights.size(), fb); // FEAT-V2
        const SceneBinSectionHeader globSec{0x424F4C47u, static_cast<uint32_t>(f.globals.size()), sizeof(SceneGlobals)}; // 'GLOB' // FEAT-V2
        std::fwrite(&globSec, sizeof(globSec), 1u, fb); // FEAT-V2
        if (!f.globals.empty()) // FEAT-V2
            std::fwrite(f.globals.data(), sizeof(SceneGlobals), f.globals.size(), fb); // FEAT-V2
        std::fclose(fb);
    }

    if (FILE *ft = std::fopen(txtPath, "w"))
    {
        static const uint32_t kPcs[4] = {0x1EE0u, 0x1EF0u, 0x2440u, 0x2900u};
        static const char *const kNames[4] = {"1EE0(skin)", "1EF0(skin_uv)", "2440(rigid)", "2900(clip)"};
        uint64_t hDraws[4] = {}, hVerts[4] = {};
        float minP[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
        float maxP[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const SceneDraw &d : f.draws)
        {
            const int s = handlerSlot(d.handlerPc);
            ++hDraws[s];
            hVerts[s] += d.vertexCount;
        }
        for (const SceneVertex &v : f.vertices)
        {
            for (int c = 0; c < 4; ++c)
            {
                if (v.pos[c] < minP[c]) minP[c] = v.pos[c];
                if (v.pos[c] > maxP[c]) maxP[c] = v.pos[c];
            }
        }
        std::fprintf(ft, "Scene buffer v1 dump: frame=%llu mscals=%u draws=%zu verts=%zu gifBytes=%u checkMismatch=%llu\n",
                     static_cast<unsigned long long>(f.frameIndex), f.mscalCount, f.draws.size(), f.vertices.size(),
                     f.gifBytes, static_cast<unsigned long long>(t_scene.totalMismatch));
        if (f.vertices.empty())
            std::fprintf(ft, "bbox: (empty)\n");
        else
            std::fprintf(ft, "bbox(clip pos): min=(%.3f, %.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f, %.3f)\n",
                         minP[0], minP[1], minP[2], minP[3], maxP[0], maxP[1], maxP[2], maxP[3]);
        std::fprintf(ft, "\nPer-handler counts:\n");
        for (int s = 0; s < 4; ++s)
            std::fprintf(ft, "  0x%04X %-14s draws=%llu verts=%llu mismatch=%llu\n",
                         kPcs[s], kNames[s], static_cast<unsigned long long>(hDraws[s]),
                         static_cast<unsigned long long>(hVerts[s]), static_cast<unsigned long long>(t_scene.mismatchByHandler[s]));
        for (int s = 0; s < 4; ++s)
        {
            if (hVerts[s] == 0u)
                continue;
            std::fprintf(ft, "\nSample vertices for 0x%04X %s:\n", kPcs[s], kNames[s]);
            int shown = 0;
            for (const SceneDraw &d : f.draws)
            {
                if (handlerSlot(d.handlerPc) != s)
                    continue;
                for (uint32_t i = 0; i < d.vertexCount && shown < 5; ++i, ++shown)
                {
                    const SceneVertex &v = f.vertices[d.firstVertex + i];
                    std::fprintf(ft, "  [%d] pos=(%.3f,%.3f,%.3f,%.3f) nrm=(%.3f,%.3f,%.3f) stq=(%.4f,%.4f,%.4f) rgba=(%u,%u,%u,%u) xyz=(%d,%d,%d) adc=0x%X\n",
                                 shown, v.pos[0], v.pos[1], v.pos[2], v.pos[3],
                                 v.normal[0], v.normal[1], v.normal[2],
                                 v.stq[0], v.stq[1], v.stq[2],
                                 v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3],
                                 v.screenXyz[0], v.screenXyz[1], v.screenXyz[2], v.adc);
                }
                if (shown >= 5)
                    break;
            }
        }
        std::fclose(ft);
    }
}

inline void onFlush()
{
    // PERF-SA: advance the double buffer only when a new frame was presented, not on every VIF1 drain
    const uint64_t pseq = ::g_hostPresentSeq.load(std::memory_order_relaxed);
    if (pseq == t_scene.lastPresentSeq)
        return;
    t_scene.lastPresentSeq = pseq;
    SceneFrame &cur = t_scene.bufs[t_scene.writeIdx];
    const bool lean = leanRdOn(); // FEAT-RD14
    if ((lean ? cur.rdBatches.empty() : cur.draws.empty()) && t_scene.mscalIndex == 0u)
        return;
    cur.frameIndex = t_scene.frameSeq++;
    cur.mscalCount = t_scene.mscalIndex;
    cur.gifBytes = t_scene.frameGifBytes;
    ++t_scene.totalFrames;
    if (lean) // FEAT-RD14
    {
        t_scene.totalDraws += cur.leanDraws;
        t_scene.totalVerts += cur.rdVerts.size();
        cur.hwVerts = std::max(cur.hwVerts, cur.rdVerts.size()); // FEAT-RD32
        cur.hwIdx = std::max(cur.hwIdx, cur.rdIndices.size());
        cur.hwBatches = std::max(cur.hwBatches, cur.rdBatches.size());
        const size_t profVerts = cur.rdVerts.size();
        {
            RecScope rs(kRpPublish); // FEAT-RD32
            ::ps2xRdPublishLean(cur.rdVerts, cur.rdIndices, cur.rdBatches, 0u, cur.maskedDraws, cur.vpScaleZ, cur.vpOffsetZ);
        }
        if (recProfOn())
            recProfFrame(profVerts);
    }
    else
    {
        t_scene.totalDraws += cur.draws.size();
        t_scene.totalVerts += cur.vertices.size();
        { // FEAT-RD2
            static thread_local std::vector<uint64_t> t_tex0s;
            t_tex0s.clear();
            t_tex0s.reserve(cur.gs.size());
            for (const SceneGs &g : cur.gs)
            {
                if (g.ownTex0) ++t_scene.totalOwnTex0;
                if ((g.prim >> 4) & 1ull) ++t_scene.totalTme;
                if ((g.prim >> 6) & 1ull) ++t_scene.totalAbe;
                t_tex0s.push_back(g.tex0);
            }
            std::sort(t_tex0s.begin(), t_tex0s.end());
            t_scene.totalDistinctTex0 += static_cast<uint64_t>(std::unique(t_tex0s.begin(), t_tex0s.end()) - t_tex0s.begin());
        }

        const int64_t df = dumpFrame();
        if (df >= 0 && !t_scene.dumped && cur.frameIndex == static_cast<uint64_t>(df))
        {
            dumpSceneFrame(cur);
            t_scene.dumped = true;
        }
    }

    if (!lean && ::ps2xRdActive()) // FEAT-RD1 // FEAT-RD49
    {
        static thread_local std::vector<::RdVert> t_rdTris;
        static thread_local std::vector<::RdBatch> t_rdBatches; // FEAT-RD2
        t_rdTris.clear();
        t_rdBatches.clear();
        uint32_t skippedDraws = 0;
        uint32_t maskedDraws = 0; // FEAT-RD2
        uint32_t bestVpVerts = 0; // FEAT-RD11
        float vpScaleZ = 0.0f;    // FEAT-RD11
        float vpOffsetZ = 0.0f;   // FEAT-RD11
        const auto toRdVert = [](const SceneVertex &sv) -> ::RdVert {
            ::RdVert rv;
            rv.pos[0] = sv.pos[0];
            rv.pos[1] = sv.pos[1];
            rv.pos[2] = sv.pos[2];
            rv.pos[3] = sv.pos[3];
            for (int c = 0; c < 4; ++c)
                rv.rgba[c] = static_cast<uint8_t>(std::min<uint32_t>(sv.rgba[c], 255u));
            const float q = sv.stq[2]; // FEAT-RD3
            if (q != 0.0f)
            {
                rv.uv[0] = sv.stq[0] / q;
                rv.uv[1] = sv.stq[1] / q;
            }
            else
            {
                rv.uv[0] = 0.0f;
                rv.uv[1] = 0.0f;
            }
            const uint32_t fByte = (sv.adc >> 4) & 0xFFu; // FIX-E1b
            rv.fog = static_cast<float>(fByte) / 255.0f;  // FIX-E1b
            return rv;
        };
        for (size_t di = 0; di < cur.draws.size(); ++di) // FEAT-RD2
        {
            const SceneDraw &d = cur.draws[di];
            const SceneGs &g = cur.gs[di];
            if (d.vertexCount > bestVpVerts && d.vpScale[2] != 0.0f) // FEAT-RD11
            {
                bestVpVerts = d.vertexCount;
                vpScaleZ = d.vpScale[2];
                vpOffsetZ = d.vpOffset[2];
            }
            if (static_cast<uint32_t>(g.frame >> 32) == 0xFFFFFFFFu) // FEAT-RD2: FRAME.FBMSK == 0xFFFFFFFF
            {
                ++maskedDraws;
                continue;
            }
            const uint32_t batchFirst = static_cast<uint32_t>(t_rdTris.size());
            if (d.flags & 4u) // FEAT-RD4: clipped convex polygon -> triangle fan
            {
                for (uint32_t k = 2; k < d.vertexCount; ++k)
                {
                    t_rdTris.push_back(toRdVert(cur.vertices[d.firstVertex]));
                    t_rdTris.push_back(toRdVert(cur.vertices[d.firstVertex + k - 1u]));
                    t_rdTris.push_back(toRdVert(cur.vertices[d.firstVertex + k]));
                }
            }
            else
            for (uint32_t i = 2; i < d.vertexCount; ++i)
            {
                const SceneVertex &sv2 = cur.vertices[d.firstVertex + i];
                if (sv2.adc & 0x8000u)
                    continue;
                t_rdTris.push_back(toRdVert(cur.vertices[d.firstVertex + i - 2u]));
                t_rdTris.push_back(toRdVert(cur.vertices[d.firstVertex + i - 1u]));
                t_rdTris.push_back(toRdVert(sv2));
            }
            const uint32_t batchCount = static_cast<uint32_t>(t_rdTris.size()) - batchFirst;
            if (batchCount == 0u)
                continue;
            if (!t_rdBatches.empty())
            {
                ::RdBatch &last = t_rdBatches.back();
                if (rdSerialsCanMerge(last.kickSerial, g.kickSerial) &&
                    last.alpha == g.alpha && last.test == g.test && last.frame == g.frame && last.zbuf == g.zbuf &&
                    last.prim == g.prim && last.tex0 == g.tex0 && last.clamp == g.clamp && last.tex1 == g.tex1 &&
                    last.texa == g.texa &&
                    last.fogcol == g.fogcol && // FIX-E1c
                    last.vpS[0] == d.vpScale[0] && last.vpS[1] == d.vpScale[1] && last.vpO[0] == d.vpOffset[0] && last.vpO[1] == d.vpOffset[1]) // FEAT-RD3 // FEAT-RD17 // FEAT-RD21
                {
                    if (last.kickSerial != g.kickSerial) // FEAT-RD17
                        ::g_rdMergedSpan.fetch_add(1u, std::memory_order_relaxed);
                    last.count += batchCount;
                    continue;
                }
            }
            t_rdBatches.push_back(::RdBatch{batchFirst, batchCount, 0u, g.alpha, g.test, g.frame, g.zbuf, g.prim, g.tex0,
                                            g.clamp, g.tex1, g.texa, g.kickSerial,
                                            {d.vpScale[0], d.vpScale[1]}, {d.vpOffset[0], d.vpOffset[1]}, 0u, g.fogcol}); // FEAT-RD3 // FEAT-RD5 // FEAT-RD14 // FEAT-RD21 // FIX-E1c
        }
        ::ps2xRdPublish(t_rdTris.data(), t_rdTris.size(), t_rdBatches.data(), t_rdBatches.size(), skippedDraws, maskedDraws, vpScaleZ, vpOffsetZ); // FEAT-RD11
    }

    t_scene.slDraws += static_cast<uint32_t>(cur.draws.size()); // FEAT-V2
    t_scene.slLights += static_cast<uint32_t>(cur.lights.size()); // FEAT-V2
    for (const SceneDraw &d : cur.draws) // FEAT-V2
    {
        if (d.lightCount > t_scene.slMaxLights)
            t_scene.slMaxLights = d.lightCount;
        if (!t_scene.slHasFirst && d.globalsIndex < cur.globals.size())
        {
            const SceneGlobals &g = cur.globals[d.globalsIndex];
            t_scene.slAmb0[0] = g.ambient[0]; t_scene.slAmb0[1] = g.ambient[1]; t_scene.slAmb0[2] = g.ambient[2];
            t_scene.slScale0[0] = g.lightScale[0]; t_scene.slScale0[1] = g.lightScale[1]; t_scene.slScale0[2] = g.lightScale[2];
            t_scene.slHasFirst = true;
        }
    }

    t_scene.writeIdx ^= 1u;
    t_scene.bufs[t_scene.writeIdx].clear();
    t_scene.nextDrawLight = 0u; // FEAT-V2
    t_scene.mscalIndex = 0u;
    t_scene.frameGifBytes = 0u;

    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t_scene.lastLightLog).count() >= 1000) // FEAT-V2
    {
        t_scene.lastLightLog = now;
        ::ps2xSafeLog("[scenelight] draws=%u lights=%u maxLightsPerDraw=%u ambient0=(%f,%f,%f) lightScale0=(%f,%f,%f)\n",
                      t_scene.slDraws, t_scene.slLights, t_scene.slMaxLights,
                      t_scene.slAmb0[0], t_scene.slAmb0[1], t_scene.slAmb0[2],
                      t_scene.slScale0[0], t_scene.slScale0[1], t_scene.slScale0[2]);
        t_scene.slDraws = 0u;
        t_scene.slLights = 0u;
        t_scene.slMaxLights = 0u;
        t_scene.slHasFirst = false;
        t_scene.slAmb0[0] = t_scene.slAmb0[1] = t_scene.slAmb0[2] = 0.0f;
        t_scene.slScale0[0] = t_scene.slScale0[1] = t_scene.slScale0[2] = 0.0f;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t_scene.lastLog).count() >= 5000)
    {
        t_scene.lastLog = now;
        const double fr = t_scene.totalFrames ? static_cast<double>(t_scene.totalFrames) : 1.0;
        const double dr = t_scene.totalDraws ? static_cast<double>(t_scene.totalDraws) : 1.0; // FEAT-RD2
        ::ps2xSafeLog("[scene] frames=%llu draws/frame=%.1f verts/frame=%.1f checkMismatch=%llu ownTex0=%.1f%% tex0/frame=%.1f tme=%.1f%% abe=%.1f%% fb=%llu\n",
                      static_cast<unsigned long long>(t_scene.totalFrames),
                      static_cast<double>(t_scene.totalDraws) / fr,
                      static_cast<double>(t_scene.totalVerts) / fr,
                      static_cast<unsigned long long>(t_scene.totalMismatch),
                      100.0 * static_cast<double>(t_scene.totalOwnTex0) / dr,
                      static_cast<double>(t_scene.totalDistinctTex0) / fr,
                      100.0 * static_cast<double>(t_scene.totalTme) / dr,
                      100.0 * static_cast<double>(t_scene.totalAbe) / dr,
                      static_cast<unsigned long long>(t_scene.totalFallbacks));
    }
}

inline void recordLight1(uint32_t pc, __m128 d0, __m128 d1, __m128 c0, __m128 params) // FEAT-V2
{
    SceneLight l{};
    l.type = pc;
    _mm_storeu_ps(l.dirOrPos[0], d0);
    _mm_storeu_ps(l.dirOrPos[1], d1);
    _mm_storeu_ps(l.colour[0], c0);
    _mm_storeu_ps(l.params, params);
    t_scene.bufs[t_scene.writeIdx].lights.push_back(l);
}

inline void recordLight4(uint32_t pc, __m128 d0, __m128 d1, __m128 d2, __m128 c0, __m128 c1, __m128 c2, __m128 c3, __m128 params) // FEAT-V2
{
    SceneLight l{};
    l.type = pc;
    _mm_storeu_ps(l.dirOrPos[0], d0);
    _mm_storeu_ps(l.dirOrPos[1], d1);
    _mm_storeu_ps(l.dirOrPos[2], d2);
    _mm_storeu_ps(l.colour[0], c0);
    _mm_storeu_ps(l.colour[1], c1);
    _mm_storeu_ps(l.colour[2], c2);
    _mm_storeu_ps(l.colour[3], c3);
    _mm_storeu_ps(l.params, params);
    t_scene.bufs[t_scene.writeIdx].lights.push_back(l);
}

inline SceneDraw &pushDraw(const uint8_t *vu, SceneFrame &f, uint32_t pc, uint32_t gifOff, uint32_t gifLen, uint32_t count, uint32_t boneCount, uint32_t flags, __m128 c1v, __m128 c2v) // FEAT-V2
{
    if (t_scene.mscalGlobalsIdx == UINT32_MAX) // FEAT-V2
    {
        SceneGlobals g{};
        for (uint32_t r = 0u; r < 4u; ++r)
            _mm_storeu_ps(g.viewProj + r * 4u, ldq(vu, 998u + r));
        _mm_storeu_ps(g.vpScale, ldq(vu, 1002u));
        _mm_storeu_ps(g.vpOffset, ldq(vu, 1003u));
        _mm_storeu_ps(g.ambient, ldq(vu, 1014u));
        _mm_storeu_ps(g.lightScale, ldq(vu, 1013u));
        _mm_storeu_ps(g.fogParams, ldq(vu, 1016u));
        t_scene.mscalGlobalsIdx = static_cast<uint32_t>(f.globals.size());
        f.globals.push_back(g);
    }
    SceneDraw d{};
    d.handlerPc = pc;
    d.mscalIndex = t_scene.mscalIndex;
    d.gifOffset = t_scene.frameGifBytes + gifOff;
    d.gifLength = gifLen;
    d.firstVertex = static_cast<uint32_t>(f.vertices.size());
    d.vertexCount = count;
    d.boneCount = boneCount;
    d.flags = flags;
    std::memcpy(d.objMatrix, t_scene.curObjMatrix, sizeof(d.objMatrix));
    _mm_storeu_ps(d.vpScale, c1v);
    _mm_storeu_ps(d.vpOffset, c2v);
    d.firstLight = t_scene.nextDrawLight; // FEAT-V2
    d.lightCount = static_cast<uint32_t>(f.lights.size()) - t_scene.nextDrawLight; // FEAT-V2
    d.globalsIndex = t_scene.mscalGlobalsIdx; // FEAT-V2
    d.firstBone = 0u; // FEAT-V2
    t_scene.nextDrawLight = static_cast<uint32_t>(f.lights.size()); // FEAT-V2
    f.draws.push_back(d);
    const uint32_t ctxt = static_cast<uint32_t>((t_scene.gs.prim >> 9) & 1ull); // FEAT-RD2
    SceneGs g{};
    g.prim = t_scene.gs.prim;
    g.tex0 = t_scene.gs.tex0[ctxt];
    g.clamp = t_scene.gs.clamp[ctxt];
    g.tex1 = t_scene.gs.tex1[ctxt];
    g.alpha = t_scene.gs.alpha[ctxt];
    g.test = t_scene.gs.test[ctxt];
    g.frame = t_scene.gs.frame[ctxt];
    g.zbuf = t_scene.gs.zbuf[ctxt];
    g.texa = t_scene.gs.texa;
    g.texclut = t_scene.gs.texclut;
    g.fba = static_cast<uint32_t>(t_scene.gs.fba[ctxt] & 1ull);
    g.ownTex0 = t_scene.curOwnTex0 ? 1u : 0u;
    g.kickSerial = t_scene.curKickSerial; // FEAT-RD5
    g.fogcol = t_scene.gs.fogcol; // FIX-E1c
    f.gs.push_back(g);
    return f.draws.back();
}

inline void markGeometryKick(std::vector<uint8_t> &gif, uint32_t gifOff) // FEAT-RD10
{
    if (::ps2xRdActive() && gifOff >= 16u && t_scene.curKickSerial != 0u) // FEAT-RD49
    {
        t_scene.curKickSerial |= 0x40000000u;
        uint64_t dLo = 0ull;
        std::memcpy(&dLo, gif.data() + gifOff - 16u, 8);
        dLo |= 0x40000000ull;
        std::memcpy(gif.data() + gifOff - 16u, &dLo, 8);
    }
}

inline ::RdVert makeRdVert(__m128 pos, __m128 stq, __m128i rgba, float fog = 1.0f) // FEAT-RD14 // FIX-E1b
{
    ::RdVert rv;
    _mm_storeu_ps(rv.pos, pos);
    const __m128i clamped = _mm_min_epu32(rgba, _mm_set1_epi32(255));
    const __m128i p16 = _mm_packus_epi32(clamped, clamped);
    const __m128i p8 = _mm_packus_epi16(p16, p16);
    const uint32_t packed = static_cast<uint32_t>(_mm_cvtsi128_si32(p8));
    std::memcpy(rv.rgba, &packed, 4);
    // FEAT-RD32: SIMD divide (bit-identical to the scalar s/q), q==0 -> uv 0
    const __m128 q = _mm_shuffle_ps(stq, stq, _MM_SHUFFLE(2, 2, 2, 2));
    const __m128 d = _mm_div_ps(stq, q);
    const __m128 nz = _mm_cmpneq_ps(q, _mm_setzero_ps());
    _mm_storel_pi(reinterpret_cast<__m64 *>(rv.uv), _mm_and_ps(d, nz));
    rv.fog = fog; // FIX-E1b
    return rv;
}

inline bool beginLeanDraw(const uint8_t *vu, SceneFrame &f, uint32_t vertCount, __m128 c1v, __m128 c2v) // FEAT-RD14
{
    static const bool s_camLog = []{ const char *e = std::getenv("PS2X_RD_CAMLOG"); return e && e[0] == '1'; }(); // PROBE-RD26
    if (s_camLog) // PROBE-RD26: FNV-1a of camera quads 998..1001
    {
        uint32_t h = 2166136261u; // PROBE-RD26
        for (uint32_t q = 998u; q <= 1001u; ++q) // PROBE-RD26
        {
            const uint8_t *cp = vu + ((q & 0x3FFu) << 4); // PROBE-RD26
            for (uint32_t i = 0; i < 16u; ++i) { h ^= cp[i]; h *= 16777619u; } // PROBE-RD26
        }
        f.curCamHash = h; // PROBE-RD26
    }
    RecScope rs(kRpBegin); // FEAT-RD32
    ++f.leanDraws;
    alignas(16) float vps[4];
    _mm_store_ps(vps, c1v);
    alignas(16) float vpo[4];
    _mm_store_ps(vpo, c2v);
    f.curVpS[0] = vps[0]; f.curVpS[1] = vps[1]; // FEAT-RD21
    f.curVpO[0] = vpo[0]; f.curVpO[1] = vpo[1]; // FEAT-RD21
    if (vertCount > f.bestVpVerts && vps[2] != 0.0f)
    {
        f.bestVpVerts = vertCount;
        f.vpScaleZ = vps[2];
        f.vpOffsetZ = vpo[2];
    }
    const uint32_t ctxt = static_cast<uint32_t>((t_scene.gs.prim >> 9) & 1ull);
    if (static_cast<uint32_t>(t_scene.gs.frame[ctxt] >> 32) == 0xFFFFFFFFu)
    {
        ++f.maskedDraws;
        return false;
    }
    return true;
}

template <class V> inline void reserveGrow(V &v, size_t need) // FEAT-RD32: geometric growth (plain reserve(need) reallocates to the exact size every draw)
{
    if (need > v.capacity())
        v.reserve(std::max(need, v.capacity() * 2u));
}

inline void pushLeanBatch(SceneFrame &f, uint32_t firstIndex, uint32_t indexCount) // FEAT-RD14
{
    RecScope rs(kRpPush); // FEAT-RD32
    if (indexCount == 0u)
        return;
    const uint32_t ctxt = static_cast<uint32_t>((t_scene.gs.prim >> 9) & 1ull);
    const uint64_t prim = t_scene.gs.prim;
    const uint64_t tex0 = t_scene.gs.tex0[ctxt];
    const uint64_t clamp = t_scene.gs.clamp[ctxt];
    const uint64_t tex1 = t_scene.gs.tex1[ctxt];
    const uint64_t alpha = t_scene.gs.alpha[ctxt];
    const uint64_t test = t_scene.gs.test[ctxt];
    const uint64_t frame = t_scene.gs.frame[ctxt];
    const uint64_t zbuf = t_scene.gs.zbuf[ctxt];
    const uint64_t texa = t_scene.gs.texa;
    const uint32_t kickSerial = t_scene.curKickSerial;
    const uint32_t fogcol = t_scene.gs.fogcol; // FIX-E1c
    if (!f.rdBatches.empty())
    {
        ::RdBatch &last = f.rdBatches.back();
        if (rdSerialsCanMerge(last.kickSerial, kickSerial) &&
            last.alpha == alpha && last.test == test && last.frame == frame && last.zbuf == zbuf &&
            last.prim == prim && last.tex0 == tex0 && last.clamp == clamp && last.tex1 == tex1 && last.texa == texa &&
            last.fogcol == fogcol && // FIX-E1c
            last.vpS[0] == f.curVpS[0] && last.vpS[1] == f.curVpS[1] && last.vpO[0] == f.curVpO[0] && last.vpO[1] == f.curVpO[1] && last.camHash == f.curCamHash) // FEAT-RD17 // FEAT-RD21 // PROBE-RD26
        {
            if (last.kickSerial != kickSerial) // FEAT-RD17
                ::g_rdMergedSpan.fetch_add(1u, std::memory_order_relaxed);
            last.count += indexCount;
            return;
        }
    }
    f.rdBatches.push_back(::RdBatch{firstIndex, indexCount, 0u, alpha, test, frame, zbuf, prim, tex0, clamp, tex1, texa, kickSerial,
                                    {f.curVpS[0], f.curVpS[1]}, {f.curVpO[0], f.curVpO[1]}, f.curCamHash, fogcol}); // FEAT-RD21 // PROBE-RD26 // FIX-E1c
}

inline void recordClipPolygon(const uint8_t *vu, int32_t polyCount, bool copyS, __m128 c1v, __m128 c2v, uint32_t gifOff, uint32_t gifLen, std::vector<uint8_t> &gif)
{
    { RecScope rm(kRpMarker); markGeometryKick(gif, gifOff); } // FEAT-RD10 // FEAT-RD32
    SceneFrame &f = t_scene.bufs[t_scene.writeIdx];
    if (leanRdOn()) // FEAT-RD14
    {
        RecScope rs(kRpClip); // FEAT-RD32 (inclusive of begin/push)
        if (!beginLeanDraw(vu, f, static_cast<uint32_t>(polyCount), c1v, c2v))
            return;
        const size_t v0 = f.rdVerts.size();
        const size_t i0 = f.rdIndices.size();
        f.rdVerts.resize(v0 + static_cast<size_t>(polyCount));
        for (int32_t k = 0; k < polyCount; ++k)
        {
            const uint32_t qOut = 877u + 4u * static_cast<uint32_t>(k);
            const uint32_t w3 = ldw(vu, (qOut + 3u) & 0x3FFu, 3u); // FIX-E1b
            const float fog = static_cast<float>((w3 >> 4) & 0xFFu) / 255.0f; // FIX-E1b
            f.rdVerts[v0 + static_cast<size_t>(k)] = makeRdVert(
                ldq(vu, 918u + 3u * static_cast<uint32_t>(k)),
                ldq(vu, qOut + 1u),
                ldqi(vu, qOut + 2u),
                fog); // FIX-E1b
        }
        if (polyCount >= 3)
        {
            reserveGrow(f.rdIndices, i0 + static_cast<size_t>(polyCount - 2) * 3u);
            const uint32_t base = static_cast<uint32_t>(v0);
            for (int32_t k = 2; k < polyCount; ++k)
            {
                f.rdIndices.push_back(base);
                f.rdIndices.push_back(base + static_cast<uint32_t>(k - 1));
                f.rdIndices.push_back(base + static_cast<uint32_t>(k));
            }
        }
        pushLeanBatch(f, static_cast<uint32_t>(i0), static_cast<uint32_t>(f.rdIndices.size() - i0));
        return;
    }
    const SceneDraw &d = pushDraw(vu, f, 0x2900u, gifOff, gifLen, static_cast<uint32_t>(polyCount), 0u, 4u | (copyS ? 2u : 0u), c1v, c2v); // FEAT-V2
    const size_t v0 = f.vertices.size();
    f.vertices.resize(v0 + static_cast<size_t>(polyCount));
    for (int32_t k = 0; k < polyCount; ++k)
    {
        SceneVertex &sv = f.vertices[v0 + static_cast<size_t>(k)];
        const uint32_t qOut = 877u + 4u * static_cast<uint32_t>(k);
        _mm_storeu_ps(sv.pos, ldq(vu, 918u + 3u * static_cast<uint32_t>(k)));
        sv.normal[0] = sv.normal[1] = sv.normal[2] = sv.normal[3] = 0.0f;
        _mm_storeu_ps(sv.stq, ldq(vu, qOut + 1u));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(sv.rgba), ldqi(vu, qOut + 2u));
        alignas(16) int32_t xyzw[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(xyzw), ldqi(vu, qOut + 3u));
        sv.screenXyz[0] = xyzw[0];
        sv.screenXyz[1] = xyzw[1];
        sv.screenXyz[2] = xyzw[2];
        sv.adc = ldw(vu, (qOut + 3u) & 0x3FFu, 3u); // FEAT-RD27
    }
    if (checkOn())
        checkDraw(d, f.vertices.data() + v0);
}

inline void recordRigid(const uint8_t *vu, int32_t N, bool copyS, __m128 c1v, __m128 c2v, const __m128 *Ps, const int32_t *Wd, uint32_t e7, uint32_t gifOff, uint32_t gifLen, std::vector<uint8_t> &gif)
{
    { RecScope rm(kRpMarker); markGeometryKick(gif, gifOff); } // FEAT-RD10 // FEAT-RD32
    SceneFrame &f = t_scene.bufs[t_scene.writeIdx];
    if (leanRdOn()) // FEAT-RD14
    {
        RecScope rs(kRpRigid); // FEAT-RD32 (inclusive of begin/push)
        if (!beginLeanDraw(vu, f, static_cast<uint32_t>(N), c1v, c2v))
            return;
        const size_t v0 = f.rdVerts.size();
        const size_t i0 = f.rdIndices.size();
        f.rdVerts.resize(v0 + static_cast<size_t>(N));
        if (N >= 3)
            reserveGrow(f.rdIndices, i0 + static_cast<size_t>(N - 2) * 3u);
        const uint32_t base = static_cast<uint32_t>(v0);
        for (int32_t j = 0; j < N; ++j)
        {
            const uint32_t qOut = (e7 + 1u + 4u * static_cast<uint32_t>(j)) & 0x3FFu;
            const uint32_t w3 = ldw(vu, (qOut + 3u) & 0x3FFu, 3u); // FIX-E1b
            const float fog = static_cast<float>((w3 >> 4) & 0xFFu) / 255.0f; // FIX-E1b
            f.rdVerts[v0 + static_cast<size_t>(j)] = makeRdVert(
                Ps[j],
                ldq(vu, (qOut + 1u) & 0x3FFu),
                ldqi(vu, (qOut + 2u) & 0x3FFu),
                fog); // FIX-E1b
            if (j >= 2)
            {
                const uint32_t adc = w3; // FIX-E1b
                if (!(adc & 0x8000u))
                {
                    f.rdIndices.push_back(base + static_cast<uint32_t>(j - 2));
                    f.rdIndices.push_back(base + static_cast<uint32_t>(j - 1));
                    f.rdIndices.push_back(base + static_cast<uint32_t>(j));
                }
            }
        }
        pushLeanBatch(f, static_cast<uint32_t>(i0), static_cast<uint32_t>(f.rdIndices.size() - i0));
        return;
    }
    const SceneDraw &d = pushDraw(vu, f, 0x2440u, gifOff, gifLen, static_cast<uint32_t>(N), 0u, copyS ? 2u : 0u, c1v, c2v); // FEAT-V2
    const size_t v0 = f.vertices.size();
    f.vertices.resize(v0 + static_cast<size_t>(N));
    for (int32_t j = 0; j < N; ++j)
    {
        SceneVertex &sv = f.vertices[v0 + static_cast<size_t>(j)];
        const uint32_t qOut = (e7 + 1u + 4u * static_cast<uint32_t>(j)) & 0x3FFu;
        const uint32_t ix = static_cast<uint32_t>(Wd[j] & 0x7FFF);
        _mm_storeu_ps(sv.pos, Ps[j]);
        _mm_storeu_ps(sv.normal, ldq(vu, (556u + ix) & 0x3FFu));
        _mm_storeu_ps(sv.stq, ldq(vu, (qOut + 1u) & 0x3FFu));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(sv.rgba), ldqi(vu, (qOut + 2u) & 0x3FFu));
        alignas(16) int32_t xyzw[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(xyzw), ldqi(vu, (qOut + 3u) & 0x3FFu));
        sv.screenXyz[0] = xyzw[0];
        sv.screenXyz[1] = xyzw[1];
        sv.screenXyz[2] = xyzw[2];
        sv.adc = static_cast<uint32_t>(xyzw[3]);
    }
    if (checkOn())
        checkDraw(d, f.vertices.data() + v0);
}

inline bool isMainPassSkinned(uint32_t vf27y) // FEAT-RD40
{
    if (!::ps2xRdHasDisplayFbp())
        return false;
    const uint64_t prim = ((vf27y >> 14) & 1u) ? ((vf27y >> 15) & 0x7FFull) : t_scene.gs.prim;
    const uint32_t ctxt = static_cast<uint32_t>((prim >> 9) & 1ull);
    const uint32_t fbp = static_cast<uint32_t>(t_scene.gs.frame[ctxt] & 0x1FFull);
    if (!::ps2xRdIsDisplayFbp(fbp))
        return false;
    const uint64_t scis = t_scene.gs.scissor[ctxt];
    const uint32_t x0 = static_cast<uint32_t>(scis & 0x7FFull);
    const uint32_t x1 = static_cast<uint32_t>((scis >> 16) & 0x7FFull);
    const uint32_t y0 = static_cast<uint32_t>((scis >> 32) & 0x7FFull);
    const uint32_t y1 = static_cast<uint32_t>((scis >> 48) & 0x7FFull);
    if (x1 < x0 || y1 < y0)
        return false;
    return (x1 + 1u - x0) >= 500u && (y1 + 1u - y0) >= 400u;
}

inline void recordSkinned(const uint8_t *vu, int32_t N, bool vi10, __m128 c1v, __m128 c2v, uint32_t i4base, uint32_t ubase, uint32_t gifOff, uint32_t gifLen, std::vector<uint8_t> &gif)
{
    { RecScope rm(kRpMarker); markGeometryKick(gif, gifOff); } // FEAT-RD10 // FEAT-RD32
    SceneFrame &f = t_scene.bufs[t_scene.writeIdx];
    if (leanRdOn()) // FEAT-RD14
    {
        RecScope rs(kRpSkinned); // FEAT-RD32 (inclusive of begin/push)
        if (!beginLeanDraw(vu, f, static_cast<uint32_t>(N), c1v, c2v))
            return;
        const size_t v0 = f.rdVerts.size();
        const size_t i0 = f.rdIndices.size();
        f.rdVerts.resize(v0 + static_cast<size_t>(N));
        if (N >= 3)
            reserveGrow(f.rdIndices, i0 + static_cast<size_t>(N - 2) * 3u);
        const uint32_t base = static_cast<uint32_t>(v0);
        for (int32_t v = 0; v < N; ++v)
        {
            const uint32_t qOut = ubase + 1u + 4u * static_cast<uint32_t>(v);
            const int32_t idx = static_cast<int16_t>(ilw(vu, (i4base + static_cast<uint32_t>(v)) & 0x3FFu, 0u));
            const uint32_t w3 = ldw(vu, (qOut + 3u) & 0x3FFu, 3u); // FIX-E1b
            const float fog = static_cast<float>((w3 >> 4) & 0xFFu) / 255.0f; // FIX-E1b
            f.rdVerts[v0 + static_cast<size_t>(v)] = makeRdVert(
                ldq(vu, (636u + static_cast<uint32_t>(idx)) & 0x3FFu),
                ldq(vu, qOut + 1u),
                ldqi(vu, qOut + 2u),
                fog); // FIX-E1b
            if (v >= 2)
            {
                const uint32_t adc = w3; // FEAT-RD27 // FIX-E1b
                if (!(adc & 0x8000u))
                {
                    f.rdIndices.push_back(base + static_cast<uint32_t>(v - 2));
                    f.rdIndices.push_back(base + static_cast<uint32_t>(v - 1));
                    f.rdIndices.push_back(base + static_cast<uint32_t>(v));
                }
            }
        }
        pushLeanBatch(f, static_cast<uint32_t>(i0), static_cast<uint32_t>(f.rdIndices.size() - i0));
        return;
    }
    const SceneDraw &d = pushDraw(vu, f, vi10 ? 0x1EF0u : 0x1EE0u, gifOff, gifLen, static_cast<uint32_t>(N), t_scene.curBoneCount, 1u | (vi10 ? 2u : 0u), c1v, c2v); // FEAT-V2
    const size_t v0 = f.vertices.size();
    f.vertices.resize(v0 + static_cast<size_t>(N));
    for (int32_t v = 0; v < N; ++v)
    {
        SceneVertex &sv = f.vertices[v0 + static_cast<size_t>(v)];
        const uint32_t qOut = ubase + 1u + 4u * static_cast<uint32_t>(v);
        const int32_t idx = static_cast<int16_t>(ilw(vu, (i4base + static_cast<uint32_t>(v)) & 0x3FFu, 0u));
        _mm_storeu_ps(sv.pos, ldq(vu, (636u + static_cast<uint32_t>(idx)) & 0x3FFu));
        _mm_storeu_ps(sv.normal, ldq(vu, (556u + static_cast<uint32_t>(idx)) & 0x3FFu));
        _mm_storeu_ps(sv.stq, ldq(vu, qOut + 1u));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(sv.rgba), ldqi(vu, qOut + 2u));
        alignas(16) int32_t xyzw[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(xyzw), ldqi(vu, qOut + 3u));
        sv.screenXyz[0] = xyzw[0];
        sv.screenXyz[1] = xyzw[1];
        sv.screenXyz[2] = xyzw[2];
        sv.adc = ldw(vu, (qOut + 3u) & 0x3FFu, 3u); // FEAT-RD27
    }
    if (checkOn())
        checkDraw(d, f.vertices.data() + v0);
}
} // namespace scene

static inline __m128 vuSat4(__m128 v) // FIX-M10
{
    const __m128 maxV = _mm_set1_ps(FLT_MAX);
    const __m128 minV = _mm_set1_ps(-FLT_MAX);
    const __m128 signBit = _mm_and_ps(v, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
    const __m128 isNan = _mm_cmpunord_ps(v, v);
    const __m128 clamped = _mm_min_ps(_mm_max_ps(v, minV), maxV);
    return _mm_blendv_ps(clamped, signBit, isNan);
}

static inline float vuSat(float v) // FIX-M10
{
    return _mm_cvtss_f32(vuSat4(_mm_set_ss(v)));
}

// Task F4: Sutherland-Hodgman 6-plane frustum clipper (2900..2F68), called from rigid2440 when no trivial-reject FCOR mask matches.
// Reproduces every context/spill/buffer/GIF store to VU memory (1020..1022, 918..959, 876..) and kicks 1023 + 876; returns to 24F8/2720 via JR vi12.
static bool clipPolygon(uint8_t *vu, uint16_t vi13, int32_t vi5, int32_t vi6, int32_t vi7, int32_t vi8, int32_t vi9, int32_t vi10, bool copyS,
                        __m128 c1v, __m128 c2v, __m128 c5v, __m128 vf28, __m128 vf29, __m128 vf10, __m128 vf30, __m128 vf31, __m128 vf7,
                        __m128 vf11, __m128 vf12, __m128 vf13, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const uint16_t retVi12 = static_cast<uint16_t>((copyS ? 0x2720u : 0x24F8u) >> 3);

    // 2900..2960: save integer context to 1020..1022 (PERF-V1G: bulk logRange + stqiRaw)
    logRange(vu, 1020u, 3u);
    stqiRaw(vu, 1022u, _mm_set_epi32(vi8 & 0xFFFF, vi7 & 0xFFFF, vi6 & 0xFFFF, vi5 & 0xFFFF));
    stqiRaw(vu, 1021u, _mm_set_epi32(retVi12, 0x7FFF, vi10 & 0xFFFF, vi9 & 0xFFFF));
    stqiRaw(vu, 1020u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1020u), vi13, 0), 5, 1));
    const uint32_t clipMode = ldw(vu, 1012u, 1u) & 1u;

    // 2968..29E8: spill triangle vertices + wrap-around vertex 0 into 918..929
    vf13 = maxV(vf13, zero); // 2978 MAXx.xyzw vf13, vf13, vf0
    logRange(vu, 918u, 60u); // PERF-V1G: covers 918..929 spill + 918..977 ping-pong buffers (<= 9 verts + wrap = 30 qwords per side)
    logRange(vu, 876u, 1u);  // PERF-V1G: GIF tag qword 876
    stqRaw(vu, 918u, vf28);
    stqRaw(vu, 921u, vf29);
    stqRaw(vu, 924u, vf10);
    stqRaw(vu, 927u, vf28);
    stqRaw(vu, 920u, vf30);
    stqRaw(vu, 923u, vf31);
    stqRaw(vu, 926u, vf7);
    stqRaw(vu, 925u, vf13);
    stqRaw(vu, 929u, vf30); // 29B0 delay slot of IBEQ
    if (clipMode != 0u)
    {
        stqRaw(vu, 919u, vf13);
        stqRaw(vu, 922u, vf13);
        stqRaw(vu, 928u, vf13);
    }
    else
    {
        stqRaw(vu, 919u, vf11);
        stqRaw(vu, 922u, vf12);
        stqRaw(vu, 928u, vf11);
    }

    // 29F0..2C70: 6 frustum planes (-Z, +Z, -X, +X, -Y, +Y), ping-ponging 918 <-> 948
    struct PlaneSpec
    {
        float sgn;
        uint32_t mask0, mask1;
        int axis;
        uint32_t inBase, outBase;
    };
    static constexpr PlaneSpec kPlanes[6] = {
        {-1.0f, 2048u >> 6, 32u, 2, 918u, 948u},
        {1.0f, 1024u >> 6, 16u, 2, 948u, 918u},
        {-1.0f, 128u >> 6, 2u, 0, 918u, 948u},
        {1.0f, 64u >> 6, 1u, 0, 948u, 918u},
        {-1.0f, 512u >> 6, 8u, 1, 918u, 948u},
        {1.0f, 256u >> 6, 4u, 1, 948u, 918u},
    };

    int32_t polyCount = 3;
    __m128 vf19_last = zero;
    const bool m9On = watch::spikeOn(); // PROBE-M9
    __m128 m9InPos[3]{}, m9InCol[3]{}, m9InTex[3]{}; // PROBE-M9
    uint32_t m9InFlags[3]{}; // PROBE-M9
    float m9InFog[3]{}; // PROBE-M9
    char m9SrcA[16][64]{}, m9SrcB[16][64]{}; // PROBE-M9
    watch::M9PlaneTrace m9Planes[6]{}; // PROBE-M9
    int m9NPlanes = 0; // PROBE-M9
    int32_t m9UnsetSlot = -1; // PROBE-M9
    if (m9On) // PROBE-M9
    {
        for (int i = 0; i < 3; ++i)
        {
            m9InPos[i] = ldq(vu, 918u + static_cast<uint32_t>(i) * 3u);
            m9InCol[i] = ldq(vu, 919u + static_cast<uint32_t>(i) * 3u);
            m9InTex[i] = ldq(vu, 920u + static_cast<uint32_t>(i) * 3u);
            m9InFlags[i] = clipBitsV(m9InPos[i]);
            const __m128 tv = fmacV<2>(c2v, bc<3>(m9InPos[i]), fmacV<4>(c1v, m9InPos[i], zero));
            const float iq = divQ2(1.0f, _mm_cvtss_f32(bc<3>(tv)));
            const __m128 fw = miniV(maxV(fmacV<2>(bc<3>(c5v), _mm_set1_ps(iq), fmacV<4>(vf0, bc<2>(c5v), zero)), zero), _mm_set1_ps(255.0f));
            m9InFog[i] = _mm_cvtss_f32(bc<3>(fw));
            std::snprintf(m9SrcA[i], sizeof(m9SrcA[i]), "copy of in[%d]", i);
        }
    }
    for (int p = 0; p < 6; ++p)
    {
        const PlaneSpec &pl = kPlanes[p];
        const __m128 sgnV = _mm_set1_ps(pl.sgn);
        __m128 vf17 = ldq(vu, pl.inBase + 0u);
        __m128 vf18 = ldq(vu, pl.inBase + 1u);
        __m128 vf23 = ldq(vu, pl.inBase + 2u);
        __m128 vf21 = ldq(vu, pl.inBase + 3u);
        uint32_t inPtr = pl.inBase + 4u;
        __m128 vf22 = ldq(vu, inPtr++);
        uint32_t outPtr = pl.outBase;
        int32_t outCount = 0;
        bool m9OutWritten[16]{}; // PROBE-M9
        int m9TPos = 0; // PROBE-M9
        if (m9On) // PROBE-M9
        {
            m9Planes[p].planeIdx = p;
            m9Planes[p].vin = polyCount;
            m9Planes[p].sides[0] = '\0';
            m9Planes[p].tList[0] = '\0';
            for (int32_t i = 0; i < 16; ++i)
                std::snprintf(m9SrcB[i], sizeof(m9SrcB[i]), "UNSET");
        }

        for (int32_t e = 0; e < polyCount; ++e)
        {
            const __m128 vf19 = fmacV<0>(vf23, zero, zero);
            const bool out0 = (clipBitsV(vf17) & pl.mask0) != 0u;
            const bool out1 = (clipBitsV(vf21) & pl.mask1) != 0u;
            const __m128 vf25w = fmacV<4>(vf17, sgnV, zero);
            const __m128 vf26w = fmacV<4>(vf21, sgnV, zero);
            vf23 = ldq(vu, inPtr++);
            if (m9On && e < 18) // PROBE-M9
            {
                m9Planes[p].sides[e] = out0 ? '1' : '0';
                m9Planes[p].sides[e + 1] = '\0';
            }

            if (!out0)
            {
                if (m9On && outCount < 16) // PROBE-M9
                {
                    m9OutWritten[outCount] = true;
                    std::snprintf(m9SrcB[outCount], sizeof(m9SrcB[outCount]), "%s", m9SrcA[e < 16 ? e : 15]);
                }
                stqRaw(vu, outPtr++, vf17);
                stqRaw(vu, outPtr++, vf18);
                stqRaw(vu, outPtr++, vf19);
                ++outCount;
            }
            if (out0 != out1)
            {
                // 2EC8..2F68: edge intersection interpolator
                __m128 d0v = fmacV<1>(vf17, bc<3>(vf25w), zero);
                __m128 d1v = fmacV<1>(vf21, bc<3>(vf26w), zero);
                for (int r = 0; r < pl.axis; ++r)
                {
                    d0v = mr32(d0v);
                    d1v = mr32(d1v);
                }
                const float d0 = _mm_cvtss_f32(d0v); // FIX-M10
                const float d1 = _mm_cvtss_f32(d1v); // FIX-M10
                const float den = vuSat(d0 - d1); // FIX-M10
                float t; // FIX-M10
                if (den == 0.0f) // FIX-M10
                    t = (d0 == 0.0f) ? 0.0f : ((std::signbit(d0) != std::signbit(den)) ? -FLT_MAX : FLT_MAX); // FIX-M10
                else // FIX-M10
                    t = vuSat(d0 / den); // FIX-M10
                const __m128 tv = _mm_set1_ps(t); // FIX-M10

                const __m128 posDiff = vuSat4(_mm_sub_ps(vf21, vf17)); // FIX-M10
                const __m128 pos = vuSat4(_mm_add_ps(vf17, vuSat4(_mm_mul_ps(tv, posDiff)))); // FIX-M10
                const __m128 colDiff = vuSat4(_mm_sub_ps(vf22, vf18)); // FIX-M10
                const __m128 colLerp = vuSat4(_mm_add_ps(vf18, vuSat4(_mm_mul_ps(tv, colDiff)))); // FIX-M10
                const __m128 col = vuSat4(_mm_add_ps(colLerp, _mm_set1_ps(0.001f))); // FIX-M10
                const __m128 stDiff = vuSat4(_mm_sub_ps(vf23, vf19)); // FIX-M10
                const __m128 st = vuSat4(_mm_add_ps(vf19, vuSat4(_mm_mul_ps(tv, stDiff)))); // FIX-M10
                const __m128 m1012 = ldq(vu, 1012u); // 2F18 + 2F40: SQ.yzw m1012, 876(vi0)
                stqRaw(vu, 876u, _mm_blend_ps(ldq(vu, 876u), m1012, 0xE));

                if (m9On) // PROBE-M9
                {
                    const float tVal = t; // FIX-M10
                    m9TPos += std::snprintf(m9Planes[p].tList + m9TPos,
                                            sizeof(m9Planes[p].tList) - static_cast<size_t>(m9TPos),
                                            "%s%.5g", m9TPos ? "," : "", tVal);
                    if (outCount < 16)
                    {
                        m9OutWritten[outCount] = true;
                        std::snprintf(m9SrcB[outCount], sizeof(m9SrcB[outCount]),
                                      "p%d:e%d(%d->%d,t=%.5g)", p, e, e, (e + 1) % polyCount, tVal);
                    }
                }
                stqRaw(vu, outPtr++, pos);
                stqRaw(vu, outPtr++, col);
                stqRaw(vu, outPtr++, st);
                ++outCount;
            }
            vf17 = fmacV<0>(vf21, zero, zero);
            vf18 = fmacV<0>(vf22, zero, zero);
            vf21 = ldq(vu, inPtr++);
            if (e + 1 < polyCount)
                vf22 = ldq(vu, inPtr++);
        }

        if (m9On) // PROBE-M9
        {
            m9Planes[p].vout = outCount;
            m9NPlanes = p + 1;
            for (int32_t j = 0; j < outCount && j < 16; ++j)
            {
                if (!m9OutWritten[j] && m9UnsetSlot < 0)
                    m9UnsetSlot = j;
                std::snprintf(m9SrcA[j], sizeof(m9SrcA[j]), "%s", m9SrcB[j]);
            }
        }
        if (outCount == 0)
        {
            if (m9On) // PROBE-M9
                watch::m9RecordAndMaybeSummary(clipMode, copyS, false);
            return true; // 2E88 -> 2D50..2DA8: completely clipped out
        }

        // 2E90..2EC0: wrap-around copy of vertex 0 + screen-space transform of v0 on last plane
        const __m128 w0 = ldq(vu, pl.outBase + 0u);
        const __m128 w1 = ldq(vu, pl.outBase + 1u);
        const __m128 w2 = ldq(vu, pl.outBase + 2u);
        stqRaw(vu, outPtr++, w0);
        stqRaw(vu, outPtr++, w1);
        stqRaw(vu, outPtr++, w2);
        if (p == 5)
            vf19_last = fmacV<2>(c2v, bc<3>(w0), fmacV<4>(c1v, w0, zero));
        polyCount = outCount;
    }

    // 2C78..2D48: emit clipped polygon GIF packet at 876..
    const uint32_t tagX = (static_cast<uint32_t>(polyCount) + 32768u) & 0xFFFFu;
    stqiRaw(vu, 876u, _mm_insert_epi32(ldqi(vu, 876u), static_cast<int32_t>(tagX), 0));
    if (!kick(vu, 1023u, kicks, gif))
        return fb1EE0(kFbClipKick);

    float q = divQ2(1.0f, _mm_cvtss_f32(bc<3>(vf19_last)));
    uint32_t ptr = 918u;
    uint32_t outQ = 877u;
    __m128 vf25 = ldq(vu, ptr + 3u);
    logRange(vu, 877u, static_cast<uint32_t>(polyCount) * 4u); // PERF-V1G
    for (int32_t k = 0; k < polyCount; ++k)
    {
        const __m128 qb = _mm_set1_ps(q);
        const __m128 f2 = fmacV<4>(vf0, bc<2>(c5v), zero);
        __m128 vf8 = fmacV<2>(bc<3>(c5v), qb, f2);
        const __m128 vf26 = ldq(vu, ptr + 1u);
        const __m128 vf21 = fmacV<4>(vf19_last, qb, zero);
        __m128 vf27 = ldq(vu, ptr + 2u);
        vf19_last = fmacV<2>(c2v, bc<3>(vf25), fmacV<4>(c1v, vf25, zero));
        vf8 = maxV(vf8, zero);
        vf27 = _mm_blend_ps(vf27, fmacV<4>(vf27, qb, zero), 0x7);
        vf8 = miniV(vf8, _mm_set1_ps(255.0f));
        const __m128i rg = ftoiS<1>(vf26);
        const __m128i xy = ftoiS<16>(vf21);
        q = divQ2(1.0f, _mm_cvtss_f32(bc<3>(vf19_last)));
        ptr += 3u;
        vf25 = ldq(vu, ptr + 3u);
        const __m128i fw = ftoiS<16>(vf8);
        stqRaw(vu, outQ + 1u, vf27);
        stqiRaw(vu, outQ + 2u, rg);
        stqiRaw(vu, outQ + 3u, xy);
        stqiRaw(vu, outQ + 0u, _mm_insert_epi32(ldqi(vu, outQ + 0u), _mm_extract_epi32(fw, 3), 3));
        outQ += 4u;
    }
    if (!kick(vu, 876u, kicks, gif))
        return false;
    if (watch::spikeOn()) // PROBE-M8
    {
        float minW = 0.0f, maxW = 0.0f, maxAbsXY = 0.0f;
        bool first = true;
        bool m9Bad = (m9UnsetSlot >= 0); // PROBE-M9
        __m128 m9OutPos[16]{}, m9OutCol[16]{}, m9OutTex[16]{}; // PROBE-M9
        float m9OutFog[16]{}; // PROBE-M9
        for (int32_t k = 0; k < polyCount; ++k)
        {
            const __m128 vp = ldq(vu, 918u + 3u * static_cast<uint32_t>(k));
            watch::m8AccumVert(vp, first, minW, maxW, maxAbsXY);
            if (k < 16) // PROBE-M9
            {
                m9OutPos[k] = vp;
                m9OutCol[k] = ldq(vu, 919u + 3u * static_cast<uint32_t>(k));
                m9OutTex[k] = ldq(vu, 920u + 3u * static_cast<uint32_t>(k));
                const __m128 tv = fmacV<2>(c2v, bc<3>(vp), fmacV<4>(c1v, vp, zero));
                const float oq = divQ2(1.0f, _mm_cvtss_f32(bc<3>(tv)));
                const __m128 fw = miniV(maxV(fmacV<2>(bc<3>(c5v), _mm_set1_ps(oq), fmacV<4>(vf0, bc<2>(c5v), zero)), zero), _mm_set1_ps(255.0f));
                m9OutFog[k] = _mm_cvtss_f32(bc<3>(fw));
                alignas(16) float p[4];
                _mm_store_ps(p, vp);
                if (p[3] == 0.0f || (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 0.0f))
                    m9Bad = true;
            }
        }
        const uint32_t serial30 = scene::t_scene.curKickSerial & 0x3FFFFFFFu;
        watch::m8EnrichDraw(serial30, 0x2900u, 1u, 3u,
                            static_cast<uint32_t>(polyCount), minW, maxW, maxAbsXY);
        watch::m9RecordAndMaybeSummary(clipMode, copyS, m9Bad); // PROBE-M9
        if (m9Bad) // PROBE-M9
            watch::m9DumpBad(serial30, watch::t_m8Mscal, clipMode, copyS, vi7, polyCount,
                             m9InPos, m9InCol, m9InTex, m9InFlags, m9InFog,
                             m9OutPos, m9OutCol, m9OutTex, m9OutFog,
                             m9SrcA, m9UnsetSlot, m9Planes, m9NPlanes);
    }
    if (scene::on()) // PERF-SA
        scene::recordClipPolygon(vu, polyCount, copyS, c1v, c2v, kicks.back().first, kicks.back().second, gif);
    return true;
}

// Rigid path 2440 (per-vertex transform / trivial-reject / assemble, loop 24C8..25B0): 1:1 value + memory port of lbVert (vu1_native_hand.inc), which is exact on every
// record without real clipping. Differences: runs in place on VU memory through stq (same program order of loads/stores as lbVert's private copy, so the data
// seen by later loads is identical); flags / STATUS / cycles / exit registers dropped; the epilogue 25B8..2200 (1023 swap, XGKICK vi7, XTOP) added.
// Real clipping (no mask of the 6 trivial-reject tests matches, routine 2900+) is handled by clipPolygon(...).
static bool rigid2440(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t top, int32_t N, uint32_t i4base, uint32_t s2base, uint32_t s3base, uint32_t ubase, uint32_t vf27x, uint32_t vf27y,
                      bool copyS, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    constexpr int kMaxN = 300;
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), v2048 = _mm_set1_ps(2048.0f);
    const __m128 c1v = ldq(vu, 1002), c2v = ldq(vu, 1003), c5v = ldq(vu, 1016), c14v = ldq(vu, 1014), vf16 = ldq(vu, 1013);
    const uint32_t m1012z = ldw(vu, 1012, 2), m1012w = ldw(vu, 1012, 3);
    const auto V7 = [&](__m128 s) { return _mm_blend_ps(s, v2048, 8); };
    static thread_local __m128 Ps[kMaxN + 2], As[kMaxN + 2], Ss[kMaxN + 2], Cs[kMaxN + 2], Tv[kMaxN + 2], Lit[kMaxN + 2];
    static thread_local float Qf[kMaxN + 2];
    static thread_local uint32_t Fb[kMaxN + 2];
    static thread_local int32_t Wd[kMaxN + 2];
    const auto s16 = [](int32_t v) { return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(v))); };

    int32_t vi1 = 0, vi5 = N, vi6 = static_cast<int32_t>(ubase), vi8 = -2, vi9 = -2, vi10 = 0, vi4 = static_cast<int32_t>(i4base);
    int32_t vi3 = static_cast<int32_t>(s3base), pS = static_cast<int32_t>(s2base);
    const int32_t vi4_0 = vi4;
    Wd[0] = s16(static_cast<int32_t>(ldw(vu, static_cast<uint32_t>(vi4_0), 0) & 0xFFFFu)); // ILW.x vi7, 0(vi4)
    const int32_t ix0 = Wd[0] & 0x7FFF;
    Ps[0] = ldq(vu, static_cast<uint32_t>(636 + ix0));
    logRange(vu, static_cast<uint32_t>(vi6), static_cast<uint32_t>(4 * N + 1)); // PERF-V1G: bulk-log [ubase, ubase + 4N]
    stqiRaw(vu, static_cast<uint32_t>(vi6), _mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x))); // SQI vf27
    vi6 = s16(vi6 + 1);
    Cs[0] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi3)));
    int32_t pC = s16(vi3 + 1);
    As[0] = ldq(vu, static_cast<uint32_t>(716 + ix0));
    Ss[0] = ldq(vu, copyS ? static_cast<uint32_t>(796 + ix0) : static_cast<uint32_t>(static_cast<uint16_t>(pS)));
    if (!copyS)
        pS = s16(pS + 1);
    Fb[0] = clipBitsV(Ps[0]);
    vi1 = (Fb[0] & 0x3Fu) ? 1 : 0;
    {
        const __m128 p1 = fmacV<4>(c1v, Ps[0], zero);
        Tv[0] = fmacV<2>(c2v, bc<3>(Ps[0]), p1);
        const __m128 p3 = fmacV<4>(Cs[0], vf16, zero);
        const __m128 p5 = fmacV<4>(c14v, p3, zero);
        Lit[0] = fmacV<2>(As[0], p3, p5);
        Qf[0] = divQ(Tv[0]);
    }
    for (int32_t it = 1; it <= N; ++it)
    {
        const int32_t j = it - 1;
        vi10 = vi1;
        char pth;
        int hit = -1;
        if (Wd[j] < 0)
            pth = 'A';
        else if (vi8 <= 0)
            pth = 'B';
        else
        {
            pth = 'C';
            const uint32_t cl0 = j >= 2 ? Fb[j - 2] : 0u, cl1 = j >= 1 ? Fb[j - 1] : 0u, cl2 = Fb[j];
            const uint32_t vis = (cl0 << 12) | (cl1 << 6) | cl2;
            static constexpr uint32_t kMasks[6] = {0xFDF7DFu, 0xFEFBEFu, 0xFF7DF7u, 0xFFBEFBu, 0xFFDF7Du, 0xFFEFBEu};
            for (int m = 0; m < 6; ++m)
                if ((vis | kMasks[m]) == 0xFFFFFFu)
                {
                    hit = m;
                    break;
                }
            if (hit < 0)
            {
                // FIX-BJ: 25B0/27D8 MADD.xyzw vf13 (Lit[j]) -> 24E0/2708 LOI 255.0 -> 24E8/2710 MINIi.xyzw vf13, vf13, I before BAL 28A0->2900; 2978 MAXx.xyzw vf13, vf13, vf0 completes clampI(Lit[j]).
                const uint64_t vxClip0 = vx::act() ? __rdtsc() : 0ull; // PROBE-VX
                const uint64_t hpClip0 = hprof::on() ? __rdtsc() : 0ull; // PROBE-RD39
                const bool vxClipOk = clipPolygon(vu, vi13, vi5, vi6, Wd[j], vi8, vi9, vi10, copyS, c1v, c2v, c5v,
                                 Ps[j - 2], Ps[j - 1], Ps[j], fmacV<0>(V7(Ss[j - 2]), zero, zero), V7(Ss[j - 1]), V7(Ss[j]),
                                 clampI(Lit[j - 2]), clampI(Lit[j - 1]), clampI(Lit[j]), kicks, gif); // PROBE-VX // FIX-BJ
                if (hpClip0) { const uint64_t dt = __rdtsc() - hpClip0; hprof::addHandler(0x2900u, dt); hprof::subtractHandler(0x2440u, dt); } // PROBE-RD39
                if (vxClip0) { vx::g_vx.hT[vx::kHClip] += __rdtsc() - vxClip0; ++vx::g_vx.hN[vx::kHClip]; } // PROBE-VX
                if (!vxClipOk) // PROBE-VX
                    return fb1EE0(kFbRigidClip);
            }
            else if (hit == 5) // ISW.x vi5, 1022 (delay slot of the IBNE at 28F8)
                stqi(vu, 1022u, _mm_insert_epi32(ldqi(vu, 1022u), vi5, 0));
        }
        const __m128 qb = _mm_set1_ps(Qf[j]);
        const __m128 f2 = fmacV<4>(vf0, bc<2>(c5v), zero);
        const __m128 f3 = fmacV<2>(bc<3>(c5v), qb, f2);
        const __m128 f5 = fmacV<4>(Tv[j], qb, zero);
        const __m128 f6 = fmacV<4>(V7(Ss[j]), qb, zero);
        Wd[it] = s16(static_cast<int32_t>(ldw(vu, static_cast<uint32_t>(vi4_0 + it), 0) & 0xFFFFu));
        const int32_t ix = Wd[it] & 0x7FFF;
        Ps[it] = ldq(vu, static_cast<uint32_t>(636 + ix));
        vi6 = s16(vi6 + 4);
        {
            const uint32_t qd = static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 3));
            stqRaw(vu, qd, _mm_blend_ps(ldq(vu, qd), f6, 0x7)); // lanes xyz only // PERF-V1G
        }
        As[it] = ldq(vu, static_cast<uint32_t>(716 + ix));
        Ss[it] = ldq(vu, copyS ? static_cast<uint32_t>(796 + ix) : static_cast<uint32_t>(static_cast<uint16_t>(pS)));
        if (!copyS)
            pS = s16(pS + 1);
        Cs[it] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(pC)));
        pC = s16(pC + 1);
        const __m128 f7 = fmacV<4>(c1v, Ps[it], zero);
        const __m128 tw = clampI(f3);
        const float vf15w = pth == 'B' ? 0.0f : 2048.0f;
        const __m128 f8 = _mm_add_ps(tw, _mm_set1_ps(vf15w)); // ADD.w (tw clamped to [0,255]: never out of the exponent window / never needs the slow path)
        Tv[it] = fmacV<2>(c2v, bc<3>(Ps[it]), f7);
        const __m128 f11 = fmacV<4>(Cs[it], vf16, zero);
        const __m128 f12 = fmacV<4>(c14v, f11, zero);
        Lit[it] = fmacV<2>(As[it], f11, f12);
        {
            const __m128i rg = ftoiS<1>(clampI(Lit[j]));
            __m128i xy = ftoiS<16>(f5);
            const __m128i ww = ftoiS<16>(f8);
            xy = _mm_insert_epi32(xy, _mm_extract_epi32(ww, 3), 3);
            stqiRaw(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 2)), rg); // PERF-V1G
            stqiRaw(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 4)), xy);
            stqiRaw(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 1)), xy);
        }
        Fb[it] = clipBitsV(Ps[it]);
        Qf[it] = divQ(Tv[it]);
        vi1 = (Fb[it] & 0x3Fu) ? 1 : 0;
        vi8 = s16(vi9 + vi10 + vi1);
        vi9 = vi10;
        vi5 = s16(vi5 - 1);
        vi4 = s16(vi4 + 1);
    }
    // epilogue 25B8..2200: ILW.w vi6 / ILW.z vi7 from 1023, XTOP vi12, ISW.z vi6 / ISW.w vi7 (swap), XGKICK vi7
    const uint32_t e6 = ilw(vu, 1023, 3), e7 = ilw(vu, 1023, 2);
    stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(e6), 2), static_cast<int>(e7), 3));
    if (!kick(vu, e7, kicks, gif))
        return fb1EE0(kFbRigidKick);
    if (watch::spikeOn()) // PROBE-M8
    {
        float minW = 0.0f, maxW = 0.0f, maxAbsXY = 0.0f;
        bool first = true;
        for (int32_t j = 0; j < N; ++j)
            watch::m8AccumVert(Ps[j], first, minW, maxW, maxAbsXY);
        watch::m8EnrichDraw(scene::t_scene.curKickSerial & 0x3FFFFFFFu, 0x2440u, 0u,
                            static_cast<uint32_t>(N), static_cast<uint32_t>(N), minW, maxW, maxAbsXY);
    }
    if (watch::on()) // PROBE-M5
        watch::checkDraw(vu, 0x2440u, top, kicks.back().first, static_cast<uint32_t>(N), false, 0u, 0u, nullptr, Ps, Wd);
    if (scene::on()) // PERF-SA
        scene::recordRigid(vu, N, copyS, c1v, c2v, Ps, Wd, e7, kicks.back().first, kicks.back().second, gif);
    R.vi[12] = top; // XTOP vi12
    return true;
}

// 1EE0 entry (vi10 = 0). Prologue 1EF8..1F78 in closed form, then the skinned path 1FE0 -> JR (1:1 value port of lbFull / lbVertex: the exact scalar vertex
// chain, no AVX2/relaxed groups), memory + GIF only. R.vi[12] is live state (the routine leaves vi12 = idx of vertex 1). Returns false (nothing needs undoing:
// the caller restores the undo log) for every case lbFull declines or that it does not model: N outside 4..67, DIV by zero, any aliasing between the output
// buffer and anything the real (software-pipelined) routine reads after its first stores, kick failure.
// 1EE0 (vi10 = 0) and 1EF0 (vi10 = 1: reads S from 796+idx in both skinned 2210 and rigid 2670).
template <bool VI10>
static void buildLeanSkinVertsImpl(const uint8_t *vu, int32_t N, uint32_t s2base, uint32_t s3base, const int32_t *idxs,
                                   __m128 vf1, __m128 vf2, __m128 vf3, __m128 vf4, __m128 vf16, __m128 accw, // FIX-E10a
                                   const Fv8V &k1, const Fv8V &k2, const Fv8V &k3, const Fv8V &k4, const Fv8V &k16, const Fv8V &kaccw, // FIX-E10a
                                   ::RdVert *outVerts, uint32_t *outIndices, uint32_t baseIndex, uint32_t &outIdxCount) // FEAT-RD40 // PERF-PF1
{
    const __m128 zero = _mm_setzero_ps();
    const __m256 z8v = _mm256_setzero_ps();
    const __m128i v255 = _mm_set1_epi32(255);
    uint32_t ic = 0u;
    for (int32_t t0 = 0; t0 < N; t0 += 8)
    {
        const int32_t n = (N - t0 < 8) ? (N - t0) : 8;
        __m128 gA[8], gB[8], gS3[8], gS2[8];
        for (int j = 0; j < 8; ++j)
        {
            const int32_t s = t0 + (j < n ? j : n - 1);
            const int32_t idx = idxs[s];
            gA[j] = ldq(vu, static_cast<uint32_t>(636 + idx));
            gB[j] = ldq(vu, static_cast<uint32_t>(716 + idx));
            gS3[j] = ldq(vu, s3base + static_cast<uint32_t>(s));
            gS2[j] = VI10 ? ldq(vu, static_cast<uint32_t>(796 + idx)) : ldq(vu, s2base + static_cast<uint32_t>(s));
        }
        Fv8V A = fv8FromRows(gA), B = fv8FromRows(gB), S3 = fv8FromRows(gS3), S2 = fv8FromRows(gS2); // FIX-E10a
        for (int c = 0; c < 4; ++c)
        {
            A.c[c] = fv8N(A.c[c]); // FIX-E10a
            B.c[c] = fv8N(B.c[c]);
            S3.c[c] = fv8N(S3.c[c]);
        }
        // PERF-PF1: S2.c[3] is unused; compute UV = (S/Q, T/Q) in 8-wide AVX2 (bit-exact IEEE-754 div)
        S2.c[0] = fv8N(S2.c[0]);
        S2.c[1] = fv8N(S2.c[1]);
        S2.c[2] = fv8N(S2.c[2]);
        const __m256 nz8 = _mm256_cmp_ps(S2.c[2], z8v, _CMP_NEQ_OQ);
        const __m256 u8 = _mm256_and_ps(_mm256_div_ps(S2.c[0], S2.c[2]), nz8);
        const __m256 v8 = _mm256_and_ps(_mm256_div_ps(S2.c[1], S2.c[2]), nz8);
        alignas(32) float uArr[8], vArr[8];
        _mm256_store_ps(uArr, u8);
        _mm256_store_ps(vArr, v8);

        Fv8V z8;
        z8.c[0] = z8.c[1] = z8.c[2] = z8.c[3] = z8v;
        __m256i bad = _mm256_setzero_si256();
        const Fv8V mula = fv8Op<1, false>(k3, A, z8, bad); // FIX-E10a
        const Fv8V t5 = fv8Op<1, true>(k4, Fv8V{{A.c[3], A.c[3], A.c[3], A.c[3]}}, mula, bad); // FIX-E10a
        const __m256 den = fv8N(t5.c[3]); // FIX-E10a
        bool fOk = !fv8Any(fv8IsZ(den)); // FIX-E10a
        alignas(32) int32_t wArr[8]; // FIX-E10a
        if (fOk) // FIX-E10a
        {
            const __m256 qv = fv8N(fv8N(_mm256_div_ps(_mm256_set1_ps(1.0f), den)));
            const Fv8V Q{{qv, qv, qv, qv}};
            const Fv8V w = fv8Op<1, true>(k1, Q, kaccw, bad);
            if (!fv8Any(bad))
                _mm256_store_si256(reinterpret_cast<__m256i *>(wArr), fv8FtoiS<16>(fv8ClampI(w.c[3])));
            else
                fOk = false;
        }
        const Fv8V bv = fv8Op<15, false>(B, k16, z8, bad);
        const Fv8V in = fv8Op<15, true>(k2, k16, bv, bad);
        const Fv8V pr = fv8Op<15, false>(S3, in, z8, bad);
        __m128 oPr[8];
        if (fOk && !fv8Any(bad)) // FIX-E10a
        {
            Fv8V prOut;
            for (int c = 0; c < 4; ++c)
                prOut.c[c] = _mm256_castsi256_ps(fv8FtoiS<1>(fv8ClampI(pr.c[c])));
            fv8ToRows(prOut, oPr);
        }
        else
        {
            for (int j = 0; j < n; ++j)
            {
                const __m128 sbv = fmacV<4>(gB[j], vf16, zero);
                const __m128 sin = fmacV<2>(vf2, vf16, sbv);
                const __m128 sprod = fmacV<4>(gS3[j], sin, zero);
                oPr[j] = _mm_castsi128_ps(ftoiS<1>(clampI(sprod)));
                const __m128 smula = fmacV<4>(vf3, gA[j], zero); // FIX-E10a
                const __m128 sT5 = fmacV<2>(vf4, bc<3>(gA[j]), smula); // FIX-E10a
                const float sden = nrm1(_mm_cvtss_f32(bc<3>(sT5))); // FIX-E10a
                if (sden != 0.0f) // FIX-E10a
                {
                    const float sQ = normRes(normRes(1.0f / sden));
                    const float sWraw = _mm_cvtss_f32(bc<3>(fmacV<2>(vf1, _mm_set1_ps(sQ), accw)));
                    wArr[j] = _mm_cvtsi128_si32(ftoiS<16>(clampI(_mm_set1_ps(sWraw))));
                }
                else
                {
                    wArr[j] = 255 << 4;
                }
            }
        }
        for (int j = 0; j < n; ++j)
        {
            const int32_t v = t0 + j;
            ::RdVert &rv = outVerts[v];
            _mm_storeu_ps(rv.pos, gA[j]);
            const __m128i clamped = _mm_min_epu32(_mm_castps_si128(oPr[j]), v255);
            const __m128i p16 = _mm_packus_epi32(clamped, clamped);
            const __m128i p8 = _mm_packus_epi16(p16, p16);
            const uint32_t packed = static_cast<uint32_t>(_mm_cvtsi128_si32(p8));
            std::memcpy(rv.rgba, &packed, 4);
            rv.uv[0] = uArr[j];
            rv.uv[1] = vArr[j];
            rv.fog = static_cast<float>((static_cast<uint32_t>(wArr[j]) >> 4) & 0xFFu) / 255.0f; // FIX-E10a
            if (v >= 2 && !(static_cast<uint32_t>(idxs[v]) & 0x8000u))
            {
                outIndices[ic++] = baseIndex + static_cast<uint32_t>(v - 2);
                outIndices[ic++] = baseIndex + static_cast<uint32_t>(v - 1);
                outIndices[ic++] = baseIndex + static_cast<uint32_t>(v);
            }
        }
    }
    outIdxCount = ic;
}

static void buildLeanSkinVerts(const uint8_t *vu, int32_t N, bool vi10, uint32_t s2base, uint32_t s3base, const int32_t *idxs,
                               __m128 vf1, __m128 vf2, __m128 vf3, __m128 vf4, __m128 vf16, __m128 accw, // FIX-E10a
                               const Fv8V &k1, const Fv8V &k2, const Fv8V &k3, const Fv8V &k4, const Fv8V &k16, const Fv8V &kaccw, // FIX-E10a
                               ::RdVert *outVerts, uint32_t *outIndices, uint32_t baseIndex, uint32_t &outIdxCount) // FEAT-RD40
{
    if (vi10)
        buildLeanSkinVertsImpl<true>(vu, N, s2base, s3base, idxs, vf1, vf2, vf3, vf4, vf16, accw, k1, k2, k3, k4, k16, kaccw, outVerts, outIndices, baseIndex, outIdxCount); // FIX-E10a
    else
        buildLeanSkinVertsImpl<false>(vu, N, s2base, s3base, idxs, vf1, vf2, vf3, vf4, vf16, accw, k1, k2, k3, k4, k16, kaccw, outVerts, outIndices, baseIndex, outIdxCount); // FIX-E10a
}

static bool handler1EE0(Regs &R, uint8_t *vu, uint16_t &vi13, uint16_t top, bool vi10, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const bool hp = hprof::on(); // PROBE-RD41
    const uint64_t tEntry = hp ? __rdtsc() : 0ull; // PROBE-RD41
    const int32_t vi1s = static_cast<int16_t>(ilw(vu, 1011, 1)); // 1F00 ILW.y vi1, 1011 (< 0: rigid path at 2440 / 2670)
    const uint16_t vi12 = R.vi[12];
    const uint16_t w1018 = ilw(vu, 1018, 3); // vi5 (before the skinned path doubles it) = vertex count N
    const uint32_t s2base = static_cast<uint16_t>(ilw(vu, 1018, 0) + vi12); // vi2 = ILW.x 1018, += vi12 (2010 / 2490)
    const uint32_t s3base = static_cast<uint16_t>(ilw(vu, 1018, 1) + vi12); // vi3 = ILW.y 1018 + vi12 (1F60)
    const uint32_t i4base = static_cast<uint16_t>(ilw(vu, 1018, 2) + vi12); // vi4 = ILW.z 1018 + vi12 (1F48)
    const uint32_t ubase = ilw(vu, 1023, 2);                                // vi6 = ILW.z 1023 (output buffer)
    const uint16_t vi8 = static_cast<uint16_t>(w1018 + 32768u);              // two IADDIU 16384
    const uint32_t vf27x = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(vi8))); // MFIR.x vf27, vi8
    const uint32_t vf27y = ldw(vu, vi13, 1);                                                  // LQI.y vf27, (vi13++)
    if (vi1s < 0)
    {
        if (w1018 < 1u || w1018 > 300u || vi13 == 1022u || vi13 == 1023u || ((vi13 + 1u) & 0x3FFu) == 1023u)
            return fb1EE0(kFbRigidN);
        vi13 = static_cast<uint16_t>(vi13 + 1u);
        return rigid2440(R, vu, vi13, top, static_cast<int32_t>(w1018), i4base, s2base, s3base, ubase, vf27x, vf27y, vi10, kicks, gif);
    }
    // skinN guard: N in [1, 255]; every output qword ubase + 4N must fit in 1024 and each input stream (vi4/vi3/vi2 bases + N) must not wrap past 1024
    const auto skinNFail = [&]() {
        const uint32_t n = w1018;
        ++s_skinNHist[n <= 3u ? n : (n < 100u ? 4u : (n < 200u ? 5u : 6u))]; // 0..3, 68..99 (and 4..67 bounds fails), 100..199, 200+
        return fb1EE0(kFbSkinN);
    };
    if (w1018 < 1u || w1018 > kMaxSkinN)
        return skinNFail();
    const int32_t N = w1018;
    const uint32_t span = 4u * static_cast<uint32_t>(N) + 1u;
    if (ubase + span > 1024u || i4base + static_cast<uint32_t>(N) > 1024u || s3base + static_cast<uint32_t>(N) > 1024u || (!vi10 && s2base + static_cast<uint32_t>(N) > 1024u))
        return skinNFail();
    const auto hits = [&](uint32_t q) { return ((q - ubase) & 0x3FFu) < span; };
    if (hits(1012) || hits(1013) || hits(1023) || hits(vi13) || hits(vi13 + 1u))
        return fb1EE0(kFbSkinAlias13);
    vi13 = static_cast<uint16_t>(vi13 + 1u);

    // PERF-V1R: check contiguous input ranges [i4base, i4base+N), [s3base, s3base+N), [s2base, s2base+N) against output [ubase, ubase+span) once per call
    const uint32_t u0 = ubase, u1 = ubase + span;
    if ((i4base < u1 && u0 < i4base + static_cast<uint32_t>(N)) ||
        (s3base < u1 && u0 < s3base + static_cast<uint32_t>(N)) ||
        (!vi10 && s2base < u1 && u0 < s2base + static_cast<uint32_t>(N)))
        return fb1EE0(kFbSkinGatherAlias);

    // PERF-V1R: load vertex indices and track [minIdx, maxIdx] for O(1) bounding-box aliasing check
    int32_t idxs[kMaxSkinN + 8];
    int32_t minIdx = INT32_MAX, maxIdx = INT32_MIN;
    for (int32_t t = 0; t < N; ++t)
    {
        const int32_t idx = static_cast<int16_t>(ilw(vu, i4base + static_cast<uint32_t>(t), 0));
        idxs[t] = idx;
        if (idx < minIdx) minIdx = idx;
        if (idx > maxIdx) maxIdx = idx;
    }
    const int32_t maxOff = vi10 ? 796 : 716;
    if (636 + minIdx >= 0 && maxOff + maxIdx < 1024)
    {
        const bool aliasA = (static_cast<uint32_t>(636 + minIdx) < u1) && (u0 < static_cast<uint32_t>(636 + maxIdx + 1));
        const bool aliasB = (static_cast<uint32_t>(716 + minIdx) < u1) && (u0 < static_cast<uint32_t>(716 + maxIdx + 1));
        const bool alias2 = vi10 && (static_cast<uint32_t>(796 + minIdx) < u1) && (u0 < static_cast<uint32_t>(796 + maxIdx + 1));
        if (aliasA || aliasB || alias2)
        {
            for (int32_t t = 0; t < N; ++t)
            {
                const int32_t idx = idxs[t];
                if (hits(static_cast<uint32_t>(636 + idx)) || hits(static_cast<uint32_t>(716 + idx)) || (vi10 && hits(static_cast<uint32_t>(796 + idx))))
                    return fb1EE0(kFbSkinGatherAlias);
            }
        }
    }
    else
    {
        for (int32_t t = 0; t < N; ++t)
        {
            const int32_t idx = idxs[t];
            if (hits(static_cast<uint32_t>(636 + idx)) || hits(static_cast<uint32_t>(716 + idx)) || (vi10 && hits(static_cast<uint32_t>(796 + idx))))
                return fb1EE0(kFbSkinGatherAlias);
        }
    }

    const __m128 zero = _mm_setzero_ps();
    const __m128 vf1 = ldq(vu, 1016), vf2 = ldq(vu, 1014), vf3 = ldq(vu, 1002), vf4 = ldq(vu, 1003), vf16 = ldq(vu, 1013);
    // PERF-V1R: cache prepared matrix/constant rows across consecutive calls with bit-identical inputs
    struct alignas(32) SkinMatCache
    {
        bool valid = false;
        __m128i raw1, raw2, raw3, raw4, raw16;
        __m128 accw;
        Fv8V k1, k2, k3, k4, k16, kaccw;
    };
    static thread_local SkinMatCache s_matCache;
    const __m128i r1 = _mm_castps_si128(vf1), r2 = _mm_castps_si128(vf2), r3 = _mm_castps_si128(vf3), r4 = _mm_castps_si128(vf4), r16 = _mm_castps_si128(vf16);
    __m128i diff = _mm_xor_si128(r1, s_matCache.raw1);
    diff = _mm_or_si128(diff, _mm_xor_si128(r2, s_matCache.raw2));
    diff = _mm_or_si128(diff, _mm_xor_si128(r3, s_matCache.raw3));
    diff = _mm_or_si128(diff, _mm_xor_si128(r4, s_matCache.raw4));
    diff = _mm_or_si128(diff, _mm_xor_si128(r16, s_matCache.raw16));
    if (!s_matCache.valid || !_mm_testz_si128(diff, diff))
    {
        s_matCache.raw1 = r1;
        s_matCache.raw2 = r2;
        s_matCache.raw3 = r3;
        s_matCache.raw4 = r4;
        s_matCache.raw16 = r16;
        s_matCache.accw = fmacV<4>(_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), bc<2>(vf1), zero); // MULAz.w ACC, vf0, vf1
        s_matCache.k1 = fv8Const(vf1);
        s_matCache.k2 = fv8Const(vf2);
        s_matCache.k3 = fv8Const(vf3);
        s_matCache.k4 = fv8Const(vf4);
        s_matCache.k16 = fv8Const(vf16);
        s_matCache.kaccw = fv8Const(s_matCache.accw);
        s_matCache.valid = true;
    }
    const __m128 accw = s_matCache.accw;
    const Fv8V &k1 = s_matCache.k1, &k2 = s_matCache.k2, &k3 = s_matCache.k3, &k4 = s_matCache.k4, &k16 = s_matCache.k16, &kaccw = s_matCache.kaccw;

    const uint32_t m1012z = ldw(vu, 1012, 2), m1012w = ldw(vu, 1012, 3);
    const uint32_t ctlOld = ldw(vu, 1023, 3) & 0xFFFFu;
    const bool leanSkinEnabled = scene::leanSkinOn(); // FEAT-RD40
    const bool leanSkinCheck = scene::leanSkinCheckOn(); // FEAT-RD40
    const bool leanMainPass = (leanSkinEnabled || leanSkinCheck) && scene::isMainPassSkinned(vf27y); // FEAT-RD40
    const uint64_t tHdrEnd = hp ? __rdtsc() : 0ull; // PROBE-RD41
    if (leanSkinEnabled && leanMainPass && !leanSkinCheck) // FEAT-RD40
    {
        logRange(vu, ubase, 1u);
        stqRaw(vu, ubase, _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x))));
        stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(ctlOld), 2), static_cast<int>(ubase), 3));
        const uint64_t t1 = hp ? __rdtsc() : 0ull; // PROBE-RD41
        const bool useLeanKick = scene::leanKickOn(); // FEAT-RD42
        if (!kick(vu, ubase, kicks, gif, useLeanKick))
            return fb1EE0(kFbSkinKick);
        if (watch::spikeOn()) // PROBE-M8
        {
            float minW = 0.0f, maxW = 0.0f, maxAbsXY = 0.0f;
            bool first = true;
            for (int32_t v = 0; v < N; ++v)
                watch::m8AccumVert(ldq(vu, static_cast<uint32_t>(636 + idxs[v]) & 0x3FFu), first, minW, maxW, maxAbsXY);
            watch::m8EnrichDraw(scene::t_scene.curKickSerial & 0x3FFFFFFFu, vi10 ? 0x1EF0u : 0x1EE0u, 0u,
                                static_cast<uint32_t>(N), static_cast<uint32_t>(N), minW, maxW, maxAbsXY);
        }
        const uint64_t t2 = hp ? __rdtsc() : 0ull; // PROBE-RD41
        uint64_t vertsT = 0; // PROBE-RD41
        { scene::RecScope rm(scene::kRpMarker); scene::markGeometryKick(gif, kicks.back().first); }
        {
            scene::RecScope rs(scene::kRpSkinned);
            scene::SceneFrame &f = scene::t_scene.bufs[scene::t_scene.writeIdx];
            if (scene::beginLeanDraw(vu, f, static_cast<uint32_t>(N), vf3, vf4))
            {
                const size_t v0 = f.rdVerts.size();
                const size_t i0 = f.rdIndices.size();
                f.rdVerts.resize(v0 + static_cast<size_t>(N));
                if (N >= 3)
                {
                    scene::reserveGrow(f.rdIndices, i0 + static_cast<size_t>(N - 2) * 3u);
                    f.rdIndices.resize(i0 + static_cast<size_t>(N - 2) * 3u);
                }
                uint32_t ic = 0u;
                const uint64_t tv0 = hp ? __rdtsc() : 0ull; // PROBE-RD41
                buildLeanSkinVerts(vu, N, vi10, s2base, s3base, idxs, vf1, vf2, vf3, vf4, vf16, accw, k1, k2, k3, k4, k16, kaccw, // FIX-E10a
                                   f.rdVerts.data() + v0, N >= 3 ? f.rdIndices.data() + i0 : nullptr, static_cast<uint32_t>(v0), ic);
                vertsT = hp ? (__rdtsc() - tv0) : 0ull; // PROBE-RD41
                f.rdIndices.resize(i0 + ic);
                scene::pushLeanBatch(f, static_cast<uint32_t>(i0), ic);
            }
        }
        const uint64_t t3 = hp ? __rdtsc() : 0ull; // PROBE-RD41
        if (watch::on()) // PROBE-M5
            watch::checkDraw(vu, vi10 ? 0x1EF0u : 0x1EE0u, top, kicks.back().first, static_cast<uint32_t>(N), true, s2base, s3base, idxs, nullptr, nullptr);
        scene::leanSkinNoteDraw(true, false, useLeanKick); // FEAT-RD42
        R.vi[12] = static_cast<uint16_t>(ilw(vu, (i4base + 1u) & 0x3FFu, 0));
        if (hp) // PROBE-RD41
        {
            const uint64_t t4 = __rdtsc();
            hprof::g_st.eeHdr += tHdrEnd - tEntry;
            hprof::g_st.eeVerts += vertsT;
            hprof::g_st.eeRec += (t3 - t2) - vertsT;
            hprof::g_st.eeKick += t2 - t1;
            hprof::g_st.eeRest += (t1 - tHdrEnd) + (t4 - t3);
            hprof::g_st.eeNSum += static_cast<uint64_t>(N);
            ++hprof::g_st.eeCalls;
        }
        return true;
    }
    // The whole output span [ubase, ubase + 4N] is logged once (if any block falls back to interpreter, undoLog restores it), then written raw.
    logRange(vu, ubase, span);
    stqRaw(vu, ubase, _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x))));
    const uint64_t tVert0 = hp ? __rdtsc() : 0ull; // PROBE-RD41

    // PERF-SVF / PERF-V1R: gather 8 vertices directly into Fv8V, compute, convert, and fuse output stores
    for (int32_t t0 = 0; t0 < N; t0 += 8)
    {
        const int32_t n = (N - t0 < 8) ? (N - t0) : 8;
        __m128 gA[8], gB[8], gS3[8], gS2[8];
        if (n == 8)
        {
            for (int j = 0; j < 8; ++j)
            {
                const int32_t idx = idxs[t0 + j];
                gA[j] = ldq(vu, static_cast<uint32_t>(636 + idx));
                gB[j] = ldq(vu, static_cast<uint32_t>(716 + idx));
                gS3[j] = ldq(vu, s3base + static_cast<uint32_t>(t0 + j));
                gS2[j] = vi10 ? ldq(vu, static_cast<uint32_t>(796 + idx)) : ldq(vu, s2base + static_cast<uint32_t>(t0 + j));
            }
        }
        else
        {
            for (int j = 0; j < 8; ++j)
            {
                const int32_t s = t0 + (j < n ? j : n - 1);
                const int32_t idx = idxs[s];
                gA[j] = ldq(vu, static_cast<uint32_t>(636 + idx));
                gB[j] = ldq(vu, static_cast<uint32_t>(716 + idx));
                gS3[j] = ldq(vu, s3base + static_cast<uint32_t>(s));
                gS2[j] = vi10 ? ldq(vu, static_cast<uint32_t>(796 + idx)) : ldq(vu, s2base + static_cast<uint32_t>(s));
            }
        }
        Fv8V A = fv8FromRows(gA), B = fv8FromRows(gB), S3 = fv8FromRows(gS3), S2 = fv8FromRows(gS2);
        for (int c = 0; c < 4; ++c)
        {
            A.c[c] = fv8N(A.c[c]);
            B.c[c] = fv8N(B.c[c]);
            S3.c[c] = fv8N(S3.c[c]);
            S2.c[c] = fv8N(S2.c[c]);
        }
        Fv8V z8;
        z8.c[0] = z8.c[1] = z8.c[2] = z8.c[3] = _mm256_setzero_ps();
        __m256i bad = _mm256_setzero_si256();
        const Fv8V mula = fv8Op<15, false>(k3, A, z8, bad);
        const Fv8V t5 = fv8Op<15, true>(k4, Fv8V{{A.c[3], A.c[3], A.c[3], A.c[3]}}, mula, bad);
        const __m256 den = fv8N(t5.c[3]);
        bool ok = !fv8Any(fv8IsZ(den)); // zero den: scalar path below reports it (returns false)
        if (ok)
        {
            const __m256 qv = fv8N(fv8N(_mm256_div_ps(_mm256_set1_ps(1.0f), den)));
            const Fv8V Q{{qv, qv, qv, qv}};
            const Fv8V r7 = fv8Op<14, false>(t5, Q, z8, bad);
            const Fv8V os2q = fv8Op<14, false>(S2, Q, z8, bad);
            const Fv8V w = fv8Op<1, true>(k1, Q, kaccw, bad);
            const Fv8V bv = fv8Op<15, false>(B, k16, z8, bad);
            const Fv8V in = fv8Op<15, true>(k2, k16, bv, bad);
            const Fv8V pr = fv8Op<15, false>(S3, in, z8, bad);
            if (!fv8Any(bad))
            {
                alignas(32) int32_t wArr[8];
                _mm256_store_si256(reinterpret_cast<__m256i *>(wArr), fv8FtoiS<16>(fv8ClampI(w.c[3])));
                Fv8V prOut;
                for (int c = 0; c < 4; ++c)
                    prOut.c[c] = _mm256_castsi256_ps(fv8FtoiS<1>(fv8ClampI(pr.c[c])));
                Fv8V r7Out;
                r7Out.c[0] = _mm256_castsi256_ps(fv8FtoiS<16>(r7.c[0]));
                r7Out.c[1] = _mm256_castsi256_ps(fv8FtoiS<16>(r7.c[1]));
                r7Out.c[2] = _mm256_castsi256_ps(fv8FtoiS<16>(r7.c[2]));
                r7Out.c[3] = _mm256_castsi256_ps(_mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(idxs + t0)), _mm256_set1_epi32(0xFFFF)));
                __m128 oS2Q[8], oPr[8], oR7[8];
                fv8ToRows(os2q, oS2Q);
                fv8ToRows(prOut, oPr);
                fv8ToRows(r7Out, oR7);
                for (int j = 0; j < n; ++j)
                {
                    const uint32_t q0 = ubase + 1u + 4u * static_cast<uint32_t>(t0 + j);
                    std::memcpy(vu + ((q0 & 0x3FFu) << 4) + 12u, &wArr[j], 4);
                    stqRaw(vu, q0 + 1u, _mm_blend_ps(ldq(vu, q0 + 1u), oS2Q[j], 0x7));
                    stqRaw(vu, q0 + 2u, oPr[j]);
                    stqRaw(vu, q0 + 3u, oR7[j]);
                }
                continue;
            }
        }
        for (int j = 0; j < n; ++j) // scalar fallback (the original chain)
        {
            const __m128 sA = gA[j], B = gB[j], S3 = gS3[j], S2 = gS2[j];
            const __m128 mula = fmacV<4>(vf3, sA, zero);
            const __m128 T5 = fmacV<2>(vf4, bc<3>(sA), mula);
            const float den = nrm1(_mm_cvtss_f32(bc<3>(T5)));
            if (den == 0.0f)
                return fb1EE0(kFbSkinDivZero); // lbFull declines (di != 0)
            const float Q = normRes(normRes(1.0f / den));
            const __m128 qb = _mm_set1_ps(Q);
            const __m128 R7 = fmacV<4>(T5, qb, zero);
            const __m128 S2Q = fmacV<4>(S2, qb, zero);
            const float Wraw = _mm_cvtss_f32(bc<3>(fmacV<2>(vf1, qb, accw)));
            const __m128 bv = fmacV<4>(B, vf16, zero);
            const __m128 in = fmacV<2>(vf2, vf16, bv);
            const __m128 prod = fmacV<4>(S3, in, zero);
            const uint32_t q0 = ubase + 1u + 4u * static_cast<uint32_t>(t0 + j);
            const int32_t w0 = _mm_cvtsi128_si32(ftoiS<16>(clampI(_mm_set1_ps(Wraw))));
            stqRaw(vu, q0, _mm_castsi128_ps(_mm_insert_epi32(ldqi(vu, q0), w0, 3)));
            stqRaw(vu, q0 + 1u, _mm_blend_ps(ldq(vu, q0 + 1u), S2Q, 0x7));
            stqRaw(vu, q0 + 2u, _mm_castsi128_ps(ftoiS<1>(clampI(prod))));
            stqRaw(vu, q0 + 3u, _mm_castsi128_ps(_mm_insert_epi32(ftoiS<16>(R7), static_cast<int32_t>(static_cast<uint32_t>(idxs[t0 + j]) & 0xFFFFu), 3)));
        }
    }
    const uint64_t tVert1 = hp ? __rdtsc() : 0ull; // PROBE-RD41
    stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(ctlOld), 2), static_cast<int>(ubase), 3)); // ISW.z / ISW.w toggle
    const uint64_t tKick0 = hp ? __rdtsc() : 0ull; // PROBE-RD41
    if (!kick(vu, ubase, kicks, gif)) // XGKICK vi6 (JR delay slot)
        return fb1EE0(kFbSkinKick);
    if (watch::spikeOn()) // PROBE-M8
    {
        float minW = 0.0f, maxW = 0.0f, maxAbsXY = 0.0f;
        bool first = true;
        for (int32_t v = 0; v < N; ++v)
            watch::m8AccumVert(ldq(vu, static_cast<uint32_t>(636 + idxs[v]) & 0x3FFu), first, minW, maxW, maxAbsXY);
        watch::m8EnrichDraw(scene::t_scene.curKickSerial & 0x3FFFFFFFu, vi10 ? 0x1EF0u : 0x1EE0u, 0u,
                            static_cast<uint32_t>(N), static_cast<uint32_t>(N), minW, maxW, maxAbsXY);
    }
    if (watch::on()) // PROBE-M5
        watch::checkDraw(vu, vi10 ? 0x1EF0u : 0x1EE0u, top, kicks.back().first, static_cast<uint32_t>(N), true, s2base, s3base, idxs, nullptr, nullptr);
    const uint64_t tKick1 = hp ? __rdtsc() : 0ull; // PROBE-RD41
    if (scene::on()) // PERF-SA
    {
        scene::SceneFrame &f = scene::t_scene.bufs[scene::t_scene.writeIdx];
        const size_t v0Chk = f.rdVerts.size();
        const size_t i0Chk = f.rdIndices.size();
        scene::recordSkinned(vu, N, vi10, vf3, vf4, i4base, ubase, kicks.back().first, kicks.back().second, gif);
        if (leanSkinCheck) // FEAT-RD40
        {
            bool mism = false;
            if (leanMainPass && f.rdVerts.size() - v0Chk == static_cast<size_t>(N))
            {
                ::RdVert chkVerts[kMaxSkinN];
                uint32_t chkIdx[kMaxSkinN * 3u];
                uint32_t chkIc = 0u;
                buildLeanSkinVerts(vu, N, vi10, s2base, s3base, idxs, vf1, vf2, vf3, vf4, vf16, accw, k1, k2, k3, k4, k16, kaccw, // FIX-E10a
                                   chkVerts, chkIdx, static_cast<uint32_t>(v0Chk), chkIc);
                if (chkIc != f.rdIndices.size() - i0Chk)
                    mism = true;
                else if (chkIc > 0u && std::memcmp(chkIdx, f.rdIndices.data() + i0Chk, chkIc * sizeof(uint32_t)) != 0)
                    mism = true;
                for (int32_t v = 0; v < N && !mism; ++v)
                {
                    const ::RdVert &ref = f.rdVerts[v0Chk + static_cast<size_t>(v)];
                    if (std::memcmp(chkVerts[v].rgba, ref.rgba, 4) != 0)
                        mism = true;
                    for (int c = 0; c < 2 && !mism; ++c)
                    {
                        const float a = chkVerts[v].uv[c], b = ref.uv[c];
                        if (std::fabs(a - b) > 1e-5f * std::max(std::fabs(b), 1e-6f))
                            mism = true;
                    }
                }
                struct LeanFogStat // FIX-E10a
                {
                    uint32_t n = 0u, mismatches = 0u;
                    int32_t maxdiff = 0;
                    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                };
                static thread_local LeanFogStat s_leanFog; // FIX-E10a
                for (int32_t v = 0; v < N; ++v) // FIX-E10a
                {
                    const uint32_t qOut = (ubase + 1u + 4u * static_cast<uint32_t>(v)) & 0x3FFu;
                    const int32_t refF = static_cast<int32_t>((ldw(vu, qOut, 3u) >> 4) & 0xFFu);
                    const int32_t leanF = static_cast<int32_t>(std::lround(chkVerts[v].fog * 255.0f));
                    const int32_t diffF = std::abs(leanF - refF);
                    ++s_leanFog.n;
                    if (diffF > s_leanFog.maxdiff)
                        s_leanFog.maxdiff = diffF;
                    if (diffF != 0)
                    {
                        ++s_leanFog.mismatches;
                        mism = true;
                    }
                }
                const auto now = std::chrono::steady_clock::now(); // FIX-E10a
                if (std::chrono::duration<double>(now - s_leanFog.t0).count() >= 1.0) // FIX-E10a
                {
                    ::ps2xSafeLog("[leanfog] n=%u maxdiff=%d mismatches=%u\n", s_leanFog.n, s_leanFog.maxdiff, s_leanFog.mismatches);
                    s_leanFog.n = 0u;
                    s_leanFog.maxdiff = 0;
                    s_leanFog.mismatches = 0u;
                    s_leanFog.t0 = now;
                }
            }
            scene::leanSkinNoteDraw(leanMainPass, mism);
        }
        else if (leanSkinEnabled) // FEAT-RD40
            scene::leanSkinNoteDraw(false, false);
    }
    const uint64_t tRec1 = hp ? __rdtsc() : 0ull; // PROBE-RD41
    R.vi[12] = static_cast<uint16_t>(ilw(vu, (i4base + 1u) & 0x3FFu, 0)); // ILW.x vi12, 1(vi4) (read from VU mem: V[1] does not exist for N==1)
    if (hp) // PROBE-RD41
    {
        const uint64_t tEnd = __rdtsc();
        hprof::g_st.eeHdr += tHdrEnd - tEntry;
        hprof::g_st.eeVerts += tVert1 - tVert0;
        hprof::g_st.eeRec += tRec1 - tKick1;
        hprof::g_st.eeKick += tKick1 - tKick0;
        hprof::g_st.eeRest += (tVert0 - tHdrEnd) + (tKick0 - tVert1) + (tEnd - tRec1);
        hprof::g_st.eeNSum += static_cast<uint64_t>(N);
        ++hprof::g_st.eeCalls;
    }
    return true;
}

// ---- Task F3 handlers: 0AF0, 0B40 (+0BC8/0C28), 15E0, 1900, 1C48 (+1C90/1CB0), 3160 (+3268) ----
static bool handler0AF0(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t vi3 = ilw(vu, 1011, 0), vi2 = ilw(vu, 1011, 3);
    const uint32_t count = static_cast<uint32_t>(vi2) + 1u;
    if (count > 512u)
        return false;
    const uint16_t base = static_cast<uint16_t>(vi3 + 716u);
    if (((vi13 - base) & 0x3FFu) < count)
        return false;
    const __m128 zero = _mm_setzero_ps();
    logRange(vu, base, count);
    for (uint32_t i = 0; i < count; ++i)
        stqRaw(vu, static_cast<uint32_t>(base) + i, zero);
    R.vf[1] = zero;
    R.vi[2] = vi2;
    R.vi[3] = vi3;
    R.vi[4] = static_cast<uint16_t>(base + vi2);
    R.vfValid = (R.vfValid & ~0x00000002u) | (1u << 1); // PERF-V1P
    R.viValid = (R.viValid & ~0x001Eu) | (1u << 2) | (1u << 3) | (1u << 4);
    return true;
}

static bool handler0B40(Regs &R, uint8_t *vu, uint16_t vi13)
{
    uint16_t vi1 = static_cast<uint16_t>(ilw(vu, 1018, 2) + R.vi[12]);
    const uint16_t n = ilw(vu, 1018, 3);
    if (n == 0u || n > 512u)
        return false;
    if (((vi13 - vi1) & 0x3FFu) < n)
        return false;
    const uint16_t vi2 = static_cast<uint16_t>(vi1 + n + 2u);
    const __m128 zero = _mm_setzero_ps();
    __m128 vf1 = _mm_blend_ps(_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), ldq(vu, static_cast<uint32_t>(vi13) - 1u), 0x2);
    __m128 vf4 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const __m128 vf11 = _mm_set1_ps(1.0f);
    uint16_t vi6 = ilw(vu, vi1, 0);
    const uint16_t vi5 = ilw(vu, static_cast<uint16_t>(vi1 + 1u), 0);
    __m128 vf12 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi6)));
    __m128 vf2 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi5)));
    vf12 = _mm_blend_ps(vf12, fmacV<0>(zero, bc<3>(vf12), zero), 0x4);
    vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
    __m128 vf3 = norm4(vf12);
    vi1 = static_cast<uint16_t>(vi1 + 2u);
    uint16_t vi7 = ilw(vu, vi1, 0);
    uint16_t vi8 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 1);
    uint16_t vi9 = 0u;
    __m128 vf5 = zero, acc = zero;
    bool macSignX = false; // 0BB0 MUL.x vf0, vf0, vf0 -> +0.0f (S=0)
    for (uint32_t k = 0; k < n; ++k)
    {
        vf5 = norm4(vf3);
        vi1 = static_cast<uint16_t>(vi1 + 1u);
        vf3 = norm4(vf2);
        vi9 = macSignX ? 128u : 0u; // 0BD0 FMAND vi9, vi3 (vi3=128) reads 0C10 MULy.x from k-1
        vf2 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi7)));
        acc = fmacV<0>(vf4, bc<1>(vf4), zero);
        vi9 = static_cast<uint16_t>(vi9 + 32767u);
        vf4 = _mm_blend_ps(vf4, fmacV<2>(vf11, bc<2>(vf4), acc), 0x1);
        vi8 = static_cast<uint16_t>(vi9 + vi8);
        const __m128 vf5_yzx = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 vf3_zxy = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 1, 0, 2));
        acc = fmacV<4>(vf5_yzx, vf3_zxy, zero);
        vi8 = static_cast<uint16_t>(vi8 & 0x8000u);
        const __m128 vf3_yzx = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 vf5_zxy = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 1, 0, 2));
        vf5 = _mm_blend_ps(vf5, fmacV<3>(vf3_yzx, vf5_zxy, acc), 0x7);
        vi6 = static_cast<uint16_t>(vi6 | vi8);
        vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
        const uint32_t qdst = static_cast<uint16_t>(vi1 - 3u);
        stqi(vu, qdst, _mm_insert_epi32(ldqi(vu, qdst), static_cast<int32_t>(vi6), 0));
        vi6 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 0);
        // 0C10 MULy.x vf0, vf4, vf1: sign bit of exact double product (execUpper/fastUpper flagsOf)
        macSignX = std::signbit(static_cast<double>(nrm1(_mm_cvtss_f32(vf4))) * static_cast<double>(nrm1(_mm_cvtss_f32(bc<1>(vf1)))));
        vi7 = ilw(vu, vi1, 0);
        vf4 = fmacV<4>(vf5, vf2, zero);
        vi8 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 1);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[5] = vf5; R.vf[11] = vf11; R.vf[12] = vf12;
    R.acc = acc;
    R.vi[2] = vi2; R.vi[3] = 128u; R.vi[4] = 0x8000u; R.vi[5] = vi5; R.vi[6] = vi6; R.vi[7] = vi7; R.vi[8] = vi8; R.vi[9] = vi9;
    R.vfValid = (R.vfValid & ~0x00020000u) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 11) | (1u << 12); // PERF-V1P
    R.viValid = (R.viValid & ~0x0002u) | 0x03FCu;
    return true;
}

static bool handler15E0(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t vi1 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 512u)
        return false;
    const uint16_t base1 = static_cast<uint16_t>(vi1 + 556u), base2 = static_cast<uint16_t>(vi1 + 716u);
    const uint16_t nextVi13 = static_cast<uint16_t>(vi13 + 3u);
    if (((nextVi13 - base2) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), half = _mm_set1_ps(0.5f);
    __m128 vf3 = ldq(vu, vi13);
    const __m128 m0 = ldq(vu, 989), m1 = ldq(vu, 990), m2 = ldq(vu, 991);
    vf3 = fmacV<2>(m2, bc<2>(vf3), fmacV<2>(m1, bc<1>(vf3), fmacV<4>(m0, bc<0>(vf3), zero)));
    const __m128 vf1 = ldq(vu, static_cast<uint16_t>(vi13 + 1u));
    const __m128 vf2 = ldq(vu, static_cast<uint16_t>(vi13 + 2u));
    vi13 = nextVi13;
    const __m128 acc05 = fmacV<0>(zero, half, zero);
    logRange(vu, base2, n);
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 nrm = ldq(vu, static_cast<uint32_t>(base1) + k);
        const __m128 p = fmacV<4>(vf3, nrm, zero);
        const __m128 d = fmacV<0>(fmacV<0>(p, bc<1>(p), zero), bc<2>(p), zero);
        const __m128 pos = fmacV<2>(d, half, acc05);
        const __m128 neg = fmacV<3>(d, half, acc05);
        const __m128 c0 = ldq(vu, static_cast<uint32_t>(base2) + k);
        const __m128 ac0 = fmacV<0>(c0, zero, zero);
        const __m128 ac1 = fmacV<2>(vf1, bc<0>(pos), ac0);
        const __m128 out = fmacV<2>(vf2, bc<0>(neg), ac1);
        stqRaw(vu, static_cast<uint32_t>(base2) + k, out);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3;
    R.I = 0.5f; R.iValid = true; // PERF-V1Q: 1650 I=0.5f
    R.vfValid = (R.vfValid & ~0x00001FFEu) | (1u << 1) | (1u << 2) | (1u << 3); // PERF-V1P: 15E0 touches vf4..12, vi1..3
    R.viValid &= ~0x000Eu;
    return true;
}

static bool handler1900(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t vi2)
{
    const uint16_t vi11 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 512u)
        return false;
    const uint16_t baseSrc = static_cast<uint16_t>(vi11 + 556u), baseDst = static_cast<uint16_t>(vi11 + 796u);
    if (((vi13 - baseDst) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 vf9 = ldq(vu, vi2), vf10 = ldq(vu, static_cast<uint32_t>(vi2) + 1u);
    const __m128 vf11 = ldq(vu, static_cast<uint32_t>(vi2) + 2u), vf12 = ldq(vu, static_cast<uint32_t>(vi2) + 3u);
    const __m128 m986 = ldq(vu, 986), m987 = ldq(vu, 987), m988 = ldq(vu, 988);
    const auto mulMat = [&](__m128 c) {
        __m128 a = fmacV<4>(vf9, bc<0>(c), zero);
        a = fmacV<2>(vf10, bc<1>(c), a);
        a = fmacV<2>(vf11, bc<2>(c), a);
        return fmacV<2>(vf12, bc<3>(c), a);
    };
    const __m128 r2 = mulMat(m987), r3 = mulMat(m988), r12 = mulMat(m986);
    logRange(vu, baseDst, n);
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 v = ldq(vu, static_cast<uint32_t>(baseSrc) + k);
        __m128 a = fmacV<4>(r12, bc<0>(v), zero);
        a = fmacV<2>(r2, bc<1>(v), a);
        a = fmacV<2>(r3, bc<2>(v), a);
        const __m128 res = fmacV<2>(vf12, one, a);
        const uint32_t qd = static_cast<uint32_t>(baseDst) + k;
        stqRaw(vu, qd, _mm_blend_ps(ldq(vu, qd), res, 0x7));
    }
    R.vf[2] = r2; R.vf[3] = r3; R.vf[4] = vf12; R.vf[9] = _mm_blend_ps(R.vf[9], vf9, 0x7); R.vf[12] = r12;
    R.vi[4] = 556u; // PERF-V1P: 19A0 IADDIU vi4, vi0, 556; 1908 LQ.xyz vf9, 0(vi2)
    R.vfValid = (R.vfValid & ~0x00101C3Eu) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 9) | (1u << 12); // PERF-V1P
    R.viValid = (R.viValid & ~0x080Eu) | (1u << 4);
    return true;
}

static bool handler1C48(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t vi1_0 = ilw(vu, 1018, 0), n = ilw(vu, 1018, 3);
    if (n == 0u || n > 1024u)
        return false;
    const __m128 vf1 = ldq(vu, vi13);
    const __m128 vf2 = ldq(vu, static_cast<uint16_t>(vi13 + 1u));
    const __m128 vf3 = ldq(vu, static_cast<uint16_t>(vi13 + 2u));
    const __m128 vf4 = ldq(vu, static_cast<uint16_t>(vi13 + 3u));
    vi13 = static_cast<uint16_t>(vi13 + 4u);
    const uint16_t base = static_cast<uint16_t>(vi1_0 + R.vi[12]);
    if (((vi13 - base) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    logRange(vu, base, n);
    __m128 vf6 = zero, acc = zero;
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 v = ldq(vu, static_cast<uint32_t>(base) + k);
        acc = fmacV<4>(vf1, bc<0>(v), zero);
        acc = fmacV<2>(vf2, bc<1>(v), acc);
        acc = fmacV<2>(vf3, bc<2>(v), acc);
        vf6 = fmacV<2>(vf4, one, acc);
        stqRaw(vu, static_cast<uint32_t>(base) + k, vf6);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[6] = vf6;
    R.acc = acc;
    R.vi[2] = static_cast<uint16_t>(base + n + 1u);
    R.vfValid = (R.vfValid & ~0x0000005Eu) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6); // PERF-V1P
    R.viValid = (R.viValid & ~0x000Au) | (1u << 2);
    return true;
}

static bool handler3160(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t vi1 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n < 2u || n > 512u)
        return false;
    const uint16_t base = static_cast<uint16_t>(vi1 + 636u);
    if (((vi13 - base) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    __m128 vf4 = ldq(vu, base);
    __m128 vf1 = fmacV<1>(ldq(vu, base), one, zero); // SUBi.w vf1, vf1, vf0 (I = 1.0f)
    for (uint32_t k = 1; k < n; ++k)
    {
        const __m128 vf2 = ldq(vu, static_cast<uint32_t>(base) + k);
        vf4 = miniV(vf4, vf1);
        vf1 = fmacV<1>(vf2, one, zero);
    }
    vf4 = miniV(vf4, vf1);
    const __m128 vf3c = ldq(vu, 1008), vf1z = ldq(vu, 1006), vf2z = ldq(vu, 1007);
    __m128 v1x = fmacV<1>(zero, bc<3>(vf4), zero); // 31E8 SUBw.x vf1, vf0, vf4
    v1x = maxV(v1x, vf3c);                         // 31F0 MAX.x vf1, vf1, vf3
    const __m128 v2x = miniV(v1x, zero);           // 31F8 MINI.x vf2, vf1, vf0
    const __m128 inp0 = ldq(vu, base);
    const __m128 v2w = fmacV<0>(bc<3>(inp0), bc<0>(v2x), zero); // 3200 ADDx.w vf2, vf1, vf2
    const float q = divQ2(_mm_cvtss_f32(v2w), _mm_cvtss_f32(bc<3>(inp0)));
    const __m128 qb = _mm_set1_ps(q);
    logRange(vu, base, n);
    __m128 vf3 = zero, acc = zero;
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 inp = ldq(vu, static_cast<uint32_t>(base) + k);
        const __m128 xyw = fmacV<4>(inp, qb, zero);
        acc = fmacV<4>(vf1z, bc<3>(xyw), zero);
        const __m128 z = fmacV<2>(vf2z, one, acc);
        vf3 = _mm_blend_ps(xyw, z, 0x4);
        stqRaw(vu, static_cast<uint32_t>(base) + k, vf3);
    }
    R.vf[3] = vf3; R.vf[4] = vf4;
    R.acc = acc;
    R.I = 1.0f; R.iValid = true; // PERF-V1Q: 3198 I=1.0f
    R.vfValid = (R.vfValid & ~0x0000001Eu) | (1u << 3) | (1u << 4); // PERF-V1P
    R.viValid &= ~0x0006u;
    return true;
}

// PERF-V1M: 4-term specular polynomial c0*d + c1*d^2 + c2*d^4 + c3*d^8 (shared by 0840 and 1090)
inline __m128 poly4Spec(__m128 d1, __m128 coeffs)
{
    const __m128 zero = _mm_setzero_ps();
    const __m128 d2 = fmacV<4>(d1, d1, zero);
    const __m128 d4 = fmacV<4>(d2, d2, zero);
    const __m128 d8 = fmacV<4>(d4, d4, zero);
    const __m128 a0 = fmacV<4>(d1, bc<0>(coeffs), zero);
    const __m128 a1 = fmacV<2>(d2, bc<1>(coeffs), a0);
    const __m128 a2 = fmacV<2>(d4, bc<2>(coeffs), a1);
    return fmacV<2>(d8, bc<3>(coeffs), a2);
}

// PERF-V1M: handler 0840 (0840..0AD8, 84 pairs) -- point specular light (Blinn-Phong half-vector + 4-term polynomial, 1/d attenuation)
static bool handler0840(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 256u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmdPos = ldq(vu, vi13);
    const __m128 cmdCol = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    vi13 = static_cast<uint16_t>(vi13 + 2u);
    const __m128 m982 = ldq(vu, 982), m983 = ldq(vu, 983), m984 = ldq(vu, 984), m985 = ldq(vu, 985);
    const __m128 L = fmacV<2>(m985, one, fmacV<2>(m984, bc<2>(cmdPos), fmacV<2>(m983, bc<1>(cmdPos), fmacV<4>(m982, bc<0>(cmdPos), zero))));
    const __m128 m989 = ldq(vu, 989), m990 = ldq(vu, 990), m991 = ldq(vu, 991), v1017 = ldq(vu, 1017);
    const __m128 hOff = fmacV<2>(m991, bc<2>(v1017), fmacV<2>(m990, bc<1>(v1017), fmacV<4>(m989, bc<0>(v1017), zero)));
    const __m128 coeffs = ldq(vu, 1015);
    if (scene::on()) // FEAT-V2
        scene::recordLight1(0x0840u, cmdPos, zero, cmdCol, coeffs); // FEAT-V2
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 pVec = _mm_set1_ps(erlenP(dVec));
        const __m128 dir = fmacV<4>(dVec, pVec, zero);
        const __m128 H = fmacV<0>(hOff, dir, zero);
        const __m128 H2 = fmacV<4>(H, H, zero);
        const __m128 hLen2 = fmacV<2>(one, bc<2>(H2), fmacV<2>(one, bc<1>(H2), fmacV<4>(one, bc<0>(H2), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(hLen2)));
        const __m128 hNorm = fmacV<4>(H, qVec, zero);
        const __m128 prod = fmacV<4>(Nrm, hNorm, zero);
        const __m128 dotNH = fmacV<0>(fmacV<0>(prod, bc<1>(prod), zero), bc<2>(prod), zero);
        const __m128 spec = maxV(poly4Spec(dotNH, coeffs), zero);
        const __m128 atten = fmacV<4>(spec, pVec, zero);
        const __m128 contrib = fmacV<4>(cmdCol, bc<0>(atten), zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid &= ~0x0000FFFEu; // PERF-V1P: 0840 touches vf1..15, vi1..2
    R.viValid &= ~0x0006u;
    return true;
}

// PERF-V1M: handler 0CC0 (0CC0..1058, 116 pairs) -- diffuse spotlight with two cone-cull early-outs (FMAND Sx)
static bool handler0CC0(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 256u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmd0 = ldq(vu, vi13);
    const __m128 cmd1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    const __m128 cmd2 = ldq(vu, static_cast<uint32_t>(vi13) + 2u);
    const __m128 cmd3 = ldq(vu, static_cast<uint32_t>(vi13) + 3u);
    vi13 = static_cast<uint16_t>(vi13 + 4u);
    if (scene::on()) // FEAT-V2
        scene::recordLight1(0x0CC0u, cmd0, cmd2, cmd1, cmd3); // FEAT-V2
    const __m128 m989 = ldq(vu, 989), m990 = ldq(vu, 990), m991 = ldq(vu, 991), v985 = ldq(vu, 985), v1010 = ldq(vu, 1010);
    const __m128 vf9 = fmacV<2>(m991, bc<2>(cmd0), fmacV<2>(m990, bc<1>(cmd0), fmacV<4>(m989, bc<0>(cmd0), zero)));
    const __m128 negCmd2 = fmacV<1>(zero, cmd2, zero);
    const __m128 vf5w = fmacV<4>(v1010, cmd3, zero);
    const __m128 D = fmacV<2>(m991, bc<2>(negCmd2), fmacV<2>(m990, bc<1>(negCmd2), fmacV<4>(m989, bc<0>(negCmd2), zero)));
    const __m128 vf7a = fmacV<4>(D, bc<3>(vf5w), zero);
    const __m128 L = fmacV<0>(vf9, v985, zero);
    const __m128 vf1 = fmacV<1>(v1010, vf7a, zero);
    const __m128 vf7b = fmacV<1>(L, vf1, zero);
    const __m128 vf6sq = fmacV<4>(vf7b, vf7b, zero);
    const __m128 vf7dot = fmacV<4>(vf7b, D, zero);
    const __m128 distSq = fmacV<2>(one, bc<2>(vf6sq), fmacV<0>(vf6sq, bc<1>(vf6sq), zero));
    const __m128 dotVal = fmacV<2>(one, bc<2>(vf7dot), fmacV<0>(vf7dot, bc<1>(vf7dot), zero));
    if ((_mm_movemask_ps(dotVal) & 1) != 0) // 0DB0 FMAND vi5, vi4 (Sx of 0D90 MADDz.x) -> early exit
        return true;
    const __m128 dotSq = fmacV<4>(dotVal, dotVal, zero);
    const __m128 distCosSq = fmacV<4>(distSq, bc<2>(cmd3), zero);
    const __m128 coneTest = fmacV<1>(dotSq, distCosSq, zero);
    if ((_mm_movemask_ps(coneTest) & 1) != 0) // 0DE0 FMAND vi4, vi4 (Sx of 0DC0 SUB.x) -> early exit
        return true;
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 d2Vec = fmacV<4>(dVec, dVec, zero);
        const __m128 dLen2 = fmacV<2>(one, bc<2>(d2Vec), fmacV<2>(one, bc<1>(d2Vec), fmacV<4>(one, bc<0>(d2Vec), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(dLen2)));
        const __m128 dir = fmacV<4>(dVec, qVec, zero);
        const __m128 spotProd = fmacV<4>(dir, D, zero);
        const __m128 nrmProd = fmacV<4>(dir, Nrm, zero);
        const __m128 cosSpot = fmacV<2>(one, bc<2>(spotProd), fmacV<0>(spotProd, bc<1>(spotProd), zero));
        const __m128 nDotL = fmacV<2>(one, bc<2>(nrmProd), fmacV<0>(nrmProd, bc<1>(nrmProd), zero));
        const __m128 spotSc = fmacV<4>(fmacV<1>(cosSpot, bc<0>(cmd3), zero), bc<1>(cmd3), zero);
        const __m128 spotClamp = miniV(maxV(spotSc, zero), one);
        const __m128 nlClamp = miniV(maxV(nDotL, zero), one);
        const __m128 atten = fmacV<4>(qVec, fmacV<4>(spotClamp, nlClamp, zero), zero);
        const __m128 contrib = fmacV<4>(cmd1, bc<0>(atten), zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid &= ~0x00001FFEu; // PERF-V1P: 0CC0 touches vf1..12, vi1..5
    R.viValid &= ~0x003Eu;
    return true;
}

// PERF-V1M: handler 1090 (1090..1530, 149 pairs) -- specular spotlight (cone falloff * Blinn-Phong polynomial, 1/d attenuation)
static bool handler1090(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 256u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmd0 = ldq(vu, vi13);
    const __m128 cmd1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    const __m128 cmd2 = ldq(vu, static_cast<uint32_t>(vi13) + 2u);
    const __m128 cmd3 = ldq(vu, static_cast<uint32_t>(vi13) + 3u);
    vi13 = static_cast<uint16_t>(vi13 + 4u);
    if (scene::on()) // FEAT-V2
        scene::recordLight1(0x1090u, cmd0, cmd2, cmd1, cmd3); // FEAT-V2
    const __m128 m989 = ldq(vu, 989), m990 = ldq(vu, 990), m991 = ldq(vu, 991), v985 = ldq(vu, 985), v1017 = ldq(vu, 1017);
    const __m128 L = fmacV<2>(v985, one, fmacV<2>(m991, bc<2>(cmd0), fmacV<2>(m990, bc<1>(cmd0), fmacV<4>(m989, bc<0>(cmd0), zero))));
    const __m128 negCmd2 = fmacV<1>(zero, cmd2, zero);
    const __m128 D = fmacV<2>(m991, bc<2>(negCmd2), fmacV<2>(m990, bc<1>(negCmd2), fmacV<4>(m989, bc<0>(negCmd2), zero)));
    const __m128 hOff = fmacV<2>(m991, bc<2>(v1017), fmacV<2>(m990, bc<1>(v1017), fmacV<4>(m989, bc<0>(v1017), zero)));
    const __m128 coeffs = ldq(vu, 1015);
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 pVec = _mm_set1_ps(erlenP(dVec));
        const __m128 dir = fmacV<4>(dVec, pVec, zero);
        const __m128 spotProd = fmacV<4>(D, dir, zero);
        const __m128 cosSpot = fmacV<0>(fmacV<0>(spotProd, bc<1>(spotProd), zero), bc<2>(spotProd), zero);
        const __m128 spotSub = maxV(fmacV<1>(cosSpot, bc<0>(cmd3), zero), zero);
        const __m128 spotClamp = miniV(fmacV<4>(spotSub, bc<1>(cmd3), zero), one);
        const __m128 H = fmacV<0>(hOff, dir, zero);
        const __m128 H2 = fmacV<4>(H, H, zero);
        const __m128 hLen2 = fmacV<2>(one, bc<2>(H2), fmacV<2>(one, bc<1>(H2), fmacV<4>(one, bc<0>(H2), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(hLen2)));
        const __m128 hNorm = fmacV<4>(H, qVec, zero);
        const __m128 nrmProd = fmacV<4>(Nrm, hNorm, zero);
        const __m128 dotNH = fmacV<0>(fmacV<0>(nrmProd, bc<1>(nrmProd), zero), bc<2>(nrmProd), zero);
        const __m128 x = fmacV<4>(dotNH, spotClamp, zero);
        const __m128 spec = maxV(poly4Spec(x, coeffs), zero);
        const __m128 atten = fmacV<4>(pVec, spec, zero);
        const __m128 contrib = fmacV<4>(cmd1, bc<0>(atten), zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid &= ~0x000FFFFEu; // PERF-V1Q: 1090 touches vf1..19, vi1..4
    R.viValid &= ~0x001Eu;
    return true;
}

// PERF-V1N: handler 02E0 (02E0..0550, 80 pairs) -- 4-directional specular light (4x4 color matrix * poly4Spec)
static bool handler02E0(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 156u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps();
    const __m128 d0 = ldq(vu, vi13);
    const __m128 d1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    const __m128 d2 = ldq(vu, static_cast<uint32_t>(vi13) + 2u);
    const __m128 c0 = ldq(vu, static_cast<uint32_t>(vi13) + 3u);
    const __m128 c1 = ldq(vu, static_cast<uint32_t>(vi13) + 4u);
    const __m128 c2 = ldq(vu, static_cast<uint32_t>(vi13) + 5u);
    const __m128 c3 = ldq(vu, static_cast<uint32_t>(vi13) + 6u);
    vi13 = static_cast<uint16_t>(vi13 + 7u);
    const __m128 m986 = ldq(vu, 986), m987 = ldq(vu, 987), m988 = ldq(vu, 988);
    const __m128 Lx = fmacV<2>(d2, bc<2>(m986), fmacV<2>(d1, bc<1>(m986), fmacV<4>(d0, bc<0>(m986), zero)));
    const __m128 Ly = fmacV<2>(d2, bc<2>(m987), fmacV<2>(d1, bc<1>(m987), fmacV<4>(d0, bc<0>(m987), zero)));
    const __m128 Lz = fmacV<2>(d2, bc<2>(m988), fmacV<2>(d1, bc<1>(m988), fmacV<4>(d0, bc<0>(m988), zero)));
    const __m128 coeffs = ldq(vu, 1015);
    if (scene::on()) // FEAT-V2
        scene::recordLight4(0x02E0u, d0, d1, d2, c0, c1, c2, c3, coeffs); // FEAT-V2
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dot = fmacV<2>(Lz, bc<2>(Nrm), fmacV<2>(Ly, bc<1>(Nrm), fmacV<4>(Lx, bc<0>(Nrm), zero)));
        const __m128 spec = maxV(poly4Spec(dot, coeffs), zero);
        const __m128 contrib = fmacV<2>(c3, bc<3>(spec), fmacV<2>(c2, bc<2>(spec), fmacV<2>(c1, bc<1>(spec), fmacV<4>(c0, bc<0>(spec), zero))));
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid = (R.vfValid & ~0x0007FFFEu) | 0x00078000u; // PERF-V1Q: 02E0 leaves vf15..18 valid for 0560
    const __m128 nrmNp1 = ldq(vu, static_cast<uint32_t>(base) + 556u + n + 1u);
    const __m128 dotNp1 = fmacV<2>(Lz, bc<2>(nrmNp1), fmacV<2>(Ly, bc<1>(nrmNp1), fmacV<4>(Lx, bc<0>(nrmNp1), zero)));
    const __m128 nrmNp2 = ldq(vu, static_cast<uint32_t>(base) + 556u + n + 2u);
    const __m128 dotNp2 = fmacV<2>(Lz, bc<2>(nrmNp2), fmacV<2>(Ly, bc<1>(nrmNp2), fmacV<4>(Lx, bc<0>(nrmNp2), zero)));
    const __m128 d2Np2 = fmacV<4>(dotNp2, dotNp2, zero);
    const __m128 d4Np2 = fmacV<4>(d2Np2, d2Np2, zero);
    R.vf[15] = fmacV<4>(d4Np2, d4Np2, zero);
    R.vf[16] = maxV(poly4Spec(dotNp1, coeffs), zero);
    R.vf[17] = d2Np2;
    R.vf[18] = d4Np2;
    R.viValid &= ~0x000Eu;
    return true;
}

// PERF-V1P: handler 0628 (0628..0830, 67 pairs) -- point diffuse light (RSQRT 1/d), preceded by 3 live-in stores SQ vf11..13 -> 17/19/21(vi4)
static bool handler0628(Regs &R, uint8_t *vu, uint16_t &vi13, bool prologue) // PERF-BI
{
    if (prologue) // PERF-BI
    { // PERF-BI
        if ((R.viValid & (1u << 4)) == 0u || (R.vfValid & ((1u << 11) | (1u << 12) | (1u << 13))) != ((1u << 11) | (1u << 12) | (1u << 13)))
            return false;
        stq(vu, static_cast<uint32_t>(R.vi[4]) + 17u, R.vf[11]);
        stq(vu, static_cast<uint32_t>(R.vi[4]) + 19u, R.vf[12]);
        stq(vu, static_cast<uint32_t>(R.vi[4]) + 21u, R.vf[13]);
    } // PERF-BI
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 240u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmd0 = ldq(vu, vi13);
    const __m128 cmd1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    vi13 = static_cast<uint16_t>(vi13 + 2u);
    if (scene::on()) // FEAT-V2
        scene::recordLight1(0x0628u, cmd0, zero, cmd1, zero); // FEAT-V2
    const __m128 m982 = ldq(vu, 982), m983 = ldq(vu, 983), m984 = ldq(vu, 984), v985 = ldq(vu, 985);
    const __m128 L = fmacV<0>(fmacV<2>(m984, bc<2>(cmd0), fmacV<2>(m983, bc<1>(cmd0), fmacV<4>(m982, bc<0>(cmd0), zero))), v985, zero);
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 d2Vec = fmacV<4>(dVec, dVec, zero);
        const __m128 d2 = fmacV<2>(one, bc<2>(d2Vec), fmacV<2>(one, bc<1>(d2Vec), fmacV<4>(one, bc<0>(d2Vec), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(d2)));
        const __m128 q2 = fmacV<4>(qVec, qVec, zero);
        const __m128 prod = fmacV<4>(dVec, Nrm, zero);
        const __m128 dot = fmacV<0>(fmacV<0>(prod, bc<1>(prod), zero), bc<2>(prod), zero);
        const __m128 dotPos = maxV(dot, zero);
        const __m128 atten = fmacV<4>(q2, dotPos, zero);
        const __m128 contrib = fmacV<4>(cmd1, bc<0>(atten), zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid &= ~0x00000FFEu;
    R.viValid &= ~0x000Eu;
    return true;
}

// PERF-V1N: handler 1720 (1720..18E8, 59 pairs) -- point ambient/falloff light (quadratic distance clamp via SQRT)
static bool handler1720(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 240u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmd0 = ldq(vu, vi13);
    const __m128 cmd1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    const __m128 cmd2 = ldq(vu, static_cast<uint32_t>(vi13) + 2u);
    vi13 = static_cast<uint16_t>(vi13 + 3u);
    if (scene::on()) // FEAT-V2
        scene::recordLight1(0x1720u, cmd0, zero, cmd1, cmd2); // FEAT-V2
    const __m128 m982 = ldq(vu, 982), m983 = ldq(vu, 983), m984 = ldq(vu, 984), v985 = ldq(vu, 985);
    const __m128 L = fmacV<0>(fmacV<2>(m984, bc<2>(cmd0), fmacV<2>(m983, bc<1>(cmd0), fmacV<4>(m982, bc<0>(cmd0), zero))), v985, zero);
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 dSq = fmacV<4>(dVec, dVec, zero);
        const __m128 d2 = fmacV<2>(one, bc<2>(dSq), fmacV<0>(dSq, bc<1>(dSq), zero));
        const __m128 qVec = _mm_set1_ps(sqrtQ(_mm_cvtss_f32(d2)));
        const __m128 t0 = fmacV<4>(d2, cmd2, zero);
        const __m128 t1 = fmacV<4>(cmd2, qVec, zero);
        const __m128 t2 = fmacV<0>(t0, bc<1>(t1), zero);
        const __m128 t3 = fmacV<0>(t2, bc<2>(cmd2), zero);
        const __m128 atten = miniV(maxV(t3, zero), bc<3>(cmd2));
        const __m128 contrib = fmacV<4>(cmd1, bc<0>(atten), zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, fmacV<0>(Col, contrib, zero));
    }
    R.vfValid &= ~0x00000FFEu; // PERF-V1P: 1720 touches vf1..11, vi1..3
    R.viValid &= ~0x000Eu;
    return true;
}

// PERF-V1N: handler 1A68 (1A68..1C18, 56 pairs) -- spherical/hemispherical UV generator (writes .xyz to 796+base+j, preserves .w)
static bool handler1A68(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 236u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 796u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), half = _mm_set1_ps(0.5f);
    const __m128 cmd0 = ldq(vu, vi13);
    const __m128 cmd1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    vi13 = static_cast<uint16_t>(vi13 + 2u);
    const __m128 m982 = ldq(vu, 982), m983 = ldq(vu, 983), m984 = ldq(vu, 984), v985 = ldq(vu, 985);
    const __m128 L = fmacV<2>(v985, one, fmacV<2>(m984, bc<2>(cmd0), fmacV<2>(m983, bc<1>(cmd0), fmacV<4>(m982, bc<0>(cmd0), zero))));
    const __m128 m989 = ldq(vu, 989), m990 = ldq(vu, 990), m991 = ldq(vu, 991);
    const __m128 V = fmacV<2>(m991, bc<2>(cmd1), fmacV<2>(m990, bc<1>(cmd1), fmacV<4>(m989, bc<0>(cmd1), zero)));
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 dSq = fmacV<4>(dVec, dVec, zero);
        const __m128 d2 = fmacV<2>(one, bc<2>(dSq), fmacV<2>(one, bc<1>(dSq), fmacV<4>(one, bc<0>(dSq), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(d2)));
        const __m128 dir = fmacV<4>(dVec, qVec, zero);
        const __m128 vProd = fmacV<4>(V, Nrm, zero);
        const __m128 dProd = fmacV<4>(dir, Nrm, zero);
        const __m128 uDot = fmacV<0>(fmacV<0>(vProd, bc<1>(vProd), zero), bc<2>(vProd), zero);
        const __m128 vDot = fmacV<0>(fmacV<0>(dProd, bc<1>(dProd), zero), bc<2>(dProd), zero);
        const __m128 dots = _mm_unpacklo_ps(uDot, vDot); // lane 0 = uDot, lane 1 = vDot
        const __m128 uv = fmacV<2>(dots, half, half);
        const __m128 uv1 = _mm_movelh_ps(uv, _mm_set_ss(1.0f)); // (u, v, 1.0f, 0.0f)
        const __m128 oldQ = ldq(vu, static_cast<uint32_t>(outBase) + j);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, _mm_blend_ps(uv1, oldQ, 0x8));
    }
    R.I = 0.5f; R.iValid = true; // PERF-V1Q: 1B68 I=0.5f
    R.vfValid &= ~0x000007FEu; // PERF-V1P: 1A68 touches vf1..10, vi1..2
    R.viValid &= ~0x0006u;
    return true;
}

// PERF-V1O: handler 0198 (0198..02D8, 41 pairs) -- 4-directional diffuse light (4x4 color matrix * max(dot, 0))
static bool handler0198(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 156u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 716u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps();
    const __m128 d0 = ldq(vu, vi13);
    const __m128 d1 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    const __m128 d2 = ldq(vu, static_cast<uint32_t>(vi13) + 2u);
    const __m128 c0 = ldq(vu, static_cast<uint32_t>(vi13) + 3u);
    const __m128 c1 = ldq(vu, static_cast<uint32_t>(vi13) + 4u);
    const __m128 c2 = ldq(vu, static_cast<uint32_t>(vi13) + 5u);
    const __m128 c3 = ldq(vu, static_cast<uint32_t>(vi13) + 6u);
    vi13 = static_cast<uint16_t>(vi13 + 7u);
    if (scene::on()) // FEAT-V2
        scene::recordLight4(0x0198u, d0, d1, d2, c0, c1, c2, c3, zero); // FEAT-V2
    const __m128 m986 = ldq(vu, 986), m987 = ldq(vu, 987), m988 = ldq(vu, 988);
    const __m128 Lx = fmacV<2>(d2, bc<2>(m986), fmacV<2>(d1, bc<1>(m986), fmacV<4>(d0, bc<0>(m986), zero)));
    const __m128 Ly = fmacV<2>(d2, bc<2>(m987), fmacV<2>(d1, bc<1>(m987), fmacV<4>(d0, bc<0>(m987), zero)));
    const __m128 Lz = fmacV<2>(d2, bc<2>(m988), fmacV<2>(d1, bc<1>(m988), fmacV<4>(d0, bc<0>(m988), zero)));
    logRange(vu, outBase, n);
    __m128 lastCol = zero; // PERF-V1Q: 02A8 ADD vf14, vf13, vf10
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 Col = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 dot = fmacV<2>(Lz, bc<2>(Nrm), fmacV<2>(Ly, bc<1>(Nrm), fmacV<4>(Lx, bc<0>(Nrm), zero)));
        const __m128 diff = maxV(dot, zero);
        const __m128 contrib = fmacV<2>(c3, bc<3>(diff), fmacV<2>(c2, bc<2>(diff), fmacV<2>(c1, bc<1>(diff), fmacV<4>(c0, bc<0>(diff), zero))));
        lastCol = fmacV<0>(Col, contrib, zero);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, lastCol);
    }
    // PERF-V1P: 0290..02B0 leave vf11 = N[n+1], vf12 = L*N[n], vf13 = Col[n]
    // (loop exit: vf11 = N[n+2] (prologue loads N0..N2, loop loads N3..N(n+2)); vf12 = L*N[n+1] (iteration i reads vf11 = N(i+1)))
    const __m128 nrmN = ldq(vu, static_cast<uint32_t>(base) + 556u + n + 1u);
    R.vf[11] = ldq(vu, static_cast<uint32_t>(base) + 556u + n + 2u);
    R.vf[12] = fmacV<2>(Lz, bc<2>(nrmN), fmacV<2>(Ly, bc<1>(nrmN), fmacV<4>(Lx, bc<0>(nrmN), zero)));
    R.vf[13] = ldq(vu, static_cast<uint32_t>(outBase) + n);
    R.vf[14] = lastCol; // PERF-V1Q
    R.vfValid = (R.vfValid & ~0x000047FEu) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14);
    R.viValid &= ~0x000Eu;
    return true;
}

// PERF-V1O: handler 0B38 (0B38..0C30, 32 pairs) -- triangle strip backface cull (sets bit 15 ADC flag on index words)
static bool handler0B38(Regs &R, uint8_t *vu, uint16_t top)
{
    const uint16_t vi1_0 = static_cast<uint16_t>(ilw(vu, 1018, 2) + top);
    const uint16_t n = ilw(vu, 1018, 3);
    if (n == 0u || n > 1024u)
        return false;
    const __m128 zero = _mm_setzero_ps();
    const __m128 vf1y = bc<1>(ldq(vu, top)); // 0B58 LQ.y vf1, -1(vi13)
    const __m128 vf11 = _mm_set1_ps(1.0f);
    __m128 vf4 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    uint16_t vi6 = ilw(vu, vi1_0, 0);
    const uint16_t vi5_0 = ilw(vu, static_cast<uint16_t>(vi1_0 + 1u), 0);
    __m128 vf12 = ldq(vu, 636u + vi6);
    vf12 = _mm_blend_ps(vf12, fmacV<0>(zero, bc<3>(vf12), zero), 0x4);
    __m128 vf2 = ldq(vu, 636u + vi5_0);
    vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
    __m128 vf3 = norm4(vf12);
    uint16_t vi7 = ilw(vu, static_cast<uint16_t>(vi1_0 + 2u), 0);
    uint16_t vi8 = ilw(vu, vi1_0, 1);
    uint32_t sx = 0u; // 0BB0 MUL.x vf0, vf0, vf0
    logRange(vu, vi1_0, n);
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 vf5 = norm4(vf3);
        vf3 = norm4(vf2);
        const uint16_t vi9 = static_cast<uint16_t>((sx ? 0x80u : 0u) + 32767u);
        vf2 = ldq(vu, 636u + vi7);
        vi8 = static_cast<uint16_t>((vi8 + vi9) & 0x8000u);
        vi6 = static_cast<uint16_t>(vi6 | vi8);
        const __m128 p5 = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 p3 = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 1, 0, 2));
        const __m128 q3 = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 q5 = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 1, 0, 2));
        const __m128 cross = fmacV<3>(q3, q5, fmacV<4>(p5, p3, zero));
        const __m128 vf5c = _mm_blend_ps(vf5, cross, 0x7);
        vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
        const uint32_t qd = static_cast<uint16_t>(vi1_0 + k) & 0x3FFu;
        stqiRaw(vu, qd, _mm_insert_epi32(ldqi(vu, qd), static_cast<int>(vi6), 0));
        vi6 = ilw(vu, static_cast<uint16_t>(vi1_0 + k + 1u), 0);
        const __m128 vf4x = fmacV<2>(vf11, bc<2>(vf4), fmacV<0>(vf4, bc<1>(vf4), zero));
        const __m128 mul0 = fmacV<4>(vf4x, vf1y, zero);
        sx = static_cast<uint32_t>(_mm_movemask_ps(mul0) & 1);
        vi7 = ilw(vu, static_cast<uint16_t>(vi1_0 + k + 3u), 0);
        vf4 = fmacV<4>(vf5c, vf2, zero);
        vi8 = ilw(vu, static_cast<uint16_t>(vi1_0 + k + 1u), 1);
    }
    R.vfValid &= ~0x0002183Eu; // PERF-V1P: 0B38 touches vf1..5, vf11, vf12, vf17, vi1..9
    R.viValid &= ~0x03FEu;
    return true;
}

// PERF-V1P: handler 1910 (1910..1A40, 39 pairs) -- position transform by 978..981 * 0..3(vi2), reads live-in vf9 (from 1908)
static bool handler1910(Regs &R, uint8_t *vu, uint16_t &vi13, uint16_t vi2)
{
    if ((R.vfValid & (1u << 9)) == 0u)
        return false;
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 512u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 796u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 T0 = R.vf[9];
    const __m128 T1 = ldq(vu, static_cast<uint32_t>(vi2) + 1u);
    const __m128 T2 = ldq(vu, static_cast<uint32_t>(vi2) + 2u);
    const __m128 T3 = ldq(vu, static_cast<uint32_t>(vi2) + 3u);
    const auto mulMat = [&](__m128 B) {
        return fmacV<2>(T3, bc<3>(B), fmacV<2>(T2, bc<2>(B), fmacV<2>(T1, bc<1>(B), fmacV<4>(T0, bc<0>(B), zero))));
    };
    const __m128 M0 = mulMat(ldq(vu, 978));
    const __m128 M1 = mulMat(ldq(vu, 979));
    const __m128 M2 = mulMat(ldq(vu, 980));
    const __m128 M3 = mulMat(ldq(vu, 981));
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + 476u + j);
        const __m128 res = fmacV<2>(M3, one, fmacV<2>(M2, bc<2>(P), fmacV<2>(M1, bc<1>(P), fmacV<4>(M0, bc<0>(P), zero))));
        const uint32_t qd = static_cast<uint32_t>(outBase) + j;
        stqRaw(vu, qd, _mm_blend_ps(ldq(vu, qd), res, 0x7));
    }
    R.vi[4] = 476u; // 1958 IADDIU vi4, vi0, 476
    R.vfValid &= ~0x00101C3Eu;
    R.viValid = (R.viValid & ~0x080Eu) | (1u << 4);
    return true;
}

// PERF-V1P: shared projective UV transform for 1CD8 (positions, 978..981) and 1CC0 (normals, 986..988 + vf0)
static bool handler1CC0_1CD8(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t vi2, bool is1CC0)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 236u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 796u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 T0 = ldq(vu, vi2), T1 = ldq(vu, static_cast<uint32_t>(vi2) + 1u);
    const __m128 T2 = ldq(vu, static_cast<uint32_t>(vi2) + 2u), T3 = ldq(vu, static_cast<uint32_t>(vi2) + 3u);
    const auto mulMat = [&](__m128 B) {
        return fmacV<2>(T3, bc<3>(B), fmacV<2>(T2, bc<2>(B), fmacV<2>(T1, bc<1>(B), fmacV<4>(T0, bc<0>(B), zero))));
    };
    const uint32_t mBase = is1CC0 ? 986u : 978u;
    const __m128 M0 = mulMat(ldq(vu, mBase)), M1 = mulMat(ldq(vu, mBase + 1u));
    const __m128 M2 = mulMat(ldq(vu, mBase + 2u)), M3 = mulMat(is1CC0 ? _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f) : ldq(vu, 981));
    const uint32_t srcOff = is1CC0 ? 556u : 476u;
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(base) + srcOff + j);
        const __m128 V = fmacV<2>(M3, one, fmacV<2>(M2, bc<2>(P), fmacV<2>(M1, bc<1>(P), fmacV<4>(M0, bc<0>(P), zero))));
        const __m128 oldUV = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 acc = _mm_blend_ps(fmacV<4>(oldUV, bc<2>(V), zero), zero, 0x4); // ADDA.z 0 + MULAz.xy
        const __m128 res = fmacV<2>(V, bc<2>(oldUV), acc);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, _mm_blend_ps(oldUV, res, 0x7));
    }
    R.vi[4] = static_cast<uint16_t>(outBase + n);
    R.vfValid &= ~0x000001FEu;
    R.viValid = (R.viValid & ~0x006Eu) | (1u << 4);
    return true;
}

// PERF-V1O: handler 1CD8 (1CD8..1E38, 45 pairs) -- projective UV transform (writes .xyz to 796+base+j, preserves .w)
static bool handler1CD8(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t vi2)
{
    return handler1CC0_1CD8(R, vu, vi13, vi2, false);
}

// PERF-V1P: handler 1CC0 (1CC0..1CD0 -> 1CF0..1E38) -- projective UV transform on normals (556+base+j, 986..988 + vf0)
static bool handler1CC0(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t vi2)
{
    return handler1CC0_1CD8(R, vu, vi13, vi2, true);
}

// PERF-V1O: handler 32A0 (32A0..3450, 44 pairs) -- silhouette shadow-volume extrusion (updates .xyz at 476+base+j, preserves .w)
static bool handler32A0(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 236u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 476u);
    if (((vi13 - outBase) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 cmd0 = ldq(vu, vi13);
    vi13 = static_cast<uint16_t>(vi13 + 1u);
    const __m128 m989 = ldq(vu, 989), m990 = ldq(vu, 990), m991 = ldq(vu, 991), v985 = ldq(vu, 985);
    const __m128 L = fmacV<2>(v985, one, fmacV<2>(m991, bc<2>(cmd0), fmacV<2>(m990, bc<1>(cmd0), fmacV<4>(m989, bc<0>(cmd0), zero))));
    logRange(vu, outBase, n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 dVec = fmacV<1>(L, P, zero);
        const __m128 d2Vec = fmacV<4>(dVec, dVec, zero);
        const __m128 d2 = fmacV<2>(one, bc<2>(d2Vec), fmacV<2>(one, bc<1>(d2Vec), fmacV<4>(one, bc<0>(d2Vec), zero)));
        const __m128 qVec = _mm_set1_ps(rsqrtQ(1.0f, _mm_cvtss_f32(d2)));
        const __m128 dir = fmacV<4>(dVec, qVec, zero);
        const __m128 prod = fmacV<4>(Nrm, dVec, zero);
        const __m128 dot = fmacV<0>(fmacV<0>(prod, bc<1>(prod), zero), bc<2>(prod), zero);
        const uint32_t dotBits = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(dot)));
        const uint32_t vi2_f = ((dotBits & 0x80000000u) ? 0x80u : 0u) | (((dotBits & 0x7FFFFFFFu) == 0u) ? 0x08u : 0u);
        const __m128 scale = fmacV<4>(_mm_set1_ps(static_cast<float>(vi2_f) * 0.0625f), bc<3>(cmd0), zero);
        const __m128 acc = fmacV<0>(P, zero, zero);
        const __m128 res = fmacV<2>(dir, scale, acc);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, _mm_blend_ps(P, res, 0x7));
    }
    R.vi[4] = 136u; // PERF-V1P: 3330 IADDIU vi4, vi0, 136
    R.vfValid &= ~0x000007FEu;
    R.viValid = (R.viValid & ~0x000Eu) | (1u << 4);
    return true;
}

// PERF-V1P: handler 0560 (0560..0620, 25 pairs) -- linear scale+bias on 1018.y+top..+1018.w, preceded by live-in stores SQ vf15..18 -> 18/20/22/24(vi4)
static bool handler0560(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    if ((R.viValid & (1u << 4)) == 0u || (R.vfValid & 0x00078000u) != 0x00078000u)
        return false;
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 18u, R.vf[15]);
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 20u, R.vf[16]);
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 22u, R.vf[17]);
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 24u, R.vf[18]);
    const uint16_t off = ilw(vu, 1018, 1), count = ilw(vu, 1018, 3);
    if (count < 2u || count > 512u)
        return false;
    const uint16_t base = static_cast<uint16_t>(off + R.vi[12]);
    const __m128 zero = _mm_setzero_ps();
    const __m128 vf1 = ldq(vu, vi13);
    const __m128 vf3 = ldq(vu, static_cast<uint32_t>(vi13) + 1u);
    vi13 = static_cast<uint16_t>(vi13 + 2u);
    if (((vi13 - base) & 0x3FFu) < count)
        return false;
    const __m128 acc = fmacV<0>(vf3, zero, zero);
    logRange(vu, base, count);
    for (uint32_t k = 0; k < count; ++k)
        stqRaw(vu, static_cast<uint32_t>(base) + k, fmacV<2>(ldq(vu, static_cast<uint32_t>(base) + k), vf1, acc));
    R.I = 0.5f; R.iValid = true; // PERF-V1Q: 05A0 I=0.5f
    R.vfValid &= ~0x0000787Eu;
    R.viValid &= ~0x000Eu;
    return true;
}

// PERF-V1P: handler 0AE0 (0AE0..0B30, 11 pairs) -- zero-fill 716+base..716+base+n (n+1 qwords; entry before 0AF0)
static bool handler0AE0(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t vi3 = ilw(vu, 1011, 0), vi2 = ilw(vu, 1011, 3);
    const uint32_t count = static_cast<uint32_t>(vi2) + 1u;
    if (count > 512u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(vi3 + 716u);
    if (((vi13 - outBase) & 0x3FFu) < count)
        return false;
    const __m128 zero = _mm_setzero_ps();
    logRange(vu, outBase, count);
    for (uint32_t i = 0; i < count; ++i)
        stqRaw(vu, static_cast<uint32_t>(outBase) + i, zero);
    R.vf[1] = zero;
    R.vi[2] = vi2;
    R.vi[3] = vi3;
    R.vi[4] = static_cast<uint16_t>(outBase + vi2);
    R.vfValid = (R.vfValid & ~0x00008002u) | (1u << 1);
    R.viValid = (R.viValid & ~0x001Eu) | (1u << 2) | (1u << 3) | (1u << 4);
    return true;
}

// PERF-V1Q: handler 0C38 (0C38..0CB8, 17 pairs) -- clamps/converts live-in vf11..14 with I, stores vf11/12 -> 9/11(vi4), then 0C80 copy loop
static bool handler0C38(Regs &R, uint8_t *vu, uint16_t &vi13, uint16_t vi2)
{
    if (!R.iValid || (R.viValid & (1u << 4)) == 0u || (R.vfValid & 0x00007800u) != 0x00007800u)
        return false;
    uint16_t vi3 = ilw(vu, static_cast<uint32_t>(vi13) - 1u, 2);
    const uint32_t count = static_cast<uint16_t>(vi2 - vi3);
    if (count == 0u || count > 1024u)
        return false;
    const __m128 iv = _mm_set1_ps(R.I);
    const __m128 vf11 = _mm_castsi128_ps(ftoiS<1>(R.vf[11]));
    const __m128 vf12 = _mm_castsi128_ps(ftoiS<1>(miniV(R.vf[12], iv)));
    const __m128 vf13 = _mm_castsi128_ps(ftoiS<1>(miniV(R.vf[13], iv)));
    const __m128 vf14 = _mm_castsi128_ps(ftoiS<1>(miniV(R.vf[14], iv)));
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 9u, vf11);
    stq(vu, static_cast<uint32_t>(R.vi[4]) + 11u, vf12);
    __m128 vf1 = _mm_setzero_ps();
    logRange(vu, vi3, count);
    do
    {
        vf1 = ldq(vu, vi13);
        vi13 = static_cast<uint16_t>(vi13 + 1u);
        vi3 = static_cast<uint16_t>(vi3 + 1u);
        stqRaw(vu, static_cast<uint32_t>(vi3) - 1u, vf1);
    } while (vi3 != vi2);
    R.vf[1] = vf1;
    R.vf[11] = vf11;
    R.vf[12] = vf12;
    R.vf[13] = vf13;
    R.vf[14] = vf14;
    R.vi[3] = vi3;
    R.vfValid = (R.vfValid & ~0x0000001Eu) | (1u << 1);
    R.viValid |= (1u << 3);
    return true;
}

// PERF-V1P: handler 1E50 (1E50..1EC8, 16 pairs) -- normal-extrude positions: P[j].xyz += N[j].xyz * cmd.yzw for j in [0..n]
static bool handler1E50(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t base = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 256u)
        return false;
    const uint16_t outBase = static_cast<uint16_t>(base + 476u);
    const uint32_t count = static_cast<uint32_t>(n) + 1u;
    if (((vi13 - outBase) & 0x3FFu) < count)
        return false;
    const __m128 zero = _mm_setzero_ps();
    const __m128 cmd = norm4(ldq(vu, vi13)); // 1E50 LQ.yzw vf2, 0(vi13); 1E60..1E70 ADDy/z/w vf1, vf0, vf2
    const __m128 scale = _mm_shuffle_ps(cmd, cmd, _MM_SHUFFLE(3, 3, 2, 1));
    logRange(vu, outBase, count);
    for (uint32_t j = 0; j < count; ++j)
    {
        const __m128 P = ldq(vu, static_cast<uint32_t>(outBase) + j);
        const __m128 Nrm = ldq(vu, static_cast<uint32_t>(base) + 556u + j);
        const __m128 acc = fmacV<0>(P, zero, zero);
        const __m128 res = fmacV<2>(Nrm, scale, acc);
        stqRaw(vu, static_cast<uint32_t>(outBase) + j, _mm_blend_ps(P, res, 0x7));
    }
    R.vfValid &= ~0x0000001Eu;
    R.viValid &= ~0x0006u;
    return true;
}

// PERF-V1Q: handler 2E20 (2E20..2E80, 13 pairs) -- internal clipPolygon vertex-emit continuation (covered by clipPolygon in handler1EE0); never entered as a command
static bool handler2E20(Regs &R, uint8_t *vu, uint16_t vi13)
{
    (void)R;
    (void)vu;
    (void)vi13;
    return false;
}

// PERF-V1Q: handler 2E88 (2E88..2F68, 29 pairs) -- internal clipPolygon end-of-plane / edge-clip subroutine (covered by clipPolygon in handler1EE0); never entered as a command
static bool handler2E88(Regs &R, uint8_t *vu, uint16_t vi13)
{
    (void)R;
    (void)vu;
    (void)vi13;
    return false;
}

// PERF-VNC: new-handler usage tracking (reset per program in vu1FuncRunInner)
static thread_local uint32_t t_newN = 0;
static thread_local uint16_t t_newPcs[32];
static inline void vncNote(uint16_t pc)
{
    for (uint32_t i = 0; i < t_newN; ++i)
        if (t_newPcs[i] == pc)
            return;
    if (t_newN < 32u)
        t_newPcs[t_newN++] = pc;
}

static thread_local uint16_t t_seq[64]; // PROBE-BG
static thread_local uint32_t t_seqN = 0; // PROBE-BG
inline uint32_t traceSeqLen() { return t_seqN; } // PROBE-BG
inline const uint16_t *traceSeq() { return t_seq; } // PROBE-BG

static bool vu1FuncRunInner(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif, bool oneShot, bool vxA)
{
    static const bool funcNewOn = []() { const char *e = std::getenv("PS2X_VU1_FUNC_NEW"); return !(e && e[0] == '0'); }(); // PERF-V1M: default ON (PERF-VNC); PS2X_VU1_FUNC_NEW=0 disables
    t_newN = 0; // PERF-VNC
    t_seqN = 0; // PROBE-BG
    if (watch::on()) // PROBE-M5
        watch::t_bones.reset();
    Regs R; // PERF-V1G: skip 560-byte memset; vfValid/viValid track exact live-in registers across handlers
    R.vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    R.vi[0] = 0u;
    R.vi[12] = static_cast<uint16_t>(top);
    R.vfValid = 1u;
    R.viValid = 1u | (1u << 12);
    uint16_t vi13 = R.vi[12];
    uint16_t vi1 = ilw(vu, vi13, 0);
    const uint64_t imgMask = (t_curSlot >= 0) ? s_imgCache[t_curSlot].mask : ~0ull; // PERF-BE
    const uint8_t *const rowIdx = pcToRow(); // PERF-BE
    vx::HScope vxHs; // PROBE-VX
    hprof::HScope hpHs; // PROBE-RD39
    const bool hpOn = hprof::on(); // PROBE-RD39
    watch::t_m8Mscal = static_cast<uint16_t>(top); // PROBE-M8
    watch::t_m8Handler = 0u; // PROBE-M8
    uint16_t m8LastWork = 0u; // PROBE-M8
    for (uint32_t step = 0; step < 100000u; ++step)
    {
        vi13 = static_cast<uint16_t>(vi13 + 1u);                      // 0028 IADDIU vi13, vi13, 1
        const uint16_t vi2 = ilw(vu, static_cast<uint32_t>(vi13) - 1u, 1); // 0038 ILW.y vi2, -1(vi13)
        const uint32_t h = (static_cast<uint32_t>(vi1) << 3) & 0x3FFFu;
        if (h != 0u) // PERF-BE
        { // PERF-BE
            const uint8_t row = rowIdx[h >> 3]; // PERF-BE
            if (row != 0xFFu && ((imgMask >> row) & 1ull) == 0ull) // PERF-BE
            { // PERF-BE
                s_fbPc = 0x4000u | h; // PERF-BE
                return false; // PERF-BE
            } // PERF-BE
        } // PERF-BE
        if (vxA) { if (h) vxHs.begin(vx::slotOf(h, vu), __rdtsc()); else vxHs.end(__rdtsc()); } // PROBE-VX
        if (hpOn) { if (h) { uint16_t hpc = static_cast<uint16_t>(h); if ((h == 0x1EE0u || h == 0x1EF0u) && static_cast<int16_t>(ilw(vu, 1011, 1)) < 0) hpc = 0x2440u; hpHs.begin(hpc); } else hpHs.end(); } // PROBE-RD39
        if (t_seqN < 64u) t_seq[t_seqN++] = static_cast<uint16_t>(h); // PROBE-BG
        if (watch::spikeOn()) // PROBE-M8
        {
            if (h != 0u && h != 0x0050u && h != 0x0060u && h != 0x0070u && h != 0x0088u)
                m8LastWork = static_cast<uint16_t>(h);
            watch::t_m8Handler = (h == 0x0050u || h == 0x0088u)
                                     ? (m8LastWork ? m8LastWork : static_cast<uint16_t>(h))
                                     : static_cast<uint16_t>(h);
        }
        switch (h)
        {
        case 0x0000u: // program end (E bit)
            return true;
        case 0x0050u: // XGKICK 1023
            if (!kick(vu, 1023u, kicks, gif))
                return false;
            break;
        case 0x0060u: // JR vi15 only
            break;
        case 0x0070u: // vi2 -= vi12; vi13 = vi2 + vi12 (stream jump)
            vi13 = vi2;
            break;
        case 0x0088u: // XGKICK vi13; vi13 += vi2
            if (!kick(vu, vi13, kicks, gif))
                return false;
            vi13 = static_cast<uint16_t>(vi13 + vi2);
            break;
        case 0x0C80u: // copy loop: vi3 = z of the header; do { vf1 = LQI(vi13++); SQ vf1 -> (vi3++) } while (vi3 != vi2)
        {
            uint16_t vi3 = ilw(vu, static_cast<uint32_t>(vi13) - 1u, 2);
            const uint32_t count = static_cast<uint16_t>(vi2 - vi3);
            if (count == 0u || count > 1024u)
            {
                s_fbPc = 0xF020u;
                return false;
            }
            __m128 vf1 = _mm_setzero_ps();
            logRange(vu, vi3, count); // PERF-V1G
            do
            {
                vf1 = ldq(vu, vi13);
                vi13 = static_cast<uint16_t>(vi13 + 1u);
                vi3 = static_cast<uint16_t>(vi3 + 1u);
                stqRaw(vu, static_cast<uint32_t>(vi3) - 1u, vf1);
            } while (vi3 != vi2);
            R.vf[1] = vf1;
            R.vi[3] = vi3;
            R.vfValid |= (1u << 1); // PERF-V1P
            R.viValid |= (1u << 3);
            break;
        }
        case 0x0AF0u:
            if (!handler0AF0(R, vu, vi13))
            {
                s_fbPc = 0x0AF0u;
                return false;
            }
            break;
        case 0x0B40u:
            if (!handler0B40(R, vu, vi13))
            {
                s_fbPc = 0x0B40u;
                return false;
            }
            break;
        case 0x15E0u:
            if (!handler15E0(R, vu, vi13))
            {
                s_fbPc = 0x15E0u;
                return false;
            }
            break;
        case 0x1900u:
            if (!handler1900(R, vu, vi13, vi2))
            {
                s_fbPc = 0x1900u;
                return false;
            }
            break;
        case 0x1C48u:
            if (!handler1C48(R, vu, vi13))
            {
                s_fbPc = 0x1C48u;
                return false;
            }
            break;
        case 0x1EE0u: // Task F2: mesh transform (vi10 = 0 entry)
            if (!handler1EE0(R, vu, vi13, static_cast<uint16_t>(top), false, kicks, gif))
            {
                s_fbPc = 0x1EE0u;
                return false;
            }
            R.vfValid = 1u; // PERF-V1P
            R.viValid = 1u | (1u << 12);
            R.I = 255.0f; R.iValid = true; // PERF-V1Q: 2278/24E0/2708 I=255.0f
            break;
        case 0x1EF0u: // Task F3: mesh transform (vi10 = 1 entry, reads S from 796+idx)
            if (!handler1EE0(R, vu, vi13, static_cast<uint16_t>(top), true, kicks, gif))
            {
                s_fbPc = 0x1EF0u;
                return false;
            }
            R.vfValid = 1u; // PERF-V1P
            R.viValid = 1u | (1u << 12);
            R.I = 255.0f; R.iValid = true; // PERF-V1Q: 2278/24E0/2708 I=255.0f
            break;
        case 0x3160u:
            if (!handler3160(R, vu, vi13))
            {
                s_fbPc = 0x3160u;
                return false;
            }
            break;
        case 0x00A8u:
            if (!handler00A8(R, vu))
            {
                s_fbPc = 0x00A8u;
                return false;
            }
            R.vfValid &= ~0x8200017Eu; // PERF-V1P: 00A8 touches vf1..6, vf8, vf25, vf31, vi1..3, vi5..7
            R.viValid &= ~0x00EEu;
            break;
        case 0x0840u: // PERF-V1M
            vncNote(0x0840u); // PERF-VNC
            if (!funcNewOn || !handler0840(R, vu, vi13))
            {
                s_fbPc = 0x0840u;
                return false;
            }
            break;
        case 0x0CC0u: // PERF-V1M
            vncNote(0x0CC0u); // PERF-VNC
            if (!funcNewOn || !handler0CC0(R, vu, vi13))
            {
                s_fbPc = 0x0CC0u;
                return false;
            }
            break;
        case 0x1090u: // PERF-V1M
            vncNote(0x1090u); // PERF-VNC
            if (!funcNewOn || !handler1090(R, vu, vi13))
            {
                s_fbPc = 0x1090u;
                return false;
            }
            break;
        case 0x02E0u: // PERF-V1N
            vncNote(0x02E0u); // PERF-VNC
            if (!funcNewOn || !handler02E0(R, vu, vi13))
            {
                s_fbPc = 0x02E0u;
                return false;
            }
            break;
        case 0x0628u: // PERF-V1N
            vncNote(0x0628u); // PERF-VNC
            if (!funcNewOn || !handler0628(R, vu, vi13, true)) // PERF-BI
            {
                s_fbPc = 0x0628u;
                return false;
            }
            break;
        case 0x0640u: // PERF-BI
            vncNote(0x0640u); // PERF-BI
            if (!funcNewOn || !handler0628(R, vu, vi13, false)) // PERF-BI
            { // PERF-BI
                s_fbPc = 0x0640u; // PERF-BI
                return false; // PERF-BI
            } // PERF-BI
            break; // PERF-BI
        case 0x1720u: // PERF-V1N
            vncNote(0x1720u); // PERF-VNC
            if (!funcNewOn || !handler1720(R, vu, vi13))
            {
                s_fbPc = 0x1720u;
                return false;
            }
            break;
        case 0x1A68u: // PERF-V1N
            vncNote(0x1A68u); // PERF-VNC
            if (!funcNewOn || !handler1A68(R, vu, vi13))
            {
                s_fbPc = 0x1A68u;
                return false;
            }
            break;
        case 0x0198u: // PERF-V1O
            vncNote(0x0198u); // PERF-VNC
            if (!funcNewOn || !handler0198(R, vu, vi13))
            {
                s_fbPc = 0x0198u;
                return false;
            }
            R.vfValid &= ~((1u << 17) | (1u << 18)); // PERF-BF
            R.viValid &= ~(1u << 6); // PERF-BF
            break;
        case 0x01C8u: // PERF-BF
            vncNote(0x01C8u); // PERF-BF
            if (!funcNewOn || !handler0198(R, vu, vi13)) // PERF-BF
            { // PERF-BF
                s_fbPc = 0x01C8u; // PERF-BF
                return false; // PERF-BF
            } // PERF-BF
            break; // PERF-BF
        case 0x0B38u: // PERF-V1O
            vncNote(0x0B38u); // PERF-VNC
            if (!funcNewOn || !handler0B38(R, vu, R.vi[12]))
            {
                s_fbPc = 0x0B38u;
                return false;
            }
            break;
        case 0x1910u: // PERF-V1O
            vncNote(0x1910u); // PERF-VNC
            if (!funcNewOn || !handler1910(R, vu, vi13, vi2))
            {
                s_fbPc = 0x1910u;
                return false;
            }
            break;
        case 0x1CD8u: // PERF-V1O
            vncNote(0x1CD8u); // PERF-VNC
            if (!funcNewOn || !handler1CD8(R, vu, vi13, vi2))
            {
                s_fbPc = 0x1CD8u;
                return false;
            }
            break;
        case 0x32A0u: // PERF-V1O
            vncNote(0x32A0u); // PERF-VNC
            if (!funcNewOn || !handler32A0(R, vu, vi13))
            {
                s_fbPc = 0x32A0u;
                return false;
            }
            break;
        case 0x0560u: // PERF-V1P
            vncNote(0x0560u); // PERF-VNC
            if (!funcNewOn || !handler0560(R, vu, vi13))
            {
                s_fbPc = 0x0560u;
                return false;
            }
            break;
        case 0x0AE0u: // PERF-V1P
            vncNote(0x0AE0u); // PERF-VNC
            if (!funcNewOn || !handler0AE0(R, vu, vi13))
            {
                s_fbPc = 0x0AE0u;
                return false;
            }
            break;
        case 0x0C38u: // PERF-V1P
            vncNote(0x0C38u); // PERF-VNC
            if (!funcNewOn || !handler0C38(R, vu, vi13, vi2))
            {
                s_fbPc = 0x0C38u;
                return false;
            }
            break;
        case 0x1CC0u: // PERF-V1P
            vncNote(0x1CC0u); // PERF-VNC
            if (!funcNewOn || !handler1CC0(R, vu, vi13, vi2))
            {
                s_fbPc = 0x1CC0u;
                return false;
            }
            break;
        case 0x1E50u: // PERF-V1P
            vncNote(0x1E50u); // PERF-VNC
            if (!funcNewOn || !handler1E50(R, vu, vi13))
            {
                s_fbPc = 0x1E50u;
                return false;
            }
            break;
        case 0x2E20u: // PERF-V1P
            vncNote(0x2E20u); // PERF-VNC
            if (!funcNewOn || !handler2E20(R, vu, vi13))
            {
                s_fbPc = 0x2E20u;
                return false;
            }
            break;
        case 0x2E88u: // PERF-V1P
            vncNote(0x2E88u); // PERF-VNC
            if (!funcNewOn || !handler2E88(R, vu, vi13))
            {
                s_fbPc = 0x2E88u;
                return false;
            }
            break;
        default:
            s_fbPc = h;
            return false;
        }
        if (oneShot) // startPC 0040: vi15 = 0, so the handler's JR vi15 lands on 0000 (E bit) -> program ends after one handler
            return true;
        vi1 = ilw(vu, vi13, 0); // every handler ends with ILW.x vi1, 0(vi13)
    }
    s_fbPc = 0xF030u; // step budget
    return false;
}

// PERF-BK: decompiled full-screen/particle VU1 programs of image 40204101 (P0=0000, P1=04A0, P2=0A18)
template <int MASK> // PERF-BK
inline __m128 fxBlend(__m128 oldV, __m128 newV) { return _mm_blend_ps(oldV, newV, MASK); } // PERF-BK
inline __m128 fxItof0(__m128 v) { return _mm_cvtepi32_ps(_mm_castps_si128(v)); } // PERF-BK
inline __m128 fxMfirW(__m128 v, uint16_t vi) // PERF-BK
{ // PERF-BK
    const int32_t sv = static_cast<int32_t>(static_cast<int16_t>(vi)); // PERF-BK
    return _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(v), sv, 3)); // PERF-BK
} // PERF-BK
inline uint32_t fxRinit(__m128 v, int lane) // PERF-BK
{ // PERF-BK
    alignas(16) uint32_t w[4]; // PERF-BK
    _mm_store_si128(reinterpret_cast<__m128i *>(w), _mm_castps_si128(v)); // PERF-BK
    return 0x3F800000u | (w[lane] & 0x007FFFFFu); // PERF-BK
} // PERF-BK
inline __m128 fxRnext(uint32_t &r, __m128 oldV, int lane) // PERF-BK
{ // PERF-BK
    const uint32_t x = (r >> 4) & 1u, y = (r >> 22) & 1u; // PERF-BK
    r = (((r << 1) ^ x ^ y) & 0x007FFFFFu) | 0x3F800000u; // PERF-BK
    float f; // PERF-BK
    std::memcpy(&f, &r, 4); // PERF-BK
    const __m128 fv = _mm_set1_ps(f); // PERF-BK
    if (lane == 0) return _mm_blend_ps(oldV, fv, 0x1); // PERF-BK
    if (lane == 1) return _mm_blend_ps(oldV, fv, 0x2); // PERF-BK
    return _mm_blend_ps(oldV, fv, 0x4); // PERF-BK
} // PERF-BK
inline void fxRand3(uint32_t &r, __m128 seed, __m128 &dst) // PERF-BK
{ // PERF-BK
    r = fxRinit(seed, 0); dst = fxRnext(r, dst, 0); // PERF-BK
    r = fxRinit(seed, 1); dst = fxRnext(r, dst, 1); // PERF-BK
    r = fxRinit(seed, 2); dst = fxRnext(r, dst, 2); // PERF-BK
} // PERF-BK
inline __m128 fxMatMul(__m128 c0, __m128 c1, __m128 c2, __m128 c3, __m128 v, bool wOne) // PERF-BK
{ // PERF-BK
    const __m128 zero = _mm_setzero_ps(); // PERF-BK
    __m128 acc = fmacV<4>(c0, bc<0>(v), zero); // PERF-BK
    acc = fmacV<2>(c1, bc<1>(v), acc); // PERF-BK
    acc = fmacV<2>(c2, bc<2>(v), acc); // PERF-BK
    return fmacV<2>(c3, wOne ? _mm_set1_ps(1.0f) : bc<3>(v), acc); // PERF-BK
} // PERF-BK
inline uint16_t fxFcor6(uint32_t clipFlags) // PERF-BK
{ // PERF-BK
    const bool r = ((clipFlags & 0x041u) == 0x041u) || ((clipFlags & 0x082u) == 0x082u) || ((clipFlags & 0x104u) == 0x104u) || // PERF-BK
                   ((clipFlags & 0x208u) == 0x208u) || ((clipFlags & 0x410u) == 0x410u) || ((clipFlags & 0x820u) == 0x820u); // PERF-BK
    return static_cast<uint16_t>(r ? 32768u : 32767u); // PERF-BK
} // PERF-BK
inline uint16_t fxFcor4Z(uint32_t clipFlags) // PERF-BK
{ // PERF-BK
    const bool r = ((clipFlags & 0x041u) == 0x041u) || ((clipFlags & 0x082u) == 0x082u) || ((clipFlags & 0x104u) == 0x104u) || // PERF-BK
                   ((clipFlags & 0x208u) == 0x208u) || ((clipFlags & 0x0C30u) != 0u); // PERF-BK
    return static_cast<uint16_t>(r ? 32768u : 32767u); // PERF-BK
} // PERF-BK

static bool fxProgram0000(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif) // PERF-BK
{ // PERF-BK
    vncNote(0xF000u); // PERF-BK
    const uint32_t base = top & 0x3FFu; // PERF-BK
    const uint16_t count = ilw(vu, base + 11u, 0u); // PERF-BK FIX-BK2
    if (count > 4096u) // PERF-BK4
    { // PERF-BK3
        s_fbPc = 0xF101u; // PERF-BK3
        return false; // PERF-BK
    } // PERF-BK3
    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f); // PERF-BK
    const __m128 vf10 = ldq(vu, base + 0u), vf11 = ldq(vu, base + 1u), vf12 = ldq(vu, base + 2u), vf13 = ldq(vu, base + 3u); // PERF-BK
    const __m128 vf14 = ldq(vu, base + 4u), vf15 = ldq(vu, base + 5u), vf16 = ldq(vu, base + 6u); // PERF-BK
    __m128 vf17 = ldq(vu, base + 7u), vf18 = ldq(vu, base + 8u); // PERF-BK
    const __m128 vf19 = ldq(vu, base + 9u), vf20 = ldq(vu, base + 10u); // PERF-BK
    __m128 vf21 = ldq(vu, base + 12u); // PERF-BK
    const __m128 vf22 = ldq(vu, base + 13u), vf23 = ldq(vu, base + 14u), vf24 = ldq(vu, base + 15u); // PERF-BK
    __m128 vf25 = ldq(vu, base + 16u); // PERF-BK
    const __m128 vf26 = ldq(vu, base + 17u); // PERF-BK
    uint32_t r = 0x3F800000u, clipFlags = 0u; // PERF-BK
    fxRand3(r, vf21, vf21); // PERF-BK
    __m128 vf27 = vf0, vf28 = vf0, vf29 = vf0, vf30 = vf0, vf31 = vf0; // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<2>(vf20, one, fmacV<1>(vf21, one, zero))); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fxItof0(_mm_castsi128_ps(ftoiS<1>(vf27)))); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<1>(vf27, vf29, zero)); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<2>(vf18, one, fmacV<4>(vf27, vf19, zero))); // PERF-BK
    vf28 = fxMatMul(vf10, vf11, vf12, vf13, vf27, true); // PERF-BK
    uint16_t vi3 = 512u, vi4 = 768u, vi6 = vi3, vi5 = vi3, vi7 = vi3, vi8 = vi4; // PERF-BK
    int16_t vi2 = static_cast<int16_t>(count - 2u); // PERF-BK
    const __m128 i01 = _mm_set1_ps(0.01f); // PERF-BK
    // Prologue particle 0 (00F8..0240) // PERF-BK
    vf29 = fxBlend<0x3>(vf29, fmacV<1>(vf28, vf24, zero)); // PERF-BK
    vf29 = fxBlend<0xC>(vf29, maxV(vf28, vf28)); // PERF-BK
    vf27 = fxBlend<0x3>(vf27, fmacV<0>(vf28, vf24, zero)); // PERF-BK
    vf27 = fxBlend<0xC>(vf27, maxV(vf28, vf28)); // PERF-BK
    fxRand3(r, vf21, vf21); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf29)) & 0xFFFFFFu; // PERF-BK
    vf29 = fxMatMul(vf14, vf15, vf16, vf17, vf29, false); // PERF-BK
    vf18 = fxBlend<0x8>(vf18, fmacV<4>(vf28, i01, zero)); // PERF-BK
    vi3 = vi6; vi6 = vi4; // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fmacV<2>(vf20, one, fmacV<1>(vf21, one, zero))); // PERF-BK
    vf25 = fxItof0(vf25); // PERF-BK
    stq(vu, vi5++, vf26); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
    vf30 = fxBlend<0x7>(vf30, _mm_castsi128_ps(ftoiS<1>(vf28))); // PERF-BK
    vf18 = fxBlend<0x8>(vf18, miniV(vf18, vf0)); // PERF-BK
    float q = divQ(vf29); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fxMatMul(vf14, vf15, vf16, vf17, vf27, false)); // PERF-BK
    vf31 = fxBlend<0x7>(vf31, fxItof0(vf30)); // PERF-BK
    vf18 = fxBlend<0x8>(vf18, maxV(vf18, zero)); // PERF-BK
    vf30 = fxBlend<0x7>(vf30, maxV(vf25, vf25)); // PERF-BK
    vf31 = fxBlend<0x7>(vf31, fmacV<1>(vf28, vf31, zero)); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<4>(vf27, _mm_set1_ps(q), zero)); // PERF-BK
    vf30 = fxBlend<0x8>(vf30, fmacV<4>(vf25, vf18, zero)); // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fmacV<4>(vf29, _mm_set1_ps(q), zero)); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf18, one, fmacV<4>(vf31, vf19, zero))); // PERF-BK
    const uint16_t adc0 = fxFcor6(clipFlags); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf27))); // PERF-BK
    vf30 = _mm_castsi128_ps(ftoiS<1>(vf30)); // PERF-BK
    vf28 = fxMfirW(vf28, adc0); // PERF-BK
    vf28 = fxBlend<0x7>(vf28, _mm_castsi128_ps(ftoiS<16>(vf28))); // PERF-BK
    vf27 = fxMfirW(vf27, adc0); // PERF-BK
    vf29 = fxMatMul(vf10, vf11, vf12, vf13, vf29, true); // PERF-BK
    uint16_t vi1 = vi5; // PERF-BK
    stq(vu, vi1++, vf30); // PERF-BK
    stq(vu, vi1++, vf22); // PERF-BK
    stq(vu, vi1++, vf28); // PERF-BK
    stq(vu, vi1++, vf23); // PERF-BK
    stq(vu, vi1++, vf27); // PERF-BK
    vf27 = fxBlend<0x3>(vf27, fmacV<1>(vf29, vf24, zero)); // PERF-BK
    vf27 = fxBlend<0xC>(vf27, maxV(vf29, vf29)); // PERF-BK
    vi5 = vi4; // PERF-BK
    vf28 = fxBlend<0x3>(vf28, fmacV<0>(vf29, vf24, zero)); // PERF-BK
    // Main loop (0248..0380 + delay 0388) // PERF-BK
    do // PERF-BK
    { // PERF-BK
        vf28 = fxBlend<0xC>(vf28, maxV(vf29, vf29)); // PERF-BK
        fxRand3(r, vf21, vf21); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
        vf27 = fxMatMul(vf14, vf15, vf16, vf17, vf27, false); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf28)) & 0xFFFFFFu; // PERF-BK
        vi4 = vi6; vi6 = vi3; // PERF-BK
        vf18 = fxBlend<0x8>(vf18, fmacV<4>(vf29, i01, zero)); // PERF-BK
        q = divQ(vf27); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf20, one, fmacV<1>(vf21, one, zero))); // PERF-BK
        vf27 = fxBlend<0x8>(vf27, miniV(vf18, vf0)); // PERF-BK
        vf30 = fxBlend<0x7>(vf30, _mm_castsi128_ps(ftoiS<1>(vf29))); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, fxMatMul(vf14, vf15, vf16, vf17, vf28, false)); // PERF-BK
        vf31 = fxBlend<0x7>(vf31, fxItof0(vf30)); // PERF-BK
        vf27 = fxBlend<0x8>(vf27, maxV(vf27, zero)); // PERF-BK
        vf30 = fxBlend<0x7>(vf30, maxV(vf25, vf25)); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, fmacV<4>(vf28, _mm_set1_ps(q), zero)); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<1>(vf29, vf31, zero)); // PERF-BK
        vf30 = fxBlend<0x8>(vf30, fmacV<4>(vf25, vf27, zero)); // PERF-BK
        vf21 = fxBlend<0x7>(vf21, maxV(vf21, vf21)); // PERF-BK
        const uint16_t adc = fxFcor6(clipFlags); // PERF-BK
        vf28 = fxMfirW(vf28, adc); // PERF-BK
        vf27 = fxMfirW(vf27, adc); // PERF-BK
        vf31 = fxBlend<0x7>(vf31, fmacV<4>(vf27, _mm_set1_ps(q), zero)); // PERF-BK
        stq(vu, vi5++, vf26); // PERF-BK
        if (!kick(vu, vi7, kicks, gif)) { s_fbPc = 0xF102u; return false; } // PERF-BK3
        vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf18, one, fmacV<4>(vf29, vf19, zero))); // PERF-BK
        vi2 = static_cast<int16_t>(vi2 - 1); // PERF-BK
        vf30 = _mm_castsi128_ps(ftoiS<1>(vf30)); // PERF-BK
        vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf28))); // PERF-BK
        vi7 = vi8; // PERF-BK
        vf28 = fxBlend<0x7>(vf28, _mm_castsi128_ps(ftoiS<16>(vf31))); // PERF-BK
        vi8 = vi5; vi5 = vi3; // PERF-BK
        vf29 = fxMatMul(vf10, vf11, vf12, vf13, vf29, true); // PERF-BK
        stq(vu, vi8++, vf30); // PERF-BK
        stq(vu, vi8++, vf22); // PERF-BK
        stq(vu, vi8++, vf28); // PERF-BK
        stq(vu, vi8++, vf23); // PERF-BK
        stq(vu, vi8++, vf27); // PERF-BK
        vf27 = fxBlend<0x3>(vf27, fmacV<1>(vf29, vf24, zero)); // PERF-BK
        vi8 = vi3; // PERF-BK
        vf27 = fxBlend<0xC>(vf27, maxV(vf29, vf29)); // PERF-BK
        vf28 = fxBlend<0x3>(vf28, fmacV<0>(vf29, vf24, zero)); // PERF-BK
        vi3 = vi4; // PERF-BK
    } while (vi2 > 0); // PERF-BK
    // Epilogue particle (0390..0490) // PERF-BK
    vf28 = fxBlend<0xC>(vf28, maxV(vf29, vf29)); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
    vf27 = fxMatMul(vf14, vf15, vf16, vf17, vf27, false); // PERF-BK
    vf17 = fxBlend<0x8>(vf17, fmacV<4>(vf29, i01, zero)); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf28)) & 0xFFFFFFu; // PERF-BK
    stq(vu, vi5++, vf26); // PERF-BK
    if (!kick(vu, vi7, kicks, gif)) { s_fbPc = 0xF103u; return false; } // PERF-BK3
    q = divQ(vf27); // PERF-BK3 (03D8 DIV reads vf27.w before same-pair MINI.w writes it)
    vf27 = fxBlend<0x8>(vf27, miniV(vf17, vf0)); // PERF-BK3
    vf28 = fxBlend<0x7>(vf28, fxMatMul(vf14, vf15, vf16, vf17, vf28, false)); // PERF-BK
    vf27 = fxBlend<0x8>(vf27, maxV(vf27, zero)); // PERF-BK
    vf17 = fxBlend<0x7>(vf17, maxV(vf25, vf25)); // PERF-BK
    vf17 = fxBlend<0x8>(vf17, fmacV<4>(vf25, vf27, zero)); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<4>(vf27, _mm_set1_ps(q), zero)); // PERF-BK
    vf25 = fxBlend<0x7>(vf25, fmacV<4>(vf28, _mm_set1_ps(q), zero)); // PERF-BK
    const __m128 vf26Out = _mm_castsi128_ps(ftoiS<1>(vf17)); // PERF-BK
    const uint16_t adcLast = fxFcor6(clipFlags); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf27))); // PERF-BK
    vf27 = fxMfirW(vf27, adcLast); // PERF-BK
    vf25 = fxMfirW(vf25, adcLast); // PERF-BK
    vf25 = fxBlend<0x7>(vf25, _mm_castsi128_ps(ftoiS<16>(vf25))); // PERF-BK
    stq(vu, vi5++, vf26Out); // PERF-BK
    stq(vu, vi5++, vf22); // PERF-BK
    stq(vu, vi5++, vf27); // PERF-BK
    stq(vu, vi5++, vf23); // PERF-BK
    stq(vu, vi5++, vf25); // PERF-BK
    if (!kick(vu, vi8, kicks, gif)) { s_fbPc = 0xF104u; return false; } // PERF-BK3
    return true; // PERF-BK3
} // PERF-BK

static bool fxProgram04A0(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif) // PERF-BK
{ // PERF-BK
    vncNote(0xF4A0u); // PERF-BK
    const uint32_t base = top & 0x3FFu; // PERF-BK
    const uint16_t count = ilw(vu, base + 11u, 0u); // PERF-BK FIX-BK2
    if (count > 4096u) // PERF-BK4
    { // PERF-BK3
        s_fbPc = 0xF105u; // PERF-BK3
        return false; // PERF-BK
    } // PERF-BK3
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f); // PERF-BK
    const __m128 vf10 = ldq(vu, base + 0u); // PERF-BK
    __m128 vf11 = ldq(vu, base + 1u), vf12 = ldq(vu, base + 2u), vf13 = ldq(vu, base + 3u); // PERF-BK
    const __m128 vf14 = ldq(vu, base + 4u), vf15 = ldq(vu, base + 5u), vf16 = ldq(vu, base + 6u); // PERF-BK
    __m128 vf17 = ldq(vu, base + 7u); // PERF-BK
    const __m128 vf18 = ldq(vu, base + 8u), vf19 = ldq(vu, base + 9u), vf20 = ldq(vu, base + 10u); // PERF-BK
    __m128 vf21 = ldq(vu, base + 12u); // PERF-BK
    const __m128 vf22 = ldq(vu, base + 13u), vf23 = ldq(vu, base + 14u), vf24 = ldq(vu, base + 15u), vf25 = ldq(vu, base + 16u); // PERF-BK
    uint32_t r = 0x3F800000u, clipFlags = 0u; // PERF-BK
    __m128 vf26 = zero, vf27 = zero, vf28 = zero, vf29 = zero, vf30 = zero, vf31 = zero; // PERF-BK
    fxRand3(r, vf21, vf26); // PERF-BK
    vf21 = fxBlend<0x7>(vf21, maxV(vf26, vf26)); // PERF-BK
    vf26 = fxBlend<0x7>(vf26, fmacV<2>(vf20, one, fmacV<1>(vf26, one, zero))); // PERF-BK
    r = fxRinit(vf21, 0); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fxItof0(_mm_castsi128_ps(ftoiS<1>(vf26)))); // PERF-BK
    vf26 = fxBlend<0x7>(vf26, fmacV<1>(vf26, vf27, zero)); // PERF-BK
    vf29 = fxRnext(r, vf29, 0); r = fxRinit(vf21, 1); vf29 = fxRnext(r, vf29, 1); r = fxRinit(vf21, 2); vf29 = fxRnext(r, vf29, 2); // PERF-BK
    vf26 = fxBlend<0x7>(vf26, fmacV<2>(vf18, one, fmacV<4>(vf26, vf19, zero))); // PERF-BK
    vf21 = fxBlend<0x7>(vf21, maxV(vf29, vf29)); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<0>(vf26, vf24, zero)); // PERF-BK
    vf26 = fxMatMul(vf10, vf11, vf12, vf13, vf26, true); // PERF-BK
    vf27 = fxMatMul(vf10, vf11, vf12, vf13, vf27, true); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf20, one, fmacV<1>(vf29, one, zero))); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf26)) & 0xFFFFFFu; // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fxItof0(_mm_castsi128_ps(ftoiS<1>(vf29)))); // PERF-BK
    vf26 = fxMatMul(vf14, vf15, vf16, vf17, vf26, false); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
    uint16_t vi3 = 512u, vi4 = 768u; // PERF-BK
    int16_t vi2 = static_cast<int16_t>(count - 2u); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<1>(vf29, vf28, zero)); // PERF-BK
    vf27 = fxMatMul(vf14, vf15, vf16, vf17, vf27, false); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf18, one, fmacV<4>(vf29, vf19, zero))); // PERF-BK
    // Prologue particle 0 (0620..0760) // PERF-BK
    const uint16_t adc0 = fxFcor4Z(clipFlags); // PERF-BK
    float qA = divQ(vf26); // PERF-BK
    vf31 = fxBlend<0x7>(vf31, fmacV<0>(vf29, vf24, zero)); // PERF-BK
    vf30 = fxMatMul(vf10, vf11, vf12, vf13, vf29, true); // PERF-BK
    fxRand3(r, vf21, vf28); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<4>(vf26, _mm_set1_ps(qA), zero)); // PERF-BK
    qA = divQ(vf27); // PERF-BK
    vf26 = fxMatMul(vf10, vf11, vf12, vf13, vf31, true); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf30)) & 0xFFFFFFu; // PERF-BK
    vf31 = maxV(vf30, vf30); // PERF-BK
    vf21 = fxBlend<0x7>(vf21, maxV(vf28, vf28)); // PERF-BK
    vi2 = static_cast<int16_t>(vi2 - 1); // PERF-BK
    vf30 = fxBlend<0x7>(vf30, fmacV<2>(vf20, one, fmacV<1>(vf28, one, zero))); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf26)) & 0xFFFFFFu; // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fmacV<4>(vf27, _mm_set1_ps(qA), zero)); // PERF-BK
    vf29 = fxMfirW(vf29, adc0); // PERF-BK
    uint16_t vi5 = vi3; // PERF-BK
    stq(vu, vi5++, vf25); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<1>(vf30))); // PERF-BK
    vf28 = fxMfirW(vf28, adc0); // PERF-BK
    vf28 = fxBlend<0x7>(vf28, _mm_castsi128_ps(ftoiS<16>(vf28))); // PERF-BK
    stq(vu, vi5++, vf22); // PERF-BK
    const __m128 vf27Next = fxMatMul(vf14, vf15, vf16, vf17, vf31, false); // PERF-BK
    vf31 = fxBlend<0x7>(vf31, fxItof0(vf27)); // PERF-BK
    vf27 = vf27Next; // PERF-BK
    vf29 = fxBlend<0x7>(vf29, _mm_castsi128_ps(ftoiS<16>(vf29))); // PERF-BK
    vf30 = fxBlend<0x7>(vf30, fmacV<1>(vf30, vf31, zero)); // PERF-BK
    vf26 = fxMatMul(vf14, vf15, vf16, vf17, vf26, false); // PERF-BK
    qA = divQ(vf27); // PERF-BK
    uint16_t adcLoop = fxFcor4Z(clipFlags); // PERF-BK
    stq(vu, vi5++, vf29); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf18, one, fmacV<4>(vf30, vf19, zero))); // PERF-BK
    stq(vu, vi5++, vf23); // PERF-BK
    stq(vu, vi5++, vf28); // PERF-BK
    if (!kick(vu, vi3, kicks, gif)) { s_fbPc = 0xF106u; return false; } // PERF-BK3
    vf28 = fxBlend<0x7>(vf28, fmacV<0>(vf29, vf24, zero)); // PERF-BK
    vi5 = vi3; // PERF-BK
    // Main loop (0768..0888 + delay 0890) // PERF-BK
    do // PERF-BK
    { // PERF-BK
        vi3 = vi4; // PERF-BK
        fxRand3(r, vf21, vf30); // PERF-BK
        vf31 = fxMatMul(vf10, vf11, vf12, vf13, vf29, true); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<4>(vf27, _mm_set1_ps(qA), zero)); // PERF-BK
        qA = divQ(vf26); // PERF-BK
        vf28 = fxMatMul(vf10, vf11, vf12, vf13, vf28, true); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf31)) & 0xFFFFFFu; // PERF-BK
        vi4 = vi5; // PERF-BK
        vf27 = vf31; // PERF-BK
        vf21 = fxBlend<0x7>(vf21, maxV(vf30, vf30)); // PERF-BK
        vf31 = fxBlend<0x7>(vf31, fmacV<2>(vf20, one, fmacV<1>(vf30, one, zero))); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf28)) & 0xFFFFFFu; // PERF-BK
        const uint16_t adcCur = adcLoop; // PERF-BK
        vf29 = fxMfirW(vf29, adcCur); // PERF-BK
        vf30 = fxBlend<0x7>(vf30, fmacV<4>(vf26, _mm_set1_ps(qA), zero)); // PERF-BK
        vi5 = vi3; // PERF-BK
        vf26 = fxBlend<0x7>(vf26, _mm_castsi128_ps(ftoiS<1>(vf31))); // PERF-BK
        vf30 = fxBlend<0x7>(vf30, _mm_castsi128_ps(ftoiS<16>(vf30))); // PERF-BK
        vf26 = fxBlend<0x7>(vf26, fxItof0(vf26)); // PERF-BK
        vf27 = fxMatMul(vf14, vf15, vf16, vf17, vf27, false); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, _mm_castsi128_ps(ftoiS<16>(vf29))); // PERF-BK
        vf31 = fxBlend<0x7>(vf31, fmacV<1>(vf31, vf26, zero)); // PERF-BK
        stq(vu, vi5++, vf25); // PERF-BK
        stq(vu, vi5++, vf22); // PERF-BK
        vf26 = fxMatMul(vf14, vf15, vf16, vf17, vf28, false); // PERF-BK
        vf30 = fxMfirW(vf30, adcCur); // PERF-BK
        stq(vu, vi5++, vf29); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<2>(vf18, one, fmacV<4>(vf31, vf19, zero))); // PERF-BK
        stq(vu, vi5++, vf23); // PERF-BK
        adcLoop = fxFcor4Z(clipFlags); // PERF-BK
        stq(vu, vi5++, vf30); // PERF-BK
        vi2 = static_cast<int16_t>(vi2 - 1); // PERF-BK
        if (!kick(vu, vi3, kicks, gif)) { s_fbPc = 0xF107u; return false; } // PERF-BK3
        qA = divQ(vf27); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, fmacV<0>(vf29, vf24, zero)); // PERF-BK
        vi5 = vi3; // PERF-BK
    } while (vi2 > 0); // PERF-BK
    // Epilogue two particles (0898..0A08) // PERF-BK
    vf29 = fxMatMul(vf10, vf11, vf12, vf13, vf29, true); // PERF-BK
    __m128 acc28 = fmacV<4>(vf10, bc<0>(vf28), zero); // PERF-BK
    acc28 = fmacV<2>(vf11, bc<1>(vf28), acc28); // PERF-BK
    acc28 = fmacV<2>(vf12, bc<2>(vf28), acc28); // PERF-BK
    const float qB = divQ(vf26); // PERF-BK3 (08B8 DIV; 08C8 MULq is 2 pairs later -> reads older qA from 0880)
    vf12 = fxBlend<0x7>(vf12, fmacV<4>(vf27, _mm_set1_ps(qA), zero)); // PERF-BK3
    clipFlags = ((clipFlags << 6) | clipBitsV(vf29)) & 0xFFFFFFu; // PERF-BK
    vf13 = fmacV<2>(vf13, one, acc28); // PERF-BK
    vf11 = maxV(vf29, vf29); // PERF-BK
    const uint16_t vi2Epi = vi4; // PERF-BK
    vf12 = fxBlend<0x7>(vf12, _mm_castsi128_ps(ftoiS<16>(vf12))); // PERF-BK
    vf26 = fxBlend<0x7>(vf26, fmacV<4>(vf26, _mm_set1_ps(qB), zero)); // PERF-BK3 (08F0 MULq is 7 pairs after 08B8)
    clipFlags = ((clipFlags << 6) | clipBitsV(vf13)) & 0xFFFFFFu; // PERF-BK
    vf12 = fxMfirW(vf12, adcLoop); // PERF-BK
    vi4 = vi2Epi; // PERF-BK
    stq(vu, vi4++, vf25); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf26))); // PERF-BK
    stq(vu, vi4++, vf22); // PERF-BK
    vf27 = fxMfirW(vf27, adcLoop); // PERF-BK
    vf26 = fxMatMul(vf14, vf15, vf16, vf17, vf11, false); // PERF-BK
    stq(vu, vi4++, vf12); // PERF-BK
    stq(vu, vi4++, vf23); // PERF-BK
    const float q1 = divQ(vf26); // PERF-BK
    vf17 = fxMatMul(vf14, vf15, vf16, vf17, vf13, false); // PERF-BK
    const float q2 = divQ(vf17); // PERF-BK
    const uint16_t adcEnd = fxFcor4Z(clipFlags); // PERF-BK
    stq(vu, vi4++, vf27); // PERF-BK
    if (!kick(vu, vi2Epi, kicks, gif)) { s_fbPc = 0xF108u; return false; } // PERF-BK3
    // 09A0 MULq vf26 reads q1 (0978 DIV Q2 has 7-cycle latency, ready at 09B0) // PERF-BK
    vf26 = fxBlend<0x7>(vf26, fmacV<4>(vf26, _mm_set1_ps(q1), zero)); // PERF-BK
    const uint16_t vi1Epi = vi5; // PERF-BK
    vf12 = fxBlend<0x7>(vf12, fmacV<4>(vf17, _mm_set1_ps(q2), zero)); // PERF-BK
    vf17 = fxMfirW(vf17, adcEnd); // PERF-BK
    vf17 = fxBlend<0x7>(vf17, _mm_castsi128_ps(ftoiS<16>(vf26))); // PERF-BK
    vi5 = vi1Epi; // PERF-BK
    vf12 = fxMfirW(vf12, adcEnd); // PERF-BK
    vf12 = fxBlend<0x7>(vf12, _mm_castsi128_ps(ftoiS<16>(vf12))); // PERF-BK
    stq(vu, vi5++, vf25); // PERF-BK
    stq(vu, vi5++, vf22); // PERF-BK
    stq(vu, vi5++, vf17); // PERF-BK
    stq(vu, vi5++, vf23); // PERF-BK
    stq(vu, vi5++, vf12); // PERF-BK
    if (!kick(vu, vi1Epi, kicks, gif)) { s_fbPc = 0xF109u; return false; } // PERF-BK3
    return true; // PERF-BK3
} // PERF-BK

static bool fxProgram0A18(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif) // PERF-BK
{ // PERF-BK
    vncNote(0xFA18u); // PERF-BK
    const uint32_t base = top & 0x3FFu; // PERF-BK
    const uint16_t count = ilw(vu, base + 11u, 0u); // PERF-BK FIX-BK2
    if (count > 4096u) // PERF-BK4
    { // PERF-BK3
        s_fbPc = 0xF10Au; // PERF-BK3
        return false; // PERF-BK
    } // PERF-BK3
    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f); // PERF-BK
    const __m128 vf10 = ldq(vu, base + 0u), vf11 = ldq(vu, base + 1u), vf12 = ldq(vu, base + 2u), vf13 = ldq(vu, base + 3u); // PERF-BK
    const __m128 vf14 = ldq(vu, base + 4u), vf15 = ldq(vu, base + 5u), vf16 = ldq(vu, base + 6u); // PERF-BK
    __m128 vf17 = ldq(vu, base + 7u), vf18 = ldq(vu, base + 8u); // PERF-BK
    const __m128 vf19 = ldq(vu, base + 9u), vf20 = ldq(vu, base + 10u); // PERF-BK
    __m128 vf21 = ldq(vu, base + 12u); // PERF-BK
    const __m128 vf22 = ldq(vu, base + 13u), vf23 = ldq(vu, base + 14u), vf24 = ldq(vu, base + 15u); // PERF-BK
    __m128 vf25 = ldq(vu, base + 16u); // PERF-BK
    const __m128 vf26 = ldq(vu, base + 17u); // PERF-BK
    uint32_t r = 0x3F800000u, clipFlags = 0u; // PERF-BK
    __m128 vf27 = vf0, vf28 = vf0, vf29 = vf0, vf30 = vf0, vf31 = vf0; // PERF-BK
    fxRand3(r, vf21, vf21); // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fmacV<2>(vf20, one, fmacV<1>(vf21, one, zero))); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fxItof0(_mm_castsi128_ps(ftoiS<1>(vf28)))); // PERF-BK
    vf28 = fxBlend<0x7>(vf28, fmacV<1>(vf28, vf27, zero)); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fmacV<2>(vf18, one, fmacV<4>(vf28, vf19, zero))); // PERF-BK
    const __m128 iInit = _mm_set1_ps(6.0f); // PERF-BK4 (0AA0 LOI 0x40C00000 = 6.0f)
    vf29 = fxBlend<0x7>(vf29, fmacV<4>(vf28, iInit, zero)); // PERF-BK
    vf30 = fxMatMul(vf10, vf11, vf12, vf13, vf27, true); // PERF-BK
    vf31 = fxBlend<0x7>(vf31, fmacV<3>(vf28, iInit, fmacV<0>(vf0, iInit, zero))); // PERF-BK
    vf25 = fxItof0(vf25); // PERF-BK
    vf27 = fxBlend<0x3>(vf27, fmacV<0>(vf30, vf24, zero)); // PERF-BK
    vf28 = fxBlend<0x3>(vf28, fmacV<1>(vf30, vf24, zero)); // PERF-BK
    const __m128 i01 = _mm_set1_ps(0.01f); // PERF-BK
    vf29 = fxBlend<0x7>(vf29, miniV(vf29, vf31)); // PERF-BK
    vf28 = fxBlend<0xC>(vf28, vf30); // PERF-BK
    vf18 = fxBlend<0x8>(vf18, fmacV<4>(vf30, i01, zero)); // PERF-BK
    vf27 = fxBlend<0xC>(vf27, maxV(vf30, vf30)); // PERF-BK
    uint16_t vi3 = 512u, vi4 = 768u, vi1 = vi3, vi6 = vi3, vi5 = vi3; // PERF-BK
    vi3 = vi4; vi4 = vi1; // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf28)) & 0xFFFFFFu; // PERF-BK
    vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<0>(vf29))); // PERF-BK
    clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
    vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<1>(vf29))); // PERF-BK
    stq(vu, vi6++, vf26); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, fxMatMul(vf14, vf15, vf16, vf17, vf27, false)); // PERF-BK
    int16_t vi2 = static_cast<int16_t>(count - 1u); // PERF-BK
    vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<2>(vf29))); // PERF-BK
    uint16_t vi7 = (clipFlags & 0x041u) == 0x041u ? 1u : 0u; // PERF-BK
    __m128 acc28 = fmacV<4>(vf14, bc<0>(vf28), zero); // PERF-BK
    acc28 = fmacV<2>(vf15, bc<1>(vf28), acc28); // PERF-BK
    // Main loop (0B68..0D28 + delay 0D30) // PERF-BK
    do // PERF-BK
    { // PERF-BK
        vf29 = fxBlend<0x7>(vf29, vf25); // PERF-BK
        acc28 = fmacV<2>(vf16, bc<2>(vf28), acc28); // PERF-BK
        vf28 = fmacV<2>(vf17, bc<3>(vf28), acc28); // PERF-BK
        vf18 = fxBlend<0x8>(vf18, miniV(vf18, vf0)); // PERF-BK
        fxRand3(r, vf21, vf21); // PERF-BK
        vf18 = fxBlend<0x8>(vf18, maxV(vf18, zero)); // PERF-BK
        const float q = divQ(vf28); // PERF-BK
        vf29 = fxBlend<0x8>(vf29, fmacV<4>(vf25, vf18, zero)); // PERF-BK
        uint16_t vi8 = vi6; // PERF-BK
        vf30 = fxBlend<0x7>(vf30, fmacV<2>(vf20, one, fmacV<1>(vf21, one, zero))); // PERF-BK
        vi7 = vi3; // PERF-BK
        vf21 = fxBlend<0x7>(vf21, maxV(vf21, vf21)); // PERF-BK
        const uint16_t adc = fxFcor6(clipFlags); // PERF-BK
        vf31 = _mm_castsi128_ps(ftoiS<1>(vf29)); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, _mm_castsi128_ps(ftoiS<1>(vf30))); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, fmacV<4>(vf28, _mm_set1_ps(q), zero)); // PERF-BK
        stq(vu, vi8++, vf31); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fxItof0(vf29)); // PERF-BK
        vf27 = fxBlend<0x7>(vf27, fmacV<4>(vf27, _mm_set1_ps(q), zero)); // PERF-BK
        vf28 = fxMfirW(vf28, adc); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, _mm_castsi128_ps(ftoiS<16>(vf28))); // PERF-BK
        vf27 = fxMfirW(vf27, adc); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<1>(vf30, vf29, zero)); // PERF-BK
        stq(vu, vi8++, vf22); // PERF-BK
        vi1 = vi5; // PERF-BK
        vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf27))); // PERF-BK
        vi6 = vi3; // PERF-BK
        stq(vu, vi8++, vf28); // PERF-BK
        vf28 = fxBlend<0x7>(vf28, maxV(vf29, vf29)); // PERF-BK
        stq(vu, vi8++, vf23); // PERF-BK
        const __m128 vf27Next = fxBlend<0x7>(vf27, fmacV<2>(vf18, one, fmacV<4>(vf29, vf19, zero))); // PERF-BK
        stq(vu, vi8++, vf27); // PERF-BK
        vf27 = vf27Next; // PERF-BK
        vi5 = vi3; vi3 = vi4; vi4 = vi7; // PERF-BK
        vf29 = fxBlend<0x7>(vf29, fmacV<4>(vf28, iInit, zero)); // PERF-BK4 (MULi.xyz with I = 6.0f from 0C40)
        if (!kick(vu, vi1, kicks, gif)) { s_fbPc = 0xF10Bu; return false; } // PERF-BK3
        vf30 = fxMatMul(vf10, vf11, vf12, vf13, vf27, true); // PERF-BK
        vf31 = fxBlend<0x7>(vf31, fmacV<3>(vf28, iInit, fmacV<0>(vf0, iInit, zero))); // PERF-BK4 (ADDAi/MSUBi with I = 6.0f from 0C40)
        vf27 = fxBlend<0x3>(vf27, fmacV<0>(vf30, vf24, zero)); // PERF-BK
        vf28 = fxBlend<0x3>(vf28, fmacV<1>(vf30, vf24, zero)); // PERF-BK
        vf29 = fxBlend<0x7>(vf29, miniV(vf29, vf31)); // PERF-BK
        vf28 = fxBlend<0xC>(vf28, vf30); // PERF-BK
        vf18 = fxBlend<0x8>(vf18, fmacV<4>(vf30, i01, zero)); // PERF-BK
        vf27 = fxBlend<0xC>(vf27, maxV(vf30, vf30)); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf28)) & 0xFFFFFFu; // PERF-BK
        vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<0>(vf29))); // PERF-BK
        stq(vu, vi6++, vf26); // PERF-BK
        clipFlags = ((clipFlags << 6) | clipBitsV(vf27)) & 0xFFFFFFu; // PERF-BK
        vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<1>(vf29))); // PERF-BK
        vi2 = static_cast<int16_t>(vi2 - 1); // PERF-BK
        vf27 = fxBlend<0x7>(vf27, fxMatMul(vf14, vf15, vf16, vf17, vf27, false)); // PERF-BK
        acc28 = fmacV<4>(vf14, bc<0>(vf28), zero); // PERF-BK
        acc28 = fmacV<2>(vf15, bc<1>(vf28), acc28); // PERF-BK
        vf18 = fxBlend<0x8>(vf18, miniV(vf18, bc<2>(vf29))); // PERF-BK
    } while (vi2 > 0); // PERF-BK
    // Epilogue particle (0D38..0DF8) // PERF-BK
    vf27 = fxBlend<0x8>(vf27, miniV(vf18, vf0)); // PERF-BK
    acc28 = fmacV<2>(vf16, bc<2>(vf28), acc28); // PERF-BK
    vf27 = fxBlend<0x8>(vf27, maxV(vf27, zero)); // PERF-BK
    vf17 = fmacV<2>(vf17, bc<3>(vf28), acc28); // PERF-BK
    vf25 = fxBlend<0x8>(vf25, fmacV<4>(vf25, vf27, zero)); // PERF-BK
    const float q = divQ(vf17); // PERF-BK
    vf25 = _mm_castsi128_ps(ftoiS<1>(vf25)); // PERF-BK
    stq(vu, vi6++, vf25); // PERF-BK
    vf25 = fxBlend<0x7>(vf25, fmacV<4>(vf17, _mm_set1_ps(q), zero)); // PERF-BK
    vf17 = fxBlend<0x7>(vf17, fmacV<4>(vf27, _mm_set1_ps(q), zero)); // PERF-BK
    const uint16_t adcLast = fxFcor6(clipFlags); // PERF-BK
    vf27 = fxMfirW(vf27, adcLast); // PERF-BK
    vf17 = fxMfirW(vf17, adcLast); // PERF-BK
    vf27 = fxBlend<0x7>(vf27, _mm_castsi128_ps(ftoiS<16>(vf25))); // PERF-BK
    vf17 = fxBlend<0x7>(vf17, _mm_castsi128_ps(ftoiS<16>(vf17))); // PERF-BK
    stq(vu, vi6++, vf22); // PERF-BK
    stq(vu, vi6++, vf27); // PERF-BK
    stq(vu, vi6++, vf23); // PERF-BK
    stq(vu, vi6++, vf17); // PERF-BK
    if (!kick(vu, vi5, kicks, gif)) { s_fbPc = 0xF10Cu; return false; } // PERF-BK3
    return true; // PERF-BK3
} // PERF-BK

// startPC / TOP / ITOP: only the main entry 0008 is supported (TOP is read by XTOP at 0010; ITOP is not used by the supported handlers).
static bool vu1FuncRun(uint8_t *vu, uint32_t startPC, uint32_t top, uint32_t itop, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif, bool vxA)
{
    (void)itop;
    const bool hpOn = hprof::on(); // PROBE-RD39
    const uint64_t hpRun0 = hpOn ? __rdtsc() : 0ull; // PROBE-RD39
    kicks.clear();
    gif.clear();
    s_fbPc = 0u;
    const uint32_t pc = startPC & 0xFFFFu; // PERF-BK
    if ((pc == 0x0000u || pc == 0x04A0u || pc == 0x0A18u) && t_curSlot >= 0 && (s_imgCache[t_curSlot].mask & (1ull << 63)) != 0ull) // PERF-BK
    { // PERF-BK
        t_newN = 0u; // PERF-BK
        UndoLog &log = undoLog(); // PERF-BK
        log.clear(); // PERF-BK
        const bool fxOk = (pc == 0x0000u) ? fxProgram0000(vu, top, kicks, gif) : (pc == 0x04A0u) ? fxProgram04A0(vu, top, kicks, gif) : fxProgram0A18(vu, top, kicks, gif); // PERF-BK
        if (fxOk) // PERF-BK
        {
            if (hpRun0) hprof::g_st.takenT += __rdtsc() - hpRun0; // PROBE-RD39
            return true; // PERF-BK
        }
        const uint64_t hpUndo0 = hpOn ? __rdtsc() : 0ull; // PROBE-RD39
        for (uint32_t i = log.size; i-- > 0;) // PERF-BK
            std::memcpy(vu + (static_cast<uint32_t>(log.data[i].qword) << 4), log.data[i].old, 16); // PERF-BK
        log.clear(); // PERF-BK
        if (hpUndo0) hprof::g_st.undoT += __rdtsc() - hpUndo0; // PROBE-RD39
        kicks.clear(); // PERF-BK
        gif.clear(); // PERF-BK
        if (hpRun0) hprof::g_st.fallbackT += __rdtsc() - hpRun0; // PROBE-RD39
        return false; // PERF-BK
    } // PERF-BK
    const bool oneShot = (startPC & 0xFFFFu) == 0x40u; // 0040: vi15 := 0, then entry at 0010; one handler, then END
    if ((startPC & 0xFFFFu) != 8u && !oneShot)
    {
        s_fbPc = 0xF001u;
        ++s_startPcs[startPC & 0xFFFFu];
        if (hpRun0) hprof::g_st.fallbackT += __rdtsc() - hpRun0; // PROBE-RD39
        return false;
    }
    // Learned skip: 256-entry direct-mapped memo keyed by the first vi1 (ILW.x at TOP); 4+ consecutive fallbacks -> skip, every 64th still tries.
    struct Memo
    {
        uint16_t key;
        uint32_t streak;
    };
    static thread_local Memo memo[256] = {};
    static thread_local uint32_t skipCount = 0;
    static const bool memoOn = []() { const char *e = std::getenv("PS2X_VU1_FUNC_MEMO"); return e && e[0] && e[0] != '0'; }();
    Memo *m = nullptr; // PERF-V1G: skip dummy Memo init when !memoOn
    if (memoOn)
    {
        const uint16_t first = ilw(vu, top, 0);
        m = &memo[first & 0xFFu];
        if (m->key != first)
        {
            m->key = first;
            m->streak = 0;
        }
        if (m->streak >= 4u && (++skipCount & 63u) != 0u)
        {
            s_fbPc = 0xF040u;
            if (hpRun0) hprof::g_st.fallbackT += __rdtsc() - hpRun0; // PROBE-RD39
            return false;
        }
    }
    UndoLog &log = undoLog(); // PERF-V1G
    log.clear();
    size_t sceneD0 = 0, sceneV0 = 0; // PERF-SA
    if (scene::on()) // PERF-SA
    {
        const uint64_t hpRec0 = hpOn ? __rdtsc() : 0ull; // PROBE-RD39
        scene::beginMscal(vu, sceneD0, sceneV0);
        if (hpRec0) hprof::g_st.recT += __rdtsc() - hpRec0; // PROBE-RD39
    }
    if (vu1FuncRunInner(vu, top, kicks, gif, oneShot, vxA))
    {
        if (scene::on()) // PERF-SA
        {
            const uint64_t hpRec0 = hpOn ? __rdtsc() : 0ull; // PROBE-RD39
            scene::commitMscal(static_cast<uint32_t>(gif.size()));
            if (hpRec0) hprof::g_st.recT += __rdtsc() - hpRec0; // PROBE-RD39
        }
        if (m)
            m->streak = 0;
        if (hpRun0) hprof::g_st.takenT += __rdtsc() - hpRun0; // PROBE-RD39
        return true;
    }
    if (scene::on()) // PERF-SA
        scene::rollbackMscal(sceneD0, sceneV0);
    if (m)
        ++m->streak;
    const uint64_t hpUndo0 = hpOn ? __rdtsc() : 0ull; // PROBE-RD39
    for (uint32_t i = log.size; i-- > 0;)
        std::memcpy(vu + (static_cast<uint32_t>(log.data[i].qword) << 4), log.data[i].old, 16);
    log.clear();
    if (hpUndo0) hprof::g_st.undoT += __rdtsc() - hpUndo0; // PROBE-RD39
    kicks.clear();
    gif.clear();
    if (hpRun0) hprof::g_st.fallbackT += __rdtsc() - hpRun0; // PROBE-RD39
    return false;
}

inline bool funcOn()
{
    // REVERSAL: PS2X_VU1_FUNC=0 restores the exact path (docs/REVERSAL_MEMO.md)
    static const bool on = []() { const char *e = std::getenv("PS2X_VU1_FUNC"); return !(e && e[0] == '0'); }();
    return on;
}
inline bool replayFuncOn()
{
    static const bool on = []() { const char *e = std::getenv("PS2X_VU1_REPLAY_FUNC"); return e && e[0] == '1'; }();
    return on;
}

static void reportMaybe()
{
    static uint32_t calls = 0;
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    if ((++calls & 0x3Fu) != 0u)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 5000)
        return;
    last = now;
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto &kv : s_fbPcs)
        v.emplace_back(kv.second, kv.first);
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    char buf[1024]; // PERF-BE
    int n = std::snprintf(buf, sizeof(buf), "[vfunc] taken=%llu fallback=%llu fb_pcs:", (unsigned long long)s_taken, (unsigned long long)s_fallback);
    for (size_t i = 0; i < v.size() && i < 8u && n > 0 && n < 440; ++i)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %04X:%llu", v[i].second, (unsigned long long)v[i].first);
    {
        std::vector<std::pair<uint64_t, uint32_t>> sp;
        for (const auto &kv : s_startPcs)
            sp.emplace_back(kv.second, kv.first);
        std::sort(sp.begin(), sp.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        if (n > 0 && n < 440)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " start_pcs:");
        for (size_t i = 0; i < sp.size() && i < 6u && n > 0 && n < 490; ++i)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %04X:%llu", sp[i].second, (unsigned long long)sp[i].first);
    }
    {
        if (n > 0 && n < 490)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " 1ee0_why:");
        for (int r = 0; r < kFbCount && n > 0 && n < 500; ++r)
            if (s_fb1EE0[r] != 0u)
                n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s:%llu", kFb1EE0Names[r], (unsigned long long)s_fb1EE0[r]);
    }
    if (s_fb1EE0[kFbSkinN] != 0u && n > 0 && n < 700)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " skinN: 0:%llu 1:%llu 2:%llu 3:%llu 4-99:%llu 100-199:%llu 200+:%llu",
                           (unsigned long long)s_skinNHist[0], (unsigned long long)s_skinNHist[1], (unsigned long long)s_skinNHist[2], (unsigned long long)s_skinNHist[3],
                           (unsigned long long)s_skinNHist[4], (unsigned long long)s_skinNHist[5], (unsigned long long)s_skinNHist[6]);
    if (n > 0 && static_cast<size_t>(n) < sizeof(buf) - 16u) // PERF-BE
    { // PERF-BE
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " img:"); // PERF-BE
        for (int i = 0; i < 8 && n > 0 && static_cast<size_t>(n) < sizeof(buf) - 48u; ++i) // PERF-BE
        { // PERF-BE
            if (!s_imgCache[i].used) continue; // PERF-BE
            uint64_t offMask = (~s_imgCache[i].mask) & ((1ull << 36) - 1ull); // PERF-BE // PERF-BF // PERF-BI
            uint32_t offbits = 0; // PERF-BE
            while (offMask) { offMask &= (offMask - 1ull); ++offbits; } // PERF-BE
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %08llx:%llu/%llu/%llu/%u", // PERF-BE
                               (unsigned long long)(s_imgCache[i].hash >> 32), (unsigned long long)s_imgCache[i].runs, // PERF-BE
                               (unsigned long long)s_imgCache[i].taken, (unsigned long long)s_imgCache[i].fb, offbits); // PERF-BE
        } // PERF-BE
    } // PERF-BE
    ps2xSafeLog("%s\n", buf);
}

// PERF-V1I: per-thread PATH1 GIF batching on Vu1Exec (reduces ~6300 arbiter submits/frame to ~dozens)
constexpr size_t kPath1BatchCap = 256u * 1024u; // PERF-V1I
static thread_local bool t_path1BatchOn = false; // PERF-V1I
static thread_local PS2Memory *t_path1Memory = nullptr; // PERF-V1I
static thread_local std::vector<uint8_t> t_path1Pending; // PERF-V1I

static inline bool path1PacketBatchable(const uint8_t *d, uint32_t n) // PERF-V1I
{
    if (n < 16u || (n & 15u) != 0u)
        return false;
    uint32_t off = 0;
    while (off < n)
    {
        if (off + 16u > n)
            return false;
        uint64_t lo, hi;
        std::memcpy(&lo, d + off, 8);
        std::memcpy(&hi, d + off + 8, 8);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        if (off == 0u && nloop == 4u && flg == 0u && nreg == 1u && (hi & 0xFull) == 0xEull)
            return false; // native image-upload setup tag (GS::tryProcessNativeImageUploadPacket checks offset 0)
        uint64_t bytes;
        if (flg == 0u)
            bytes = static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (flg == 1u)
            bytes = ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
        else
            return false; // IMAGE / disabled: needs per-packet handling
        if (bytes > n - off - 16u)
            return false;
        off += 16u + static_cast<uint32_t>(bytes);
    }
    return off == n;
}

static inline void flushPath1Pending(bool countVx) // PERF-V1I
{
    if (t_path1Pending.empty())
        return;
    PS2Memory *mem = t_path1Memory;
    if (mem)
    {
        const uint64_t t0 = (countVx && vx::act()) ? __rdtsc() : 0ull;
        mem->submitGifPacket(GifPathId::Path1, t_path1Pending.data(), static_cast<uint32_t>(t_path1Pending.size()));
        if (t0)
            vx::g_vx.gifT += __rdtsc() - t0;
    }
    t_path1Pending.clear();
}

static inline void submitPath1Batched(PS2Memory *memory, const uint8_t *p, uint32_t n) // PERF-V1I
{
    if (!t_path1BatchOn)
    {
        memory->submitGifPacket(GifPathId::Path1, p, n);
        return;
    }
    if (t_path1Memory != memory)
    {
        flushPath1Pending(false);
        t_path1Memory = memory;
    }
    if (!path1PacketBatchable(p, n))
    {
        flushPath1Pending(false);
        memory->submitGifPacket(GifPathId::Path1, p, n);
        return;
    }
    if (!t_path1Pending.empty() && t_path1Pending.size() + n > kPath1BatchCap)
        flushPath1Pending(false);
    if (n > kPath1BatchCap)
    {
        memory->submitGifPacket(GifPathId::Path1, p, n);
        return;
    }
    if (t_path1Pending.capacity() < kPath1BatchCap)
        t_path1Pending.reserve(kPath1BatchCap);
    t_path1Pending.insert(t_path1Pending.end(), p, p + n);
}

// Engine + GIF submission (same order / destinations as finishXgkick). Returns true when the program was executed functionally.
static bool vu1FuncExec(uint8_t *vuData, uint32_t dataSize, uint32_t startPC, uint32_t top, uint32_t itop, GS &gs, PS2Memory *memory, std::vector<uint8_t> *capture, bool stats)
{
    static thread_local std::vector<std::pair<uint32_t, uint32_t>> kicks;
    static thread_local std::vector<uint8_t> gif;
    bool ok = false;
    const uint64_t vxT0 = vx::act() ? __rdtsc() : 0ull; // PROBE-VX
    uint64_t vxT1 = 0; // PROBE-VX
    struct VxGif { uint64_t &t1; uint64_t t0; ~VxGif() { if (t0 && t1) vx::g_vx.gifT += __rdtsc() - t1; } } vxGif{vxT1, vxT0}; // PROBE-VX: submit time (t1 set only when ok)
    if (dataSize == 16384u)
        ok = vu1FuncRun(vuData, startPC, top, itop, kicks, gif, vxT0 != 0ull); // PERF-V1G
    else
        s_fbPc = 0xF002u;
    if (vxT0) // PROBE-VX
    {
        const uint64_t n = __rdtsc();
        if (ok) { vx::g_vx.compT += n - vxT0; vxT1 = n; }
        else { vx::g_vx.fbT += n - vxT0; ++vx::g_vx.fbN; }
    }
    if (stats)
    {
        if (ok)
        { // PERF-BE
            ++s_taken;
            if (t_curSlot >= 0) ++s_imgCache[t_curSlot].taken; // PERF-BE
        } // PERF-BE
        else
        {
            ++s_fallback;
            if (t_curSlot >= 0) ++s_imgCache[t_curSlot].fb; // PERF-BE
            ++s_fbPcs[s_fbPc];
        }
        if (!capture)
            reportMaybe();
    }
    if (!ok)
    {
        if (!capture && scene::on()) // FEAT-RD2
            ++scene::t_scene.totalFallbacks;
        if (t_path1BatchOn && !capture) // PERF-V1I: flush pending PATH1 before fallback submits directly
            flushPath1Pending(true);
        return false;
    }
    {
        // PERF: kicks are normally appended back to back in `gif`; then submit the whole stream once (one arbiter pass instead of one per kick).
        const uint32_t prefixBytes = (!capture && scene::on() && ::ps2xRdActive()) ? 32u : 0u; // FEAT-RD5 // FEAT-RD49
        uint32_t expect = 0u;
        bool contiguous = true;
        for (const auto &k : kicks)
        {
            if (k.second == 0u)
                continue;
            if (k.first != expect + prefixBytes)
            {
                contiguous = false;
                break;
            }
            expect += prefixBytes + k.second;
        }
        if (contiguous && expect == gif.size())
        {
            if (expect != 0u)
            {
                if (capture)
                    capture->insert(capture->end(), gif.data(), gif.data() + expect);
                else if (memory)
                    submitPath1Batched(memory, gif.data(), expect); // PERF-V1I
                else
                    gs.processGIFPacket(gif.data(), expect);
            }
            return true;
        }
    }
    const uint32_t prefixBytes = (!capture && scene::on() && ::ps2xRdActive()) ? 32u : 0u; // FEAT-RD5 // FEAT-RD49
    for (const auto &k : kicks)
    {
        if (k.second == 0u)
            continue;
        const uint8_t *p = gif.data() + k.first - prefixBytes;
        const uint32_t len = k.second + prefixBytes;
        if (capture)
            capture->insert(capture->end(), p, p + len);
        else if (memory)
            submitPath1Batched(memory, p, len); // PERF-V1I
        else
            gs.processGIFPacket(p, len);
    }
    return true;
}
} // namespace vu1func

// PERF-V1I: entry points used by vpExecThreadFunc (ps2_vif1_interpreter.cpp, declared extern there)
void ps2xVu1Path1Batch(bool on) // PERF-V1I
{
    if (!on)
        vu1func::flushPath1Pending(true);
    vu1func::t_path1BatchOn = on;
}
void ps2xVu1FlushPath1() // PERF-V1I
{
    vu1func::flushPath1Pending(true);
    if (vu1func::scene::on()) // PERF-SA
        vu1func::scene::onFlush();
    if (vu1func::hprof::on()) // PROBE-RD39
        ++vu1func::hprof::g_st.frames; // PROBE-RD39
    if (vu1func::watch::on()) // PROBE-M5
        vu1func::watch::s_vbl.fetch_add(1u, std::memory_order_relaxed);
}

// PROBE-VX: entry points used by vpExecThreadFunc (ps2_vif1_interpreter.cpp, declared extern there)
bool ps2xVxOn() { return vu1func::vx::on(); } // PROBE-VX
void ps2xVxMarkThread() { vu1func::vx::t_exec = true; } // PROBE-VX
void ps2xVxIdle(uint64_t ticks) { vu1func::vx::g_vx.idleT += ticks; } // PROBE-VX
void ps2xVxEv(uint32_t kind, uint64_t ticks) // PROBE-VX
{
    using namespace vu1func::vx;
    St &s = g_vx;
    s.evT[kind & 7u] += ticks;
    ++s.evN[kind & 7u];
    if (kind == 5u)
        ++s.frames; // VP_FLUSH == one VIF1 drain
    if ((++s.tick & 63u) != 0u)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (s.tsc0 == 0u)
    {
        s.t0 = now;
        s.tsc0 = __rdtsc();
        return;
    }
    const double sec = std::chrono::duration<double>(now - s.t0).count();
    if (sec < 5.0)
        return;
    const uint64_t tn = __rdtsc();
    const double ms = 1000.0 / (static_cast<double>(tn - s.tsc0) / sec); // ms per tick
    const double fr = s.frames ? static_cast<double>(s.frames) : 1.0;
    uint64_t busy = 0;
    for (int i = 0; i < 8; ++i)
        busy += s.evT[i];
    const double k = ms / fr; // ms per frame per tick
    uint32_t ord[kHN];
    for (uint32_t i = 0; i < kHN; ++i)
        ord[i] = i;
    std::sort(ord, ord + kHN, [&](uint32_t a, uint32_t b) { return s.hT[a] > s.hT[b]; });
    char hb[512];
    int n = 0;
    hb[0] = 0;
    for (int i = 0; i < 8 && n < 440; ++i)
    {
        const uint32_t h = ord[i];
        if (s.hT[h] == 0u)
            break;
        n += std::snprintf(hb + n, sizeof(hb) - static_cast<size_t>(n), " %s=%.2f(%.1f)", kHNames[h], s.hT[h] * k, static_cast<double>(s.hN[h]) / fr);
    }
    ::ps2xSafeLog("[vx] frames=%llu ms/frame: busy=%.2f idle=%.2f | w=%.2f mscal=%.2f mscnt=%.2f gif=%.2f flush=%.2f other=%.2f | func: compute=%.2f gifsubmit=%.2f fallback=%.2f(n/frame %.1f) | h:%s\n",
                  (unsigned long long)s.frames, busy * k, s.idleT * k, s.evT[1] * k, s.evT[2] * k, s.evT[3] * k, s.evT[4] * k, s.evT[5] * k,
                  (s.evT[0] + s.evT[6] + s.evT[7]) * k, s.compT * k, s.gifT * k, s.fbT * k, static_cast<double>(s.fbN) / fr, hb);
    St fresh;
    fresh.t0 = now;
    fresh.tsc0 = tn;
    s = fresh;
}

// PROBE-M8: global-scope lookup entry points used by rd_view.cpp (declared extern there)
bool ps2xVu1SpikeLookupDraw(uint32_t serial, uint16_t &handlerPc, uint16_t &mscal,
                            uint8_t &clipRan, uint32_t &nVertsIn, uint32_t &nVertsOut,
                            float &minW, float &maxW, float &maxAbsXY) // PROBE-M8
{
    if (!vu1func::watch::spikeOn() || serial == 0u)
        return false;
    std::lock_guard<std::mutex> lk(vu1func::watch::s_m8Mtx);
    const vu1func::watch::M8DrawEntry &e = vu1func::watch::s_m8Ring[serial & (vu1func::watch::kM8RingSize - 1u)];
    if (e.serial != serial)
        return false;
    handlerPc = e.handlerPc;
    mscal = e.mscal;
    clipRan = e.clipRan;
    nVertsIn = e.nVertsIn;
    nVertsOut = e.nVertsOut;
    minW = e.minW;
    maxW = e.maxW;
    maxAbsXY = e.maxAbsXY;
    return true;
}

bool ps2xVu1SpikeLookupGif16(uint32_t serial, uint32_t vertIdx, int32_t &outXpx, int32_t &outYpx) // PROBE-M8
{
    if (!vu1func::watch::spikeOn() || serial == 0u)
        return false;
    std::lock_guard<std::mutex> lk(vu1func::watch::s_m8Mtx);
    const vu1func::watch::M8DrawEntry &e = vu1func::watch::s_m8Ring[serial & (vu1func::watch::kM8RingSize - 1u)];
    if (e.serial != serial || vertIdx >= e.nGifVerts)
        return false;
    outXpx = static_cast<int32_t>(e.gifX[vertIdx]) / 16;
    outYpx = static_cast<int32_t>(e.gifY[vertIdx]) / 16;
    return true;
}
