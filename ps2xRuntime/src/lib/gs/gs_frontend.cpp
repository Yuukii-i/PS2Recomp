#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "ps2_log.h"
#include "runtime/ps2_memory.h"
#include "probe_ae.h"
void ps2xSafeLog(const char *fmt, ...); // stderr is routed to NUL unless PS2X_VERBOSE_LOG=1
namespace probe_ae {
    void bhLog(const char *fmt, ...);
}
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <chrono>
#include <intrin.h>
#include <atomic>
std::atomic<uint64_t> g_gqBuild{0}, g_gqSubmit{0}, g_gqDraws{0}; // PROBE-GQ2
// PROBE-RDP: per-site rdtsc accounting (GS thread only; 0 noteDraw, 1 noteClut, 2 markDest, 3 0x7F nop)
#include <cstdlib> // PROBE-RDP
extern std::atomic<uint64_t> g_hostPresentSeq; // PROBE-RDP
bool g_rdpOn = []() { const char *e = std::getenv("PS2X_RD_PROF"); return e && e[0] == '1'; }(); // PROBE-RDP
uint64_t g_rdpCyc[4] = {0, 0, 0, 0}; // PROBE-RDP
uint32_t g_rdpCnt[4] = {0, 0, 0, 0}; // PROBE-RDP
static uint32_t s_rdpQueueDraws = 0u; // PROBE-RDP
struct RdpScope // PROBE-RDP
{
    int id; uint64_t t0;
    explicit RdpScope(int i) : id(i), t0(g_rdpOn ? __rdtsc() : 0ull) {} // PROBE-RDP
    ~RdpScope() { if (g_rdpOn) { g_rdpCyc[id] += __rdtsc() - t0; ++g_rdpCnt[id]; } } // PROBE-RDP
};
extern void ps2xSafeLog(const char *fmt, ...); // PROBE-RDP
extern uint64_t g_rdpNoteSubCyc[6]; // FEAT-RD45: 0=rt, 1=fast, 2=clut, 3=quickKey, 4=rawCrc, 5=decode
extern uint64_t g_rdpRtSubCyc[4];   // FEAT-RD46: 0=rt_a(fb), 1=rt_b(mask), 2=rt_c(mutex), 3=rt_d(sync)
static void rdpMaybeReport() // PROBE-RDP
{
    static uint64_t s_cyc0 = 0, s_seq0 = 0;
    static std::chrono::steady_clock::time_point s_t0;
    static bool s_init = false;
    const auto now = std::chrono::steady_clock::now();
    if (!s_init)
    {
        s_init = true; s_t0 = now; s_cyc0 = __rdtsc(); s_seq0 = g_hostPresentSeq.load(std::memory_order_relaxed);
        return;
    }
    const double dms = std::chrono::duration<double, std::milli>(now - s_t0).count();
    if (dms < 5000.0)
        return;
    const uint64_t cyc = __rdtsc() - s_cyc0;
    const uint64_t seq = g_hostPresentSeq.load(std::memory_order_relaxed);
    const double frames = static_cast<double>(seq - s_seq0 ? seq - s_seq0 : 1);
    const double cpm = static_cast<double>(cyc) / dms; // cycles per ms
    auto ms = [&](int i) { return static_cast<double>(g_rdpCyc[i]) / cpm / frames; };
    auto cn = [&](int i) { return static_cast<unsigned>(static_cast<double>(g_rdpCnt[i]) / frames); };
    auto subMs = [&](int i) { return static_cast<double>(g_rdpNoteSubCyc[i]) / cpm / frames; }; // FEAT-RD45
    auto rtSubMs = [&](int i) { return static_cast<double>(g_rdpRtSubCyc[i]) / cpm / frames; }; // FEAT-RD46
    ps2xSafeLog("[rdprof] per-frame ms: noteDraw=%.2f(%u) noteClut=%.2f(%u) markDest=%.2f(%u) nop=%.2f(%u) queueDraws=%u noteSub[rt=%.2f(rt_a=%.2f rt_b=%.2f rt_c=%.2f rt_d=%.2f) fast=%.2f clut=%.2f qk=%.2f crc=%.2f dec=%.2f]\n",
                ms(0), cn(0), ms(1), cn(1), ms(2), cn(2), ms(3), cn(3),
                static_cast<unsigned>(static_cast<double>(s_rdpQueueDraws) / frames),
                subMs(0), rtSubMs(0), rtSubMs(1), rtSubMs(2), rtSubMs(3), subMs(1), subMs(2), subMs(3), subMs(4), subMs(5));
    for (int i = 0; i < 4; ++i) { g_rdpCyc[i] = 0; g_rdpCnt[i] = 0; }
    for (int i = 0; i < 6; ++i) { g_rdpNoteSubCyc[i] = 0; } // FEAT-RD45
    for (int i = 0; i < 4; ++i) { g_rdpRtSubCyc[i] = 0; }   // FEAT-RD46
    s_rdpQueueDraws = 0u; s_t0 = now; s_cyc0 = __rdtsc(); s_seq0 = seq;
}
extern bool ps2xRdViewOn(); // FEAT-RD5
extern bool ps2xRdActive(); // FEAT-RD49
extern bool ps2xRdTakeoverOn(); // FEAT-RD12
extern void ps2xRdResetState(); // FEAT-RD49
extern void ps2xRdSetPresentSize(uint32_t w, uint32_t h); // FEAT-RD12
extern void ps2xRdNoteDraw(uint32_t serial, uint64_t tex0, uint64_t texa, const uint8_t *vram, const uint16_t *clut512); // FEAT-RD5
extern void ps2xRdNoteState(uint32_t serial, uint64_t prim, uint64_t alpha, uint64_t test, uint64_t frame,
                            uint64_t zbuf, uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa, uint64_t fba); // FEAT-RD17
extern std::atomic<uint32_t> g_rdStateChangesInKick; // FEAT-RD17
extern void ps2xRdNoteXyoff(uint32_t serial, uint64_t xyoff); // FEAT-RD21
extern void ps2xRdNoteScissor(uint32_t serial, uint64_t scis); // FEAT-RD22
extern const uint16_t *ps2xRdActiveClut(); // FEAT-RD5
static uint32_t s_rdCurSerial = 0u;       // FEAT-RD5
static uint32_t s_rdLastNotedSerial = 0u; // FEAT-RD5
static uint32_t s_rdLastStateSerial = 0u; // FEAT-RD17
static uint64_t s_rdFirstAlpha = 0ull;    // FEAT-RD17
static uint64_t s_rdFirstFrame = 0ull;    // FEAT-RD17
static uint64_t s_rdFirstTest = 0ull;     // FEAT-RD17
static bool s_rdKickStateChanged = false; // FEAT-RD17
static uint32_t s_rdLastNonZeroSerial = 0u; // FEAT-RD8
static uint32_t s_rdMainFbp = 0xFFFFFFFFu;  // FEAT-RD8b
static uint32_t s_rd2dTexCounter = 1u;      // FEAT-RD8
bool g_rdCurDrawIsGeom = false;             // FEAT-RD18
std::atomic<bool> g_rdForceClassic{false};  // FEAT-RD48
std::atomic<uint64_t> g_rdFrameRtSyncCyc{0ull}; // FEAT-RD48
static std::atomic<uint32_t> s_rdLatchPrims{0u}; // FEAT-RD50
static int rdForceClassicMode() // FEAT-RD48: 0=never, 1=always, 2=auto
{
    static const int mode = []() {
        const char *e = std::getenv("PS2X_RD_FORCECLASSIC");
        if (e && e[0] == '0') return 0;
        if (e && e[0] == '1') return 1;
        return 2;
    }();
    return mode;
}
bool ps2xRdForceClassic() // FEAT-RD48
{
    const int mode = rdForceClassicMode();
    if (mode == 0) return false;
    if (mode == 1) return true;
    return g_rdForceClassic.load(std::memory_order_acquire);
}
static std::atomic<uint64_t> s_rdFastGeomQwords{0ull}; // FEAT-RD38
static uint32_t s_rdFastGeomLastSerial = 0u;           // FEAT-RD38
static inline bool rdFastGeomOn()                      // FEAT-RD38
{
    static const bool enabled = []() {
        const char *e = std::getenv("PS2X_RD_FASTGEOM");
        return !(e && e[0] == '0'); // FEAT-RD38: default ON, PS2X_RD_FASTGEOM=0 disables
    }();
    return enabled && ps2xRdTakeoverOn() && !ps2xRdForceClassic(); // FEAT-RD48
}
bool ps2xRdFastGeomOn() // FEAT-RD40
{
    return rdFastGeomOn();
}
static inline bool rdIsPureVertexPackedReg(uint8_t reg) // FEAT-RD38
{
    return (reg >= 0x01u && reg <= 0x05u) || reg == 0x0Au || reg == 0x0Cu || reg == 0x0Du || reg == 0x0Fu;
}
bool g_rdClassicPresentRequested = false;   // FEAT-RD18
std::atomic<bool> g_rdFrameIsClassic{false}; // FEAT-RD18
extern std::atomic<uint32_t> g_rdRepeatFrames;       // FEAT-RD33c
extern std::atomic<uint32_t> g_rdClassicFramesTotal; // FEAT-RD33c
static std::atomic<bool> s_rdFrameHasGeom{false}; // FEAT-RD18
static std::atomic<uint32_t> s_rdFrameGeomFbp{0xFFFFFFFFu}; // FEAT-RD18b
static std::atomic<uint32_t> s_rdDispFbps[4] = {{0xFFFFFFFFu}, {0xFFFFFFFFu}, {0xFFFFFFFFu}, {0xFFFFFFFFu}}; // FEAT-RD19
static std::atomic<uint32_t> s_rdDispFbpNext{0u}; // FEAT-RD19
static std::atomic<uint32_t> s_rdDispFbw[4] = {{10u}, {10u}, {10u}, {10u}}; // FEAT-RD28b
static void rdNoteDispFbp(uint64_t dispfb) // FEAT-RD19
{
    const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFull);
    for (int i = 0; i < 4; ++i)
        if (s_rdDispFbps[i].load(std::memory_order_relaxed) == fbp)
            return;
    const uint32_t slot = s_rdDispFbpNext.fetch_add(1u, std::memory_order_relaxed) & 3u;
    s_rdDispFbw[slot].store(static_cast<uint32_t>((dispfb >> 9) & 0x3Full), std::memory_order_relaxed); // FEAT-RD28b
    s_rdDispFbps[slot].store(fbp, std::memory_order_relaxed);
}
bool ps2xRdIsDisplayFbp(uint32_t fbp) // FEAT-RD19
{
    for (int i = 0; i < 4; ++i)
        if (s_rdDispFbps[i].load(std::memory_order_relaxed) == fbp)
            return true;
    return false;
}
bool ps2xRdHasDisplayFbp() // FEAT-RD19
{
    for (int i = 0; i < 4; ++i)
        if (s_rdDispFbps[i].load(std::memory_order_relaxed) != 0xFFFFFFFFu)
            return true;
    return false;
}
static bool s_rdRunIsGeom = false;         // FEAT-RD18
// FEAT-RD23: main 3D pass = (FBP, XYOFFSET) with most geometry kicks in the previous frame
static std::atomic<uint64_t> s_rdPassKey[8] = {{~0ull}, {~0ull}, {~0ull}, {~0ull}, {~0ull}, {~0ull}, {~0ull}, {~0ull}}; // FEAT-RD23
static std::atomic<uint32_t> s_rdPassCnt[8]; // FEAT-RD23
static std::atomic<uint64_t> s_rdMainKey{~0ull}; // FEAT-RD23
static uint64_t rdPassKey(uint32_t fbp, uint64_t ofx, uint64_t ofy) // FEAT-RD23
{
    return (static_cast<uint64_t>(fbp & 0x1FFu) << 32) | ((ofx & 0xFFFFull) << 16) | (ofy & 0xFFFFull);
}
static void rdNotePassKick(uint32_t fbp, uint64_t ofx, uint64_t ofy) // FEAT-RD23
{
    const uint64_t k = rdPassKey(fbp, ofx, ofy);
    for (int i = 0; i < 8; ++i)
    {
        const uint64_t cur = s_rdPassKey[i].load(std::memory_order_relaxed);
        if (cur == k)
        {
            s_rdPassCnt[i].fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        if (cur == ~0ull)
        {
            uint64_t exp = ~0ull;
            s_rdPassKey[i].compare_exchange_strong(exp, k, std::memory_order_relaxed);
            if (exp == ~0ull || exp == k)
            {
                s_rdPassCnt[i].fetch_add(1u, std::memory_order_relaxed);
                return;
            }
        }
    }
}
static void rdRollPassFrame() // FEAT-RD23: called once per presented frame
{
    uint32_t best = 0u;
    uint64_t bestKey = ~0ull;
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t n = s_rdPassCnt[i].exchange(0u, std::memory_order_relaxed);
        const uint64_t k = s_rdPassKey[i].exchange(~0ull, std::memory_order_relaxed);
        if (k != ~0ull && n > best)
        {
            best = n;
            bestKey = k;
        }
    }
    if (best > 0u)
        s_rdMainKey.store(bestKey, std::memory_order_relaxed);
}
bool ps2xRdIsMainPass(uint32_t fbp, uint64_t xyoffset) // FEAT-RD23: xyoffset = ofx | ofy<<32
{
    const uint64_t m = s_rdMainKey.load(std::memory_order_relaxed);
    if (m == ~0ull)
        return true;
    return rdPassKey(fbp, xyoffset & 0xFFFFull, (xyoffset >> 32) & 0xFFFFull) == m;
}
uint64_t ps2xRdMainXyoff() // FEAT-RD23: ofx | ofy<<32, or ~0 if unknown
{
    const uint64_t m = s_rdMainKey.load(std::memory_order_relaxed);
    if (m == ~0ull)
        return ~0ull;
    return ((m >> 16) & 0xFFFFull) | ((m & 0xFFFFull) << 32);
}
uint32_t ps2xRdFbPageRanges(uint32_t *first, uint32_t *last) // FEAT-RD28b: up to 5 framebuffer page ranges (display FBPs + main pass FBP)
{
    uint32_t n = 0u;
    auto add = [&](uint32_t fbp, uint32_t fbw)
    {
        if (fbw == 0u)
            fbw = 10u;
        first[n] = fbp;
        last[n] = fbp + fbw * 16u - 1u; // fbw*64 px * 512 rows * 4 B / 8192
        if (last[n] > 511u)
            last[n] = 511u;
        ++n;
    };
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t f = s_rdDispFbps[i].load(std::memory_order_relaxed);
        if (f != 0xFFFFFFFFu)
            add(f, s_rdDispFbw[i].load(std::memory_order_relaxed));
    }
    const uint64_t m = s_rdMainKey.load(std::memory_order_relaxed);
    if (m != ~0ull)
    {
        const uint32_t mf = static_cast<uint32_t>((m >> 32) & 0x1FFull);
        bool have = false;
        for (uint32_t i = 0; i < n; ++i)
            have = have || first[i] == mf;
        if (!have)
            add(mf, 10u);
    }
    return n;
}

struct RdVert // FEAT-RD8
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2];
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

extern void ps2xRdPublish2D(const RdVert *v, size_t n, const Rd2DBatch *b, size_t nb, uint32_t rtSkipped, uint32_t pointLineSkipped, uint32_t gMin, uint32_t gMax, const uint32_t *mainRangePairs, size_t numMainRanges); // FEAT-RD8 // FEAT-RD13 // FEAT-RD33

struct RdKickRec // FEAT-RD33
{
    uint32_t serial;
    uint32_t fbp;
    uint64_t xyoff;
    uint32_t scW; // FEAT-RD33b
    uint32_t scH; // FEAT-RD33b
};
static bool rdScissorQualifies(uint32_t w, uint32_t h) // FEAT-RD33b: >=90% of 512x448 (fallback thresholds 500x400)
{
    return w >= 500u && h >= 400u;
}

static std::mutex s_rd2dCapMutex; // FEAT-RD8
static std::vector<RdVert> s_rd2dVerts; // FEAT-RD8
static std::vector<Rd2DBatch> s_rd2dBatches; // FEAT-RD8
static uint32_t s_rd2dRtSkipped = 0u; // FEAT-RD8
static uint32_t s_rd2dPointLineSkipped = 0u; // FEAT-RD8
static uint32_t s_rd2dGMin = 0u; // FEAT-RD13
static uint32_t s_rd2dGMax = 0u; // FEAT-RD13
static std::vector<RdKickRec> s_rdKickRecs; // FEAT-RD33
static uint32_t s_rdLastKickRecSerial = 0u; // FEAT-RD33
static bool rdNoteKickBookkeeping(const GSContext &ctx, const GSPrimReg &prim, const GSTexaReg &texa, const uint8_t *localMem); // FEAT-RD42

static uint64_t rdPackTex0(const GSTex0Reg &t) // FEAT-RD8
{
    return (static_cast<uint64_t>(t.tbp0) & 0x3FFFull) |
           ((static_cast<uint64_t>(t.tbw) & 0x3Full) << 14) |
           ((static_cast<uint64_t>(t.psm) & 0x3Full) << 20) |
           ((static_cast<uint64_t>(t.tw) & 0xFull) << 26) |
           ((static_cast<uint64_t>(t.th) & 0xFull) << 30) |
           ((static_cast<uint64_t>(t.tcc) & 0x1ull) << 34) |
           ((static_cast<uint64_t>(t.tfx) & 0x3ull) << 35) |
           ((static_cast<uint64_t>(t.cbp) & 0x3FFFull) << 37) |
           ((static_cast<uint64_t>(t.cpsm) & 0xFull) << 51) |
           ((static_cast<uint64_t>(t.csm) & 0x1ull) << 55) |
           ((static_cast<uint64_t>(t.csa) & 0x1Full) << 56) |
           ((static_cast<uint64_t>(t.cld) & 0x7ull) << 61);
}

static uint64_t rdPackTexa(const GSTexaReg &texa) // FEAT-RD8
{
    return static_cast<uint64_t>(texa.ta0) |
           (texa.aem ? (1ull << 15) : 0ull) |
           (static_cast<uint64_t>(texa.ta1) << 32);
}

static uint64_t rdPackPrim(const GSPrimReg &p) // FEAT-RD8
{
    return (static_cast<uint64_t>(p.type) & 7ull) |
           (p.iip ? (1ull << 3) : 0ull) |
           (p.tme ? (1ull << 4) : 0ull) |
           (p.fge ? (1ull << 5) : 0ull) |
           (p.abe ? (1ull << 6) : 0ull) |
           (p.aa1 ? (1ull << 7) : 0ull) |
           (p.fst ? (1ull << 8) : 0ull) |
           (p.ctxt ? (1ull << 9) : 0ull) |
           (p.fix ? (1ull << 10) : 0ull);
}

static uint64_t rdPackFrame(const GSFrameReg &f) // FEAT-RD8
{
    return (static_cast<uint64_t>(f.fbp) & 0x1FFull) |
           ((static_cast<uint64_t>(f.fbw) & 0x3Full) << 16) |
           ((static_cast<uint64_t>(f.psm) & 0x3Full) << 24) |
           (static_cast<uint64_t>(f.fbmsk) << 32);
}

static uint64_t rdPackScissor(const GSContext &c) // FEAT-RD22
{
    return static_cast<uint64_t>(c.scissor.x0 & 0x7FF) | (static_cast<uint64_t>(c.scissor.x1 & 0x7FF) << 16) |
           (static_cast<uint64_t>(c.scissor.y0 & 0x7FF) << 32) | (static_cast<uint64_t>(c.scissor.y1 & 0x7FF) << 48);
}

static uint64_t rdPackZbuf(const GSZbufReg &z) // FEAT-RD8
{
    return (static_cast<uint64_t>(z.zbp) & 0x1FFull) |
           ((static_cast<uint64_t>(z.psm) & 0xFull) << 24) |
           (z.zmask ? (1ull << 32) : 0ull);
}
extern void ps2xRdMarkVramWrite(uint32_t firstByte, uint32_t byteCount); // FEAT-RD5b
static void rdMarkDest(uint32_t psm, uint32_t dbp, uint32_t dbw, uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh) // FEAT-RD5b
{
    uint32_t pageH = 32u, bppNum = 4u, bppDen = 1u; // CT32/CT24 and unknown (conservative)
    switch (psm)
    {
    case 2u: case 10u: case 50u: case 58u: pageH = 64u; bppNum = 2u; break; // CT16/CT16S/Z16
    case 19u: pageH = 64u; bppNum = 1u; break;                              // T8
    case 20u: pageH = 128u; bppNum = 1u; bppDen = 2u; break;                // T4
    default: break;
    }
    const uint32_t rows = std::min<uint32_t>(((dsay + rrh + pageH - 1u) / pageH) * pageH, 2048u);
    const uint32_t widthPx = std::max<uint32_t>(std::max<uint32_t>(dbw, 1u) * 64u, dsax + rrw);
    const uint64_t bytes = (static_cast<uint64_t>(widthPx) * rows * bppNum + bppDen - 1u) / bppDen;
    const uint64_t pages = (bytes + 8191u) / 8192u + 1u;
    ps2xRdMarkVramWrite(dbp * 256u, static_cast<uint32_t>(std::min<uint64_t>(pages * 8192u, 0x400000u)));
}
extern "C" void ps2_gs_front_stats(uint64_t *out) { out[0] = g_gqBuild.exchange(0); out[1] = g_gqSubmit.exchange(0); out[2] = g_gqDraws.exchange(0); }

// PROBE-GW: GsWorker CPU-time split (PS2X_GW=1). Exclusive time per category via a category switch; only the thread that called ps2_gw_mark() accounts.
bool g_gwOn = []() { const char *e = std::getenv("PS2X_GW"); return e && e[0] == '1'; }(); // PROBE-GW
std::atomic<uint64_t> g_gwFrames{0}, g_gwPresentT{0}; // PROBE-GW: presents (any thread), present latch ticks (any thread)
namespace gwimpl // PROBE-GW
{
enum { cOther, cIdle, cRegs, cKick, cImage, cPrim, cState, cVkrec, cSubmit, cSync, cPresent, cN };
static thread_local bool t_mark = false;
static int s_cur = cOther;
static uint64_t s_last = 0, s_T[cN] = {}, s_prims = 0, s_imgBytes = 0, s_runs = 0, s_brk = 0, s_redund = 0, s_f0 = 0, s_p0 = 0, s_tsc0 = 0; // PERF-GB
static std::chrono::steady_clock::time_point s_t0{};
} // namespace gwimpl
extern "C" void ps2_gw_mark() // PROBE-GW: call once on the GsWorker thread
{
    using namespace gwimpl;
    t_mark = true;
    s_last = s_tsc0 = __rdtsc();
    s_t0 = std::chrono::steady_clock::now();
    s_cur = cOther;
}
extern "C" int ps2_gw_sw(int cat) // PROBE-GW: returns previous category, -1 if not the GsWorker thread
{
    using namespace gwimpl;
    if (!t_mark)
        return -1;
    const uint64_t now = __rdtsc();
    s_T[s_cur] += now - s_last;
    s_last = now;
    const int prev = s_cur;
    s_cur = cat;
    return prev;
}
extern "C" void ps2_gw_count(int what, uint64_t n) // PROBE-GW: 0 prims, 1 image bytes, 2 runs, 3 state-serial breaks, 4 redundant state breaks
{
    using namespace gwimpl;
    if (!t_mark)
        return;
    switch (what) { case 0: s_prims += n; break; case 1: s_imgBytes += n; break; case 2: s_runs += n; break; case 3: s_brk += n; break; default: s_redund += n; break; } // PERF-GB
}
namespace gwimpl { static uint64_t s_iuT[14] = {}, s_iuB[14] = {}, s_iuX = 0, s_iuXB = 0, s_iuGT = 0, s_iuGB = 0; } // PROBE-IU
extern "C" void ps2_iu_rec(int kind, uint64_t bytes, uint64_t ticks) // PROBE-IU: kind 0 CT32 1 CT24 2 CT16 3 CT16S 4 T8 5 T8H 6 T4 7 T4HL 8 T4HH 9-12 Z 13 other
{
    using namespace gwimpl;
    if (!t_mark) return;
    if (kind < 0 || kind > 13) kind = 13;
    s_iuT[kind] += ticks; s_iuB[kind] += bytes;
}
extern "C" void ps2_iu_gpu(uint64_t bytes, uint64_t ticks) // PROBE-IU: GPU page upload (ensureUpload)
{
    using namespace gwimpl;
    if (!t_mark) return;
    s_iuGT += ticks; s_iuGB += bytes;
}
extern "C" void ps2_iu_xfer(uint64_t bytes) // PROBE-IU
{
    using namespace gwimpl;
    if (!t_mark) return;
    ++s_iuX; s_iuXB += bytes;
}
extern "C" void ps2_gw_tick() // PROBE-GW: call from the GsWorker loop (outside the idle wait); prints about every 5 s
{
    using namespace gwimpl;
    if (!t_mark)
        return;
    const auto nowT = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(nowT - s_t0).count();
    if (ms < 5000.0)
        return;
    const uint64_t now = __rdtsc();
    s_T[s_cur] += now - s_last;
    s_last = now;
    const double tpm = static_cast<double>(now - s_tsc0) / ms; // ticks per ms
    const uint64_t fr = g_gwFrames.load(std::memory_order_relaxed), pt = g_gwPresentT.load(std::memory_order_relaxed);
    const uint64_t frames = fr - s_f0;
    const double f = frames ? static_cast<double>(frames) : 1.0;
    auto pf = [&](uint64_t t) { return static_cast<double>(t) / tpm / f; };
    const uint64_t idle = s_T[cIdle];
    uint64_t tot = 0;
    for (int i = 0; i < cN; ++i)
        tot += s_T[i];
    const double presentPf = static_cast<double>(pt - s_p0) / tpm / f; // present latch wall time on whatever thread ran it
    ::ps2xSafeLog(
                 "[gw] frames=%llu ms/frame: busy=%.2f idle=%.2f | regs=%.2f kicks=%.2f(%.0f prims/frame) image=%.2f(%.1f KB/frame) primbuild=%.2f statechg=%.2f(%.0f runs/frame brk=%.0f redund=%.0f) vkrec=%.2f submit=%.2f sync=%.2f present=%.2f(latch %.2f) other=%.2f\n",
                 static_cast<unsigned long long>(frames), pf(tot - idle), pf(idle), pf(s_T[cRegs]), pf(s_T[cKick]), static_cast<double>(s_prims) / f,
                 pf(s_T[cImage]), static_cast<double>(s_imgBytes) / 1024.0 / f, pf(s_T[cPrim]), pf(s_T[cState]), static_cast<double>(s_runs) / f,
                 static_cast<double>(s_brk) / f, static_cast<double>(s_redund) / f, pf(s_T[cVkrec]), pf(s_T[cSubmit]), pf(s_T[cSync]), pf(s_T[cPresent]), presentPf, pf(s_T[cOther]));
    { // PROBE-IU
        auto ms1 = [&](int k) { return pf(s_iuT[k]); };
        auto kb = [&](int k) { return static_cast<double>(s_iuB[k]) / 1024.0 / f; };
        const double zT = pf(s_iuT[9] + s_iuT[10] + s_iuT[11] + s_iuT[12]), zB = (static_cast<double>(s_iuB[9] + s_iuB[10] + s_iuB[11] + s_iuB[12])) / 1024.0 / f;
        ::ps2xSafeLog("[iu] ms/frame: swz CT32=%.2f(%.0fKB) CT24=%.2f(%.0fKB) CT16=%.2f(%.0fKB) CT16S=%.2f(%.0fKB) T8=%.2f(%.0fKB) T4=%.2f(%.0fKB) T8H=%.2f(%.0fKB) T4HL/HH=%.2f(%.0fKB) Z=%.2f(%.0fKB) other=%.2f(%.0fKB) | xfers/frame=%.1f avg=%lluB | gpuup=%.2fms (%.0fKB/frame)\n",
                      ms1(0), kb(0), ms1(1), kb(1), ms1(2), kb(2), ms1(3), kb(3), ms1(4), kb(4), ms1(6), kb(6), ms1(5), kb(5),
                      ms1(7) + ms1(8), kb(7) + kb(8), zT, zB, ms1(13), kb(13), static_cast<double>(s_iuX) / f,
                      static_cast<unsigned long long>(s_iuX ? s_iuXB / s_iuX : 0ull), pf(s_iuGT), static_cast<double>(s_iuGB) / 1024.0 / f);
        for (int i = 0; i < 14; ++i) s_iuT[i] = s_iuB[i] = 0;
        s_iuX = s_iuXB = s_iuGT = s_iuGB = 0;
    }
    for (int i = 0; i < cN; ++i)
        s_T[i] = 0;
    s_prims =s_imgBytes = s_runs = s_brk = s_redund = 0;
    s_f0 = fr;
    s_p0 = pt;
    s_t0 = nowT;
    s_tsc0 = now;
}
namespace
{
struct GwScope // PROBE-GW
{
    int p;
    explicit GwScope(int c) : p(g_gwOn ? ps2_gw_sw(c) : -1) {}
    ~GwScope() { if (p >= 0) ps2_gw_sw(p); }
};
} // namespace

std::atomic<uint64_t> g_hostPresentSeq{0};
// PROBE-GT: present-path accounting (0=skip_nokick, 1=coalesce_latch, 2=skip_empty, 3=coalesce_main, 4=latched)
extern "C" bool ps2_gt_enabled();
static std::atomic<uint64_t> g_gtCounts[5]{};
extern "C" void ps2_gt_count(int what, uint64_t n)
{
    if (what >= 0 && what < 5 && ps2_gt_enabled())
        g_gtCounts[what].fetch_add(n, std::memory_order_relaxed);
}
extern "C" void ps2_gt_take_counts(uint64_t *out)
{
    for (int i = 0; i < 5; ++i)
        out[i] = g_gtCounts[i].exchange(0, std::memory_order_relaxed);
}
// PERF-JD: rdtsc ticks per stage of the vblank present latch: 0 total, 1 Flush+Sync, 2 backend Present, 3 Vk syncPages (in Present), 4 VRAM snapshot, 5 CPU de-swizzle, 7 count
std::atomic<uint64_t> g_presTicks[8]{};

// PERF-LN (PS2X_FT2=1): frame-time metric measured where the frame is produced + hitch dump of what the late frame waited for.
extern bool g_ps2Ft2On; extern "C" void ps2_ft2_ev(uint32_t type, uint64_t arg); // PROBE-FT2
extern "C" uint64_t ps2x_ft2_waitTicks(int idx); // ps2_memory.cpp (g_waitTicks[idx], non-resetting)
extern "C" uint64_t ps2x_ft2_gsBusy();           // gs/ps2_gif_arbiter.cpp (g_gsqBusy, non-resetting)
extern "C" uint64_t ps2x_ft2_presentHold();      // gs/ps2_vk_gs_backend.cpp (g_gqPresentHold, non-resetting)
void ps2xSafeLog(const char *fmt, ...);
static void ft2OnPresent()
{
    static const bool s_on = []() { const char *e = std::getenv("PS2X_FT2"); return e && e[0] == '1'; }();
    if (!s_on)
        return;
    using clk = std::chrono::steady_clock;
    static double s_tpm = 0.0;
    static clk::time_point s_last{};
    static clk::time_point s_winStart{};
    static bool s_have = false;
    static std::vector<double> s_dts;
    static uint32_t s_over20 = 0, s_over25 = 0, s_hitches = 0;
    static uint64_t s_prev[5]{}; // fence ticks, vif busy, gs busy, present hold, fence count
    if (s_tpm == 0.0)
    {
        const auto c0 = clk::now();
        const uint64_t t0 = __rdtsc();
        while (std::chrono::duration<double, std::milli>(clk::now() - c0).count() < 100.0) {}
        const uint64_t t1 = __rdtsc();
        s_tpm = static_cast<double>(t1 - t0) / std::chrono::duration<double, std::milli>(clk::now() - c0).count();
        if (s_tpm <= 0.0) s_tpm = 1.0;
    }
    const auto now = clk::now();
    const uint64_t cur[5] = {ps2x_ft2_waitTicks(0), ps2x_ft2_waitTicks(4), ps2x_ft2_gsBusy(), ps2x_ft2_presentHold(), ps2x_ft2_waitTicks(5)};
    double d[5];
    for (int i = 0; i < 5; ++i)
    {
        d[i] = static_cast<double>(cur[i] >= s_prev[i] ? cur[i] - s_prev[i] : cur[i]); // other reporters may reset the shared counters
        s_prev[i] = cur[i];
    }
    if (!s_have)
    {
        s_have = true;
        s_last = now;
        s_winStart = now;
        return;
    }
    const double dt = std::chrono::duration<double, std::milli>(now - s_last).count();
    s_last = now;
    s_dts.push_back(dt);
    if (dt > 20.0) ++s_over20;
    if (dt > 25.0) ++s_over25;
    if (dt > 22.0 && s_hitches < 400u)
    {
        ++s_hitches;
        ::ps2xSafeLog("[hitch] dt=%.1f ee_wait=%.1f vif_busy=%.1f gs_busy=%.1f present_hold=%.1f fence_n=%llu\n",
                      dt, d[0] / s_tpm, d[1] / s_tpm, d[2] / s_tpm, d[3] / s_tpm, static_cast<unsigned long long>(d[4]));
        std::fflush(stderr);
    }
    if (std::chrono::duration<double, std::milli>(now - s_winStart).count() >= 5000.0 && !s_dts.empty())
    {
        std::vector<double> v = s_dts;
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
        ::ps2xSafeLog("[ft2] n=%zu p50=%.2f p95=%.2f p99=%.2f max=%.2f ms | over20=%u over25=%u\n",
                      v.size(), pct(0.50), pct(0.95), pct(0.99), v.back(), s_over20, s_over25);
        std::fflush(stderr);
        s_dts.clear();
        s_over20 = s_over25 = 0;
        s_winStart = now;
    }
}
namespace
{
    static inline bool isVerboseLog()
    {
        static int s_verbose = -1;
        if (s_verbose == -1)
        {
            const char *v = std::getenv("PS2X_VERBOSE_LOG");
            s_verbose = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
        }
        return s_verbose == 1;
    }

    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    static inline bool isSkipDisplayCopyEnabled()
    {
        static int s_val = -1;
        if (s_val == -1)
        {
            const char *env = std::getenv("PS2X_GS_SKIP_DISPLAY_COPY");
            s_val = (env && (env[0] == '1' || env[0] == 't' || env[0] == 'T')) ? 1 : 0;
        }
        return s_val == 1;
    }

    static std::atomic<uint64_t> s_coRecognised{0};
    static std::atomic<uint64_t> s_coSkipped{0};
    static std::atomic<uint64_t> s_coFlushed{0};
    static std::atomic<uint32_t> s_coAlphaLogged{0};
    static auto s_coLastSecLog = std::chrono::steady_clock::now();

    static bool s_hasDeferredDisplayCopy = false;
    static GSPrimitiveBatch s_deferredDisplayCopyBatch{};

    static inline void checkPeriodicCoLog()
    {
        static uint32_t s_coCalls = 0; // PERF-GD: one clock read per sprite draw -> one per 256 (the log is per second anyway)
        if ((++s_coCalls & 0xFFu) != 0u)
            return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_coLastSecLog).count() >= 1)
        {
            s_coLastSecLog = now;
            s_coAlphaLogged.store(0, std::memory_order_relaxed);
            probe_ae::bhLog("[probe] co disp recognised=%llu skipped=%llu flushed=%llu\n",
                            (unsigned long long)s_coRecognised.load(std::memory_order_relaxed),
                            (unsigned long long)s_coSkipped.load(std::memory_order_relaxed),
                            (unsigned long long)s_coFlushed.load(std::memory_order_relaxed));
        }
    }

    static inline void flushDeferredDisplayCopy(GSRasterBackend *backend)
    {
        if (s_hasDeferredDisplayCopy && backend)
        {
            s_hasDeferredDisplayCopy = false;
            s_coFlushed.fetch_add(1u, std::memory_order_relaxed);
            g_rdCurDrawIsGeom = false; // FEAT-RD18
            backend->Submit(s_deferredDisplayCopyBatch);
        }
        else
        {
            s_hasDeferredDisplayCopy = false;
        }
    }

    static inline bool frameOverlaps(uint32_t fbpA, uint32_t fbwA, uint32_t fbpB, uint32_t fbwB)
    {
        uint32_t startA = fbpA;
        uint32_t endA = fbpA + std::max(fbwA, 1u) * 32u;
        uint32_t startB = fbpB;
        uint32_t endB = fbpB + std::max(fbwB, 1u) * 32u;
        return (startA < endB) && (startB < endA);
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};

    // Task LW: lean per-triangle path (PS2X_GS_LEAN=1, PS2X_GS_LEAN_VERIFY=1)
    static inline bool isGsLeanEnabled()
    {
        static const bool s_on = []() {
            const char *e = std::getenv("PS2X_GS_LEAN");
            const bool on = !(e && e[0] == '0');
            if (on)
            {
                ::ps2xSafeLog("[gslean] ON\n");
                std::fflush(stderr);
            }
            return on;
        }();
        return s_on;
    }

    static inline bool isGsLeanVerifyEnabled()
    {
        static const bool s_verify = []() {
            if (!isGsLeanEnabled())
                return false;
            const char *e = std::getenv("PS2X_GS_LEAN_VERIFY");
            return (e && e[0] == '1');
        }();
        return s_verify;
    }

    struct LeanVerifyKick
    {
        bool drawing;
        GSVertex vtx;
    };
    static std::vector<LeanVerifyKick> *s_leanVerifyRecord = nullptr;

    // PROBE-INTERP: per-draw log for the frame-interpolation feasibility study (PS2X_GS_INTERP_PROBE=1, file logs/interp_probe.txt)
    static FILE *s_ip = nullptr;
    static const bool s_interpProbeActive = []() -> bool {
        const char *e = std::getenv("PS2X_GS_INTERP_PROBE");
        if (e && e[0] == '1')
        {
            s_ip = std::fopen("logs/interp_probe.txt", "w");
            return s_ip != nullptr;
        }
        return false;
    }();

    // Task LV: GIF packet shape statistics (PS2X_GIFSTAT=1)
    static const bool s_gifStatOn = []() {
        const char *e = std::getenv("PS2X_GIFSTAT");
        return e && e[0] == '1';
    }();

    static uint64_t s_gifStatRegCounts[256]{};

    struct GifShapeKey
    {
        uint64_t regs;
        uint32_t nloop;
        uint16_t prim;
        uint8_t flg;
        uint8_t nreg;
        uint8_t pre;
        uint8_t eop;

        bool operator==(const GifShapeKey &o) const
        {
            return regs == o.regs &&
                   nloop == o.nloop &&
                   prim == o.prim &&
                   flg == o.flg &&
                   nreg == o.nreg &&
                   pre == o.pre &&
                   eop == o.eop;
        }
    };

    struct GifShapeKeyHash
    {
        size_t operator()(const GifShapeKey &k) const
        {
            uint64_t h = k.regs;
            h ^= (static_cast<uint64_t>(k.nloop) << 32) |
                 (static_cast<uint64_t>(k.prim) << 16) |
                 (static_cast<uint64_t>(k.flg) << 10) |
                 (static_cast<uint64_t>(k.nreg) << 4) |
                 (static_cast<uint64_t>(k.pre) << 1) |
                 (static_cast<uint64_t>(k.eop));
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ull;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    struct GifShapeVal
    {
        uint64_t count = 0;
        uint64_t qwords = 0;
        uint64_t ticks = 0;
    };

    static void recordGifStatTag(uint64_t tagLo, uint64_t tagHi,
                                 uint32_t nloop, uint8_t flg, uint32_t nreg,
                                 bool pre, uint32_t qwords, uint64_t ticks,
                                 uint64_t curTsc)
    {
        static std::unordered_map<GifShapeKey, GifShapeVal, GifShapeKeyHash> s_shapes;
        static uint64_t s_totalTags = 0;
        static uint64_t s_totalQwords = 0;
        static uint64_t s_totalTicks = 0;
        static auto s_winStart = std::chrono::steady_clock::now();
        static uint64_t s_winStartTsc = curTsc;
        static uint64_t s_lastCheckTsc = curTsc;
        static bool s_initialized = false;

        if (!s_initialized)
        {
            s_winStart = std::chrono::steady_clock::now();
            s_winStartTsc = curTsc;
            s_lastCheckTsc = curTsc;
            s_initialized = true;
        }

        const uint8_t eop = static_cast<uint8_t>((tagLo >> 15) & 1);
        const uint16_t prim = pre ? static_cast<uint16_t>((tagLo >> 47) & 0x7FF) : 0u;

        GifShapeKey key;
        key.regs = tagHi;
        key.nloop = nloop;
        key.prim = prim;
        key.flg = flg;
        key.nreg = static_cast<uint8_t>(nreg);
        key.pre = pre ? 1u : 0u;
        key.eop = eop;

        auto &val = s_shapes[key];
        val.count++;
        val.qwords += qwords;
        val.ticks += ticks;

        s_totalTags++;
        s_totalQwords += qwords;
        s_totalTicks += ticks;

        if (curTsc > s_lastCheckTsc && curTsc - s_lastCheckTsc >= 10000000ull)
        {
            s_lastCheckTsc = curTsc;
            const auto now = std::chrono::steady_clock::now();
            const double winMs = std::chrono::duration<double, std::milli>(now - s_winStart).count();
            if (winMs >= 5000.0)
            {
                const double sec = winMs / 1000.0;
                const double ticksPerMs = winMs > 0.0 ? static_cast<double>(curTsc - s_winStartTsc) / winMs : 1.0;
                const double msInTags = ticksPerMs > 0.0 ? (static_cast<double>(s_totalTicks) / ticksPerMs) / sec : 0.0;

                ::ps2xSafeLog("[gifstat] window_ms=%.0f tags/s=%.0f qwords/s=%.0f ms/s_in_tags=%.1f\n",
                              winMs,
                              static_cast<double>(s_totalTags) / sec,
                              static_cast<double>(s_totalQwords) / sec,
                              msInTags);

                std::vector<std::pair<GifShapeKey, GifShapeVal>> shapeList;
                shapeList.reserve(s_shapes.size());
                for (const auto &kv : s_shapes)
                {
                    shapeList.push_back(kv);
                }
                std::sort(shapeList.begin(), shapeList.end(), [](const auto &a, const auto &b) {
                    return a.second.ticks > b.second.ticks;
                });

                const size_t printShapeCount = std::min<size_t>(10, shapeList.size());
                for (size_t i = 0; i < printShapeCount; ++i)
                {
                    const auto &k = shapeList[i].first;
                    const auto &v = shapeList[i].second;
                    const double tagsPerSec = static_cast<double>(v.count) / sec;
                    const double qwPerTag = v.count > 0 ? static_cast<double>(v.qwords) / static_cast<double>(v.count) : 0.0;
                    const double nsPerTag = (v.count > 0 && ticksPerMs > 0.0) ? (static_cast<double>(v.ticks) / static_cast<double>(v.count) / ticksPerMs) * 1e6 : 0.0;
                    const double msPerSec = ticksPerMs > 0.0 ? (static_cast<double>(v.ticks) / ticksPerMs) / sec : 0.0;
                    ::ps2xSafeLog("[gifstat] flg=%u nloop=%u nreg=%u regs=%016llX pre=%u prim=%X eop=%u | tags/s=%.0f qw/tag=%.1f ns/tag=%.0f ms/s=%.1f\n",
                                  static_cast<unsigned>(k.flg),
                                  static_cast<unsigned>(k.nloop),
                                  static_cast<unsigned>(k.nreg),
                                  static_cast<unsigned long long>(k.regs),
                                  static_cast<unsigned>(k.pre),
                                  static_cast<unsigned>(k.prim),
                                  static_cast<unsigned>(k.eop),
                                  tagsPerSec, qwPerTag, nsPerTag, msPerSec);
                }

                struct RegStat
                {
                    uint8_t id;
                    uint64_t count;
                };
                std::vector<RegStat> regList;
                regList.reserve(256);
                for (uint32_t r = 0; r < 256; ++r)
                {
                    if (s_gifStatRegCounts[r] > 0)
                    {
                        regList.push_back({static_cast<uint8_t>(r), s_gifStatRegCounts[r]});
                    }
                }
                std::sort(regList.begin(), regList.end(), [](const RegStat &a, const RegStat &b) {
                    return a.count > b.count;
                });

                const size_t printRegCount = std::min<size_t>(10, regList.size());
                for (size_t i = 0; i < printRegCount; ++i)
                {
                    const double nPerSec = static_cast<double>(regList[i].count) / sec;
                    ::ps2xSafeLog("[gifstat] reg=%02X n/s=%.0f\n",
                                  static_cast<unsigned>(regList[i].id),
                                  nPerSec);
                }

                std::fflush(stderr);

                s_shapes.clear();
                std::memset(s_gifStatRegCounts, 0, sizeof(s_gifStatRegCounts));
                s_totalTags = 0;
                s_totalQwords = 0;
                s_totalTicks = 0;
                s_winStart = now;
                s_winStartTsc = curTsc;
            }
        }
    }
}


std::unique_ptr<GSRasterBackend> ps2x_vk_make_backend(); // PERF-VK (ps2_vk_gs_backend.cpp)

static std::unique_ptr<GSRasterBackend> makeDefaultRasterBackend()
{
    const char *v = std::getenv("PS2X_GS_VULKAN");
    if (v && v[0] == '1')
        return ps2x_vk_make_backend();
    return std::make_unique<GSCpuBackend>();
}

GS::GS()
    : m_backend(makeDefaultRasterBackend())
{
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = makeDefaultRasterBackend();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    flushDeferredOrdered();
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_stateDirty = true; // PERF-GR
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        (flushPendingDraws(), m_backend.get())->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
        m_stateDirty = true;
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    flushDeferredDisplayCopy(m_backend.get()); // outside m_stateMutex: no pending run here
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

// PERF-GD: the GS debug history (debug panel, GS tab) was recorded for every GIF tag, register write and draw, each time taking the backend mutex
// (GetTransferSnapshot) and copying a ~300 byte entry twice: ~100-200 ns per draw on the GS thread. It does not influence emulation. Opt in with
// PS2X_GS_DEBUG_HISTORY=1 when the panel's history is wanted.
static inline bool gsDebugHistoryEnabled()
{
    static const bool on = []() { const char *e = std::getenv("PS2X_GS_DEBUG_HISTORY"); return e && e[0] == '1'; }();
    return on;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused || !gsDebugHistoryEnabled())
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

void GS::latchHostPresentationFrame()
{
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            ps2_gt_count(2, 1); // PROBE-GT: skip_empty
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    const uint64_t pjStart = __rdtsc(); // PERF-JD
    struct GwPres { uint64_t t0; ~GwPres() { if (g_gwOn) { g_gwPresentT.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); g_gwFrames.fetch_add(1, std::memory_order_relaxed); } } } gwPres{pjStart}; // PROBE-GW
    const bool takeover = ps2xRdTakeoverOn(); // FEAT-RD12
    const bool frameHasGeom = s_rdFrameHasGeom.exchange(false, std::memory_order_acq_rel); // FEAT-RD18
    rdRollPassFrame(); // FEAT-RD23
    static uint32_t s_rdEmptyLatches = 0u; // FEAT-RD33c
    if (frameHasGeom) s_rdEmptyLatches = 0u; else if (s_rdEmptyLatches < 1000000u) ++s_rdEmptyLatches; // FEAT-RD33c
    const uint32_t latchPrims = s_rdLatchPrims.exchange(0u, std::memory_order_acq_rel); // FEAT-RD50
    static uint32_t s_rdLowPrimLatches = 0u; // FEAT-RD50
    if (latchPrims < 64u) { if (s_rdLowPrimLatches < 1000000u) ++s_rdLowPrimLatches; } // FEAT-RD50
    else s_rdLowPrimLatches = 0u; // FEAT-RD50
    static uint32_t s_rdResetCooldown = 0u; // FEAT-RD50
    if (s_rdResetCooldown > 0u) --s_rdResetCooldown; // FEAT-RD50
    static double s_rdRtSyncEma = 0.0;       // FEAT-RD48
    static uint32_t s_rdRtSyncHighCnt = 0u;  // FEAT-RD48
    static uint32_t s_rdForceWarmup = 0u;    // FEAT-RD48
    static double s_rdSyncCpm = 0.0;         // FEAT-RD48
    static uint64_t s_rdSyncTsc0 = 0ull;     // FEAT-RD48
    static std::chrono::steady_clock::time_point s_rdSyncT0; // FEAT-RD48
    static bool s_rdSyncInit = false;        // FEAT-RD48
    const auto rdSyncNow = std::chrono::steady_clock::now(); // FEAT-RD48
    if (!s_rdSyncInit) { s_rdSyncInit = true; s_rdSyncT0 = rdSyncNow; s_rdSyncTsc0 = pjStart; } // FEAT-RD48
    else {
        const double dms = std::chrono::duration<double, std::milli>(rdSyncNow - s_rdSyncT0).count();
        if (dms >= 50.0) { s_rdSyncCpm = static_cast<double>(pjStart - s_rdSyncTsc0) / dms; s_rdSyncT0 = rdSyncNow; s_rdSyncTsc0 = pjStart; }
    }
    const uint64_t frameSyncCyc = g_rdFrameRtSyncCyc.exchange(0ull, std::memory_order_acq_rel); // FEAT-RD48
    const double rtSyncMs = (s_rdSyncCpm > 0.0) ? (static_cast<double>(frameSyncCyc) / s_rdSyncCpm) : 0.0; // FEAT-RD48
    const bool wasForced = g_rdForceClassic.load(std::memory_order_acquire); // FEAT-RD49 // FEAT-RD50
    const bool doStageReset = wasForced ? (s_rdLowPrimLatches >= 8u) // FEAT-RD50: never use s_rdEmptyLatches while forced
                                        : (s_rdEmptyLatches >= 8u && (s_rdRtSyncEma != 0.0 || s_rdRtSyncHighCnt != 0u)); // FEAT-RD48
    if (doStageReset) // FEAT-RD48 // FEAT-RD50: stage-transition reset
    {
        g_rdForceClassic.store(false, std::memory_order_release);
        s_rdRtSyncEma = 0.0;
        s_rdRtSyncHighCnt = 0u;
        s_rdForceWarmup = 0u;
        if (wasForced) // FEAT-RD49 // FEAT-RD50: clear stale per-frame & rd_view state before resuming
        {
            s_rdLowPrimLatches = 0u;
            s_rdResetCooldown = 120u;
            {
                std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
                s_rd2dVerts.clear();
                s_rd2dBatches.clear();
                s_rdKickRecs.clear();
                s_rdLastKickRecSerial = 0u;
                s_rdCurSerial = 0u;
                s_rdLastNotedSerial = 0u;
                s_rdLastStateSerial = 0u;
                ps2xRdResetState();
            }
            ps2xSafeLog("[rd] forceClassic OFF (stage transition, prims/latch=%u)\n", latchPrims);
        }
    }
    const bool forcedNow = ps2xRdForceClassic(); // FEAT-RD48
    const bool useClassicForced = forcedNow && (s_rdForceWarmup == 0u); // FEAT-RD48
    if (s_rdForceWarmup > 0u) --s_rdForceWarmup; // FEAT-RD48
    const bool useRepeat = takeover && !useClassicForced && !frameHasGeom && s_rdEmptyLatches < 8u; // FEAT-RD33c // FEAT-RD48
    const bool useClassic = takeover && (useClassicForced || (!frameHasGeom && !useRepeat)); // FEAT-RD18 // FEAT-RD33c // FEAT-RD48
    if (takeover && rdForceClassicMode() == 2 && !forcedNow) // FEAT-RD48
    {
        s_rdRtSyncEma = 0.9 * s_rdRtSyncEma + 0.1 * rtSyncMs;
        if (s_rdRtSyncEma > 6.0) ++s_rdRtSyncHighCnt; else s_rdRtSyncHighCnt = 0u;
        if (s_rdRtSyncHighCnt >= 30u && s_rdResetCooldown == 0u) // FEAT-RD50
        {
            g_rdForceClassic.store(true, std::memory_order_release);
            s_rdForceWarmup = 1u; // FEAT-RD48: wait 1 latch for in-flight kicks to finish full raster
            {
                std::lock_guard<std::mutex> capLock(s_rd2dCapMutex); // FEAT-RD49
                s_rd2dVerts.clear();
                s_rd2dBatches.clear();
                s_rdKickRecs.clear();
                s_rdLastKickRecSerial = 0u;
                s_rdCurSerial = 0u;
                s_rdLastNotedSerial = 0u;
                s_rdLastStateSerial = 0u;
                ps2xRdResetState();
            }
            ps2xSafeLog("[rd] forceClassic ON (rtSync ema=%.1f ms)\n", s_rdRtSyncEma);
        }
    }
    if (useRepeat) g_rdRepeatFrames.fetch_add(1, std::memory_order_relaxed); // FEAT-RD33c
    if (useClassic) g_rdClassicFramesTotal.fetch_add(1, std::memory_order_relaxed); // FEAT-RD33c
    const uint32_t frameGeomFbp = s_rdFrameGeomFbp.exchange(0xFFFFFFFFu, std::memory_order_acq_rel); // FEAT-RD18b
    if (!takeover || useClassic) // FEAT-RD18
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            const uint64_t pjMid = __rdtsc();
            g_rdClassicPresentRequested = useClassic; // FEAT-RD18
            frame = m_backend->Present(request);
            g_rdClassicPresentRequested = false; // FEAT-RD18
            g_presTicks[1].fetch_add(pjMid - pjStart, std::memory_order_relaxed);
            g_presTicks[2].fetch_add(__rdtsc() - pjMid, std::memory_order_relaxed);
        }
    }

    bool hasFrame = static_cast<bool>(frame);
    uint32_t displayFbp = hasFrame ? frame.displayFbp : 0xFFFFFFFFu; // FEAT-RD18
    const uint32_t sourceFbp = frame.sourceFbp;
    uint32_t width = frame.width;
    uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    if (takeover && !useClassic) // FEAT-RD12 // FEAT-RD18: compute display size & FBP from DISPLAY1/2 without GPU/CPU readback
    {
        auto circuitValid = [](uint64_t disp, uint64_t fb) -> bool {
            const uint32_t dw = static_cast<uint32_t>((disp >> 32) & 0x0FFFu);
            const uint32_t dh = static_cast<uint32_t>((disp >> 44) & 0x07FFu);
            const uint32_t magh = static_cast<uint32_t>((disp >> 23) & 0x0Fu);
            return ((fb >> 9) & 0x3Fu) != 0u || dw != 0u || dh != 0u || magh != 0u;
        };
        const bool v1 = (request.pmode & 1ull) != 0ull && circuitValid(request.display1, request.dispfb1);
        const bool v2 = (request.pmode & 2ull) != 0ull && circuitValid(request.display2, request.dispfb2);
        if (v1)
            displayFbp = static_cast<uint32_t>(request.dispfb1 & 0x1FFu);
        else if (v2)
            displayFbp = static_cast<uint32_t>(request.dispfb2 & 0x1FFu);
        if (v1 || v2)
        {
            const uint64_t d = v1 ? request.display1 : request.display2;
            const uint32_t dw = static_cast<uint32_t>((d >> 32) & 0x0FFFu);
            const uint32_t dh = static_cast<uint32_t>((d >> 44) & 0x07FFu);
            const uint32_t magh = static_cast<uint32_t>((d >> 23) & 0x0Fu);
            width = (dw + 1u) / (magh + 1u);
            height = dh + 1u;
            if (height > 448u && height <= 896u)
                height = (height + 1u) / 2u;
            if (width < 64u || height < 64u)
            {
                width = 640u;
                height = 448u;
            }
            width = std::min<uint32_t>(width, 640u);
            height = std::min<uint32_t>(height, 512u);
        }
        else
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            width = (m_hostPresentationWidth != 0u) ? m_hostPresentationWidth : 640u;
            height = (m_hostPresentationHeight != 0u) ? m_hostPresentationHeight : 448u;
        }
        ps2xRdSetPresentSize(width, height);
        hasFrame = true;
    }
    else if (useClassic && hasFrame) // FEAT-RD18
    {
        ps2xRdSetPresentSize(width, height);
    }
    g_rdFrameIsClassic.store(useClassic, std::memory_order_release); // FEAT-RD18
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        if (displayFbp != 0xFFFFFFFFu)
            m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp != 0xFFFFFFFFu ? displayFbp : 0u, sourceFbp, width, height, usedPreferred);
        g_hostPresentSeq.fetch_add(1, std::memory_order_release);
        if (ps2xRdActive() && !useRepeat) // FEAT-RD8 // FEAT-RD18 // FEAT-RD33c // FEAT-RD49: repeat frames publish nothing (previous composition + 2D list redrawn)
        {
            static std::vector<RdVert> s_pubVerts;
            static std::vector<Rd2DBatch> s_pubBatches;
            static std::vector<RdKickRec> s_pubKickRecs; // FEAT-RD33
            static std::vector<uint32_t> s_pubSerials;   // FEAT-RD33
            static std::vector<uint32_t> s_pubRanges;    // FEAT-RD33
            uint32_t rtSk = 0u, plSk = 0u;
            uint32_t gMin = 0u, gMax = 0u; // FEAT-RD13
            {
                std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
                s_pubVerts.swap(s_rd2dVerts);
                s_pubBatches.swap(s_rd2dBatches);
                s_pubKickRecs.swap(s_rdKickRecs); // FEAT-RD33
                s_rdLastKickRecSerial = 0u;       // FEAT-RD33
                rtSk = s_rd2dRtSkipped;
                plSk = s_rd2dPointLineSkipped;
                gMin = s_rd2dGMin; // FEAT-RD13
                gMax = s_rd2dGMax; // FEAT-RD13
                s_rd2dRtSkipped = 0u;
                s_rd2dPointLineSkipped = 0u;
                s_rd2dGMin = 0u; // FEAT-RD13
                s_rd2dGMax = 0u; // FEAT-RD13
            }
            // FEAT-RD33: pick main pass of THIS frame = (display FBP, XYOFFSET) with most draws
            s_pubSerials.clear();
            s_pubRanges.clear();
            if (!s_pubKickRecs.empty())
            {
                const bool hasDisp = ps2xRdHasDisplayFbp();
                struct PassCnt { uint64_t key; uint32_t count; };
                std::vector<PassCnt> counts;
                counts.reserve(8);
                for (const RdKickRec &rec : s_pubKickRecs)
                {
                    if (hasDisp && !ps2xRdIsDisplayFbp(rec.fbp))
                        continue;
                    if (!rdScissorQualifies(rec.scW, rec.scH)) // FEAT-RD33b
                        continue;
                    const uint64_t k = rdPassKey(rec.fbp, rec.xyoff & 0xFFFFull, (rec.xyoff >> 32) & 0xFFFFull);
                    bool found = false;
                    for (PassCnt &pc : counts)
                    {
                        if (pc.key == k)
                        {
                            ++pc.count;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        counts.push_back({k, 1u});
                }
                uint64_t bestKey = ~0ull;
                uint32_t bestCnt = 0u;
                for (const PassCnt &pc : counts)
                {
                    if (pc.count > bestCnt)
                    {
                        bestCnt = pc.count;
                        bestKey = pc.key;
                    }
                }
                if (bestCnt > 0u)
                {
                    s_pubSerials.reserve(s_pubKickRecs.size());
                    for (const RdKickRec &rec : s_pubKickRecs)
                    {
                        if (hasDisp && !ps2xRdIsDisplayFbp(rec.fbp))
                            continue;
                        if (!rdScissorQualifies(rec.scW, rec.scH)) // FEAT-RD33b
                            continue;
                        const uint64_t k = rdPassKey(rec.fbp, rec.xyoff & 0xFFFFull, (rec.xyoff >> 32) & 0xFFFFull);
                        if (k == bestKey)
                            s_pubSerials.push_back(rec.serial);
                    }
                    std::sort(s_pubSerials.begin(), s_pubSerials.end());
                    s_pubSerials.erase(std::unique(s_pubSerials.begin(), s_pubSerials.end()), s_pubSerials.end());
                    for (uint32_t s : s_pubSerials)
                    {
                        if (s_pubRanges.empty() || s > s_pubRanges.back() + 1u)
                        {
                            s_pubRanges.push_back(s);
                            s_pubRanges.push_back(s);
                        }
                        else
                        {
                            s_pubRanges.back() = s;
                        }
                    }
                }
                s_pubKickRecs.clear();
            }
            const uint32_t filterFbp = (frameHasGeom && frameGeomFbp != 0xFFFFFFFFu) ? frameGeomFbp : ((displayFbp != 0xFFFFFFFFu) ? displayFbp : s_rdMainFbp); // FEAT-RD18b
            size_t keptVerts = 0u;
            size_t keptBatches = 0u;
            uint32_t keptAtFlip = 0u;
            uint32_t droppedAtFlip = 0u;
            for (size_t i = 0u; i < s_pubBatches.size(); ++i)
            {
                Rd2DBatch b = s_pubBatches[i];
                const uint32_t bFbp = static_cast<uint32_t>(b.frame & 0x1FFull);
                if (filterFbp != 0xFFFFFFFFu && bFbp == filterFbp)
                {
                    if (keptVerts != b.first && b.count > 0u)
                        std::memmove(&s_pubVerts[keptVerts], &s_pubVerts[b.first], b.count * sizeof(RdVert));
                    b.first = static_cast<uint32_t>(keptVerts);
                    keptVerts += b.count;
                    s_pubBatches[keptBatches++] = b;
                    ++keptAtFlip;
                }
                else
                {
                    ++droppedAtFlip;
                    ++rtSk;
                }
            }
            s_pubVerts.resize(keptVerts);
            s_pubBatches.resize(keptBatches);
            ps2xRdPublish2D(s_pubVerts.data(), s_pubVerts.size(), s_pubBatches.data(), s_pubBatches.size(), rtSk, plSk, gMin, gMax, s_pubRanges.data(), s_pubRanges.size() / 2u); // FEAT-RD13 // FEAT-RD33
            s_pubVerts.clear();
            s_pubBatches.clear();
            s_pubRanges.clear();

            static uint32_t s_rdClassicFrames = 0u;
            static uint32_t s_rdRdFrames = 0u;
            static uint64_t s_rd2dKeptAtFlip = 0ull;
            static uint64_t s_rd2dDroppedAtFlip = 0ull;
            if (useClassic)
                ++s_rdClassicFrames;
            else
                ++s_rdRdFrames;
            s_rd2dKeptAtFlip += keptAtFlip;
            s_rd2dDroppedAtFlip += droppedAtFlip;
            static auto s_rdFlipLogT = std::chrono::steady_clock::now();
            const auto flipNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(flipNow - s_rdFlipLogT).count() >= 5000)
            {
                s_rdFlipLogT = flipNow;
                const uint32_t totF = s_rdClassicFrames + s_rdRdFrames;
                const double fr = totF ? static_cast<double>(totF) : 1.0;
                const uint32_t fgQwords = static_cast<uint32_t>(s_rdFastGeomQwords.exchange(0ull, std::memory_order_relaxed)); // FEAT-RD38
                ps2xSafeLog("[rd] classicFrames=%u rdFrames=%u 2dKeptAtFlip=%.1f 2dDroppedAtFlip=%.1f fastGeomQwords=%u forceClassic=%d rtSyncEma=%.2f\n",
                            s_rdClassicFrames,
                            s_rdRdFrames,
                            static_cast<double>(s_rd2dKeptAtFlip) / fr,
                            static_cast<double>(s_rd2dDroppedAtFlip) / fr,
                            fgQwords,
                            forcedNow ? 1 : 0,
                            s_rdRtSyncEma); // FEAT-RD48
            }
        }
        ps2_gt_count(4, 1); // PROBE-GT: latched
        ft2OnPresent(); // PERF-LN
        if (g_ps2Ft2On) ps2_ft2_ev(12, g_hostPresentSeq.load(std::memory_order_relaxed)); // PROBE-FT2: GS_FRAME_DONE
    }
    else
    {
        ps2_gt_count(2, 1); // PROBE-GT: skip_empty
    }

    // PERF-JD: every 128 latches print the average ms per latch for each stage
    g_presTicks[0].fetch_add(__rdtsc() - pjStart, std::memory_order_relaxed);
    if ((g_presTicks[7].fetch_add(1, std::memory_order_relaxed) + 1u) % 128u == 0u)
    {
        static uint64_t s_prev[6]{};
        static uint64_t s_prevTsc = __rdtsc();
        static auto s_prevT = std::chrono::steady_clock::now();
        const uint64_t nowTsc = __rdtsc();
        const auto nowT = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(nowT - s_prevT).count();
        const double ticksPerMs = ms > 0.0 ? static_cast<double>(nowTsc - s_prevTsc) / ms : 1.0;
        double v[6];
        for (int i = 0; i < 6; ++i)
        {
            const uint64_t cur = g_presTicks[i].load(std::memory_order_relaxed);
            v[i] = static_cast<double>(cur - s_prev[i]) / ticksPerMs / 128.0;
            s_prev[i] = cur;
        }
        extern void ps2xSafeLog(const char *fmt, ...); // stderr is routed to NUL unless PS2X_VERBOSE_LOG=1
        ps2xSafeLog("[present] n=128 window_ms=%.0f per-latch ms: total=%.2f flushsync=%.2f present=%.2f vksync=%.2f snapshot=%.2f deswizzle=%.2f\n",
                     ms, v[0], v[1], v[2], v[3], v[4], v[5]);
        s_prevTsc = nowTsc;
        s_prevT = nowT;
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    // Task FV: capture ~3 game frames of GIF traffic into logs/ours.gs (PCSX2 GS-dump format) for replay in PCSX2.
    {
        extern std::atomic<uint64_t> g_gsFrameKicks;
        static const bool s_fvOn = []() { const char *v = std::getenv("PS2X_GSDUMP"); return v && v[0] == '1'; }();
        static std::vector<uint8_t> s_fvBuf;
        static bool s_fvDone = false;
        static uint64_t s_fvStartKick = 0;
        static bool s_fvStarted = false;
        static uint64_t s_fvLastKick = 0;
        static auto s_fvT0 = std::chrono::steady_clock::now();
        if (s_fvOn && !s_fvDone && data && sizeBytes >= 16)
        {
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_fvT0).count();
            const uint64_t kick = g_gsFrameKicks.load(std::memory_order_relaxed);
            if (!s_fvStarted && secs > 45.0)
            {
                s_fvStarted = true;
                s_fvStartKick = kick;
                s_fvLastKick = kick;
            }
            if (s_fvStarted)
            {
                if (kick != s_fvLastKick)
                {
                    s_fvBuf.push_back(1u); s_fvBuf.push_back(0u);  // VSync, field 0
                    s_fvLastKick = kick;
                }
                const uint8_t hdr[6] = {0u, 3u, static_cast<uint8_t>(sizeBytes), static_cast<uint8_t>(sizeBytes >> 8),
                                        static_cast<uint8_t>(sizeBytes >> 16), static_cast<uint8_t>(sizeBytes >> 24)};
                s_fvBuf.insert(s_fvBuf.end(), hdr, hdr + 6);
                s_fvBuf.insert(s_fvBuf.end(), data, data + sizeBytes);
                if (kick >= s_fvStartKick + 3u || s_fvBuf.size() > (48u << 20))
                {
                    s_fvBuf.push_back(1u); s_fvBuf.push_back(0u);
                    s_fvDone = true;
                    std::vector<uint8_t> prelude;
                    if (FILE *pf = std::fopen("pcsx2dumps/prelude.bin", "rb"))
                    {
                        std::fseek(pf, 0, SEEK_END); const long pl = std::ftell(pf); std::fseek(pf, 0, SEEK_SET);
                        prelude.resize(static_cast<size_t>(pl));
                        if (std::fread(prelude.data(), 1, prelude.size(), pf) != prelude.size()) prelude.clear();
                        std::fclose(pf);
                    }
                    if (FILE *of = std::fopen("logs/ours.gs", "wb"))
                    {
                        std::fwrite(prelude.data(), 1, prelude.size(), of);
                        std::fwrite(s_fvBuf.data(), 1, s_fvBuf.size(), of);
                        std::fclose(of);
                    }
                    std::printf("[probe] fv gsdump written prelude=%zu stream=%zu bytes kicks=%llu..%llu\n", prelude.size(), s_fvBuf.size(),
                                static_cast<unsigned long long>(s_fvStartKick), static_cast<unsigned long long>(kick));
                    std::fflush(stdout);
                }
            }
        }
    }
    const bool probeActive = probe_ae::isEnabled();
    std::chrono::steady_clock::time_point tLockWaitStart;
    if (probeActive) {
        tLockWaitStart = std::chrono::steady_clock::now();
    }

    std::unique_lock<std::recursive_mutex> lock(m_stateMutex);
    struct RunFlush { const GS *g; ~RunFlush() { g->flushPendingDraws(); } } runFlush{this}; // PERF-GR

    if (probeActive) {
        auto tLockAcquired = std::chrono::steady_clock::now();
        uint64_t waitUs = std::chrono::duration_cast<std::chrono::microseconds>(tLockAcquired - tLockWaitStart).count();
        if (waitUs > 0) {
            probe_ae::onBhGsBlockWait(waitUs);
        }
    }

    if (!data || sizeBytes < 16 || !m_backend)
        return;

    if (tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        if (nloop == 0u && (s_rdCurSerial & 0x40000000u) != 0u) // FEAT-RD42
        {
            rdNoteKickBookkeeping(m_ctx[m_prim.ctxt ? 1 : 0], m_prim, m_texa, m_localMemoryStorage);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        const uint32_t gifStatDataOffset = offset;
        const uint64_t gifStatTsc0 = s_gifStatOn ? __rdtsc() : 0ull;

        bool fastGeomEligible = false; // FEAT-RD38
        if (nloop > 0u && rdFastGeomOn() && (s_rdCurSerial & 0x40000000u) != 0u &&
            (flg == GIF_FMT_PACKED || flg == GIF_FMT_REGLIST))
        {
            const GSContext &fgCtx = m_ctx[m_prim.ctxt ? 1 : 0];
            const uint32_t fgScW = static_cast<uint32_t>(fgCtx.scissor.x1) + 1u - fgCtx.scissor.x0;
            const uint32_t fgScH = static_cast<uint32_t>(fgCtx.scissor.y1) + 1u - fgCtx.scissor.y0;
            const uint64_t fgXyoff = static_cast<uint64_t>(fgCtx.xyoffset.ofx) |
                                     (static_cast<uint64_t>(fgCtx.xyoffset.ofy) << 32);
            fastGeomEligible = (!ps2xRdHasDisplayFbp() || ps2xRdIsDisplayFbp(fgCtx.frame.fbp)) &&
                               rdScissorQualifies(fgScW, fgScH) &&
                               ps2xRdIsMainPass(fgCtx.frame.fbp, fgXyoff);
        }

        if (flg == GIF_FMT_PACKED)
        {
            if (fastGeomEligible) // FEAT-RD38
            {
                bool allVertexOnly = true;
                bool hasVertexKick = false;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint8_t reg = regs[r];
                    if (!rdIsPureVertexPackedReg(reg))
                        allVertexOnly = false;
                    if (reg == 0x04u || reg == 0x05u)
                        hasVertexKick = true;
                }
                if (allVertexOnly)
                {
                    const uint32_t qwords = nloop * nreg;
                    const uint32_t skipBytes = qwords * 16u;
                    if (offset + skipBytes > sizeBytes)
                        return;
                    offset += skipBytes;
                    s_rdFastGeomQwords.fetch_add(qwords, std::memory_order_relaxed);
                    if (hasVertexKick && (s_rdFastGeomLastSerial != s_rdCurSerial || m_stateDirty))
                    {
                        s_rdFastGeomLastSerial = s_rdCurSerial;
                        refreshDrawBatch(0);
                        queueDraw(m_drawBatch);
                    }
                }
                else
                {
                    uint64_t skippedQwords = 0ull;
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (offset + 16 > sizeBytes)
                                return;
                            const uint8_t reg = regs[r];
                            if (rdIsPureVertexPackedReg(reg))
                            {
                                offset += 16;
                                ++skippedQwords;
                            }
                            else
                            {
                                uint64_t lo = loadLE64(data + offset);
                                uint64_t hi = loadLE64(data + offset + 8);
                                offset += 16;
                                writeRegisterPacked(reg, lo, hi);
                            }
                        }
                    }
                    if (skippedQwords != 0ull)
                        s_rdFastGeomQwords.fetch_add(skippedQwords, std::memory_order_relaxed);
                    if (hasVertexKick && (s_rdFastGeomLastSerial != s_rdCurSerial || m_stateDirty))
                    {
                        s_rdFastGeomLastSerial = s_rdCurSerial;
                        refreshDrawBatch(0);
                        queueDraw(m_drawBatch);
                    }
                }
            }
            else
            {
            const bool isDominantShape = (nreg == 4u) &&
                (((tagHi & 0xFFFFull) == 0x512Aull) || ((tagHi & 0xFFFFull) == 0x412Aull));

            if (isGsLeanEnabled() && isDominantShape)
            {
                static uint64_t s_leanTagIndex = 0;
                static uint64_t s_leanVerifiedCount = 0;
                static uint64_t s_leanMismatchCount = 0;
                const uint64_t tagIdx = s_leanTagIndex++;
                const bool doVerify = isGsLeanVerifyEnabled() && ((tagIdx & 63u) == 0u);

                if (doVerify)
                {
                    const uint8_t initR = m_curR;
                    const uint8_t initG = m_curG;
                    const uint8_t initB = m_curB;
                    const uint8_t initA = m_curA;
                    const float initS = m_curS;
                    const float initT = m_curT;
                    const float initQ = m_curQ;
                    const uint16_t initU = m_curU;
                    const uint16_t initV = m_curV;
                    const uint8_t initFog = m_curFog;
                    const uint32_t tagPayloadOffset = offset;

                    std::vector<LeanVerifyKick> recordedGeneric;
                    recordedGeneric.reserve(nloop);
                    s_leanVerifyRecord = &recordedGeneric;

                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (offset + 16 > sizeBytes)
                            {
                                s_leanVerifyRecord = nullptr;
                                return;
                            }
                            uint64_t lo = loadLE64(data + offset);
                            uint64_t hi = loadLE64(data + offset + 8);
                            offset += 16;
                            writeRegisterPacked(regs[r], lo, hi);
                        }
                    }
                    s_leanVerifyRecord = nullptr;

                    const uint8_t genR = m_curR;
                    const uint8_t genG = m_curG;
                    const uint8_t genB = m_curB;
                    const uint8_t genA = m_curA;
                    const float genS = m_curS;
                    const float genT = m_curT;
                    const float genQ = m_curQ;
                    const uint16_t genU = m_curU;
                    const uint16_t genV = m_curV;
                    const uint8_t genFog = m_curFog;

                    uint8_t curR = initR;
                    uint8_t curG = initG;
                    uint8_t curB = initB;
                    uint8_t curA = initA;
                    float curS = initS;
                    float curT = initT;
                    float curQ = initQ;
                    uint16_t curU = initU;
                    uint16_t curV = initV;
                    uint8_t curFog = initFog;

                    std::vector<LeanVerifyKick> scratchKicks;
                    scratchKicks.reserve(nloop);

                    const bool isXYZF2 = ((tagHi >> 12u) & 0xFu) == 0x04u;
                    uint32_t verifyOff = tagPayloadOffset;
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        if (verifyOff + 64u > sizeBytes)
                            break;
                        const uint64_t lo0 = loadLE64(data + verifyOff);
                        const uint64_t hi0 = loadLE64(data + verifyOff + 8u);
                        const uint64_t lo1 = loadLE64(data + verifyOff + 16u);
                        const uint64_t hi1 = loadLE64(data + verifyOff + 24u);
                        const uint64_t lo2 = loadLE64(data + verifyOff + 32u);
                        const uint64_t hi2 = loadLE64(data + verifyOff + 40u);
                        const uint64_t lo3 = loadLE64(data + verifyOff + 48u);
                        const uint64_t hi3 = loadLE64(data + verifyOff + 56u);
                        verifyOff += 64u;

                        curFog = static_cast<uint8_t>((hi0 >> 36u) & 0xFFu);

                        const uint32_t sBits = static_cast<uint32_t>(lo1 & 0xFFFFFFFFu);
                        const uint32_t tBits = static_cast<uint32_t>((lo1 >> 32u) & 0xFFFFFFFFu);
                        const uint32_t qBits = static_cast<uint32_t>(hi1 & 0xFFFFFFFFu);
                        std::memcpy(&curS, &sBits, 4);
                        std::memcpy(&curT, &tBits, 4);
                        std::memcpy(&curQ, &qBits, 4);
                        if (curQ == 0.0f)
                            curQ = 1.0f;

                        curR = static_cast<uint8_t>(lo2 & 0xFFu);
                        curG = static_cast<uint8_t>((lo2 >> 32u) & 0xFFu);
                        curB = static_cast<uint8_t>(hi2 & 0xFFu);
                        curA = static_cast<uint8_t>((hi2 >> 32u) & 0xFFu);

                        const uint16_t x = static_cast<uint16_t>(lo3 & 0xFFFFu);
                        const uint16_t y = static_cast<uint16_t>((lo3 >> 32u) & 0xFFFFu);
                        const uint32_t z = isXYZF2 ? static_cast<uint32_t>((hi3 >> 4u) & 0xFFFFFFu)
                                                   : static_cast<uint32_t>(hi3 & 0xFFFFFFFFu);
                        const uint8_t f = isXYZF2 ? static_cast<uint8_t>((hi3 >> 36u) & 0xFFu)
                                                  : curFog;
                        const bool adk = ((hi3 >> 47u) & 1u) != 0u;

                        GSVertex vtx{};
                        vtx.x = static_cast<float>(x) / 16.0f;
                        vtx.y = static_cast<float>(y) / 16.0f;
                        vtx.z = static_cast<float>(z);
                        vtx.r = curR;
                        vtx.g = curG;
                        vtx.b = curB;
                        vtx.a = curA;
                        vtx.q = curQ;
                        vtx.s = curS;
                        vtx.t = curT;
                        vtx.u = curU;
                        vtx.v = curV;
                        vtx.fog = f;

                        scratchKicks.push_back({!adk, vtx});
                    }

                    bool mismatch = false;
                    if (scratchKicks.size() != recordedGeneric.size())
                    {
                        mismatch = true;
                    }
                    else
                    {
                        for (size_t i = 0; i < scratchKicks.size(); ++i)
                        {
                            const auto &sk = scratchKicks[i];
                            const auto &gk = recordedGeneric[i];
                            if (sk.drawing != gk.drawing ||
                                sk.vtx.x != gk.vtx.x ||
                                sk.vtx.y != gk.vtx.y ||
                                sk.vtx.z != gk.vtx.z ||
                                sk.vtx.r != gk.vtx.r ||
                                sk.vtx.g != gk.vtx.g ||
                                sk.vtx.b != gk.vtx.b ||
                                sk.vtx.a != gk.vtx.a ||
                                sk.vtx.q != gk.vtx.q ||
                                sk.vtx.s != gk.vtx.s ||
                                sk.vtx.t != gk.vtx.t ||
                                sk.vtx.u != gk.vtx.u ||
                                sk.vtx.v != gk.vtx.v ||
                                sk.vtx.fog != gk.vtx.fog)
                            {
                                mismatch = true;
                                break;
                            }
                        }
                    }

                    if (curR != genR || curG != genG || curB != genB || curA != genA ||
                        curS != genS || curT != genT || curQ != genQ ||
                        curU != genU || curV != genV || curFog != genFog)
                    {
                        mismatch = true;
                    }

                    if (mismatch)
                    {
                        s_leanMismatchCount++;
                        if (s_leanMismatchCount <= 20u)
                        {
                            ::ps2xSafeLog("[gslean] MISMATCH tag#%llu ...\n", static_cast<unsigned long long>(tagIdx));
                            std::fflush(stderr);
                        }
                    }
                    s_leanVerifiedCount++;
                    if ((s_leanVerifiedCount & 4095u) == 0u)
                    {
                        ::ps2xSafeLog("[gslean] verified=%llu mismatches=%llu\n",
                                      static_cast<unsigned long long>(s_leanVerifiedCount),
                                      static_cast<unsigned long long>(s_leanMismatchCount));
                        std::fflush(stderr);
                    }
                }
                else
                {
                    const bool isXYZF2 = ((tagHi >> 12u) & 0xFu) == 0x04u;
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        if (offset + 64u > sizeBytes)
                            return;
                        const uint64_t lo0 = loadLE64(data + offset);
                        const uint64_t hi0 = loadLE64(data + offset + 8u);
                        const uint64_t lo1 = loadLE64(data + offset + 16u);
                        const uint64_t hi1 = loadLE64(data + offset + 24u);
                        const uint64_t lo2 = loadLE64(data + offset + 32u);
                        const uint64_t hi2 = loadLE64(data + offset + 40u);
                        const uint64_t lo3 = loadLE64(data + offset + 48u);
                        const uint64_t hi3 = loadLE64(data + offset + 56u);
                        offset += 64u;

                        m_curFog = static_cast<uint8_t>((hi0 >> 36u) & 0xFFu);

                        const uint32_t sBits = static_cast<uint32_t>(lo1 & 0xFFFFFFFFu);
                        const uint32_t tBits = static_cast<uint32_t>((lo1 >> 32u) & 0xFFFFFFFFu);
                        const uint32_t qBits = static_cast<uint32_t>(hi1 & 0xFFFFFFFFu);
                        std::memcpy(&m_curS, &sBits, 4);
                        std::memcpy(&m_curT, &tBits, 4);
                        std::memcpy(&m_curQ, &qBits, 4);
                        if (m_curQ == 0.0f)
                            m_curQ = 1.0f;

                        m_curR = static_cast<uint8_t>(lo2 & 0xFFu);
                        m_curG = static_cast<uint8_t>((lo2 >> 32u) & 0xFFu);
                        m_curB = static_cast<uint8_t>(hi2 & 0xFFu);
                        m_curA = static_cast<uint8_t>((hi2 >> 32u) & 0xFFu);

                        const uint16_t x = static_cast<uint16_t>(lo3 & 0xFFFFu);
                        const uint16_t y = static_cast<uint16_t>((lo3 >> 32u) & 0xFFFFu);
                        const uint32_t z = isXYZF2 ? static_cast<uint32_t>((hi3 >> 4u) & 0xFFFFFFu)
                                                   : static_cast<uint32_t>(hi3 & 0xFFFFFFFFu);
                        const uint8_t f = isXYZF2 ? static_cast<uint8_t>((hi3 >> 36u) & 0xFFu)
                                                  : m_curFog;
                        const bool adk = ((hi3 >> 47u) & 1u) != 0u;

                        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
                        vtx.x = static_cast<float>(x) / 16.0f;
                        vtx.y = static_cast<float>(y) / 16.0f;
                        vtx.z = static_cast<float>(z);
                        vtx.r = m_curR;
                        vtx.g = m_curG;
                        vtx.b = m_curB;
                        vtx.a = m_curA;
                        vtx.q = m_curQ;
                        vtx.s = m_curS;
                        vtx.t = m_curT;
                        vtx.u = m_curU;
                        vtx.v = m_curV;
                        vtx.fog = f;
                        vertexKick(!adk);
                    }
                }
            }
            else
            {
                for (uint32_t loop = 0; loop < nloop; ++loop)
                {
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (offset + 16 > sizeBytes)
                            return;
                        uint64_t lo = loadLE64(data + offset);
                        uint64_t hi = loadLE64(data + offset + 8);
                        offset += 16;
                        writeRegisterPacked(regs[r], lo, hi);
                    }
                }
            }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            if (fastGeomEligible) // FEAT-RD38
            {
                bool allVertexOnly = true;
                bool hasVertexKick = false;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint8_t reg = regs[r];
                    if (!rdIsPureVertexPackedReg(reg))
                        allVertexOnly = false;
                    if (reg == 0x04u || reg == 0x05u)
                        hasVertexKick = true;
                }
                const uint32_t totalItems = nloop * nreg;
                if (allVertexOnly)
                {
                    const uint32_t payloadBytes = totalItems * 8u;
                    const uint32_t padBytes = (totalItems & 1u) ? 8u : 0u;
                    if (offset + payloadBytes + padBytes > sizeBytes)
                        return;
                    offset += payloadBytes + padBytes;
                    s_rdFastGeomQwords.fetch_add((payloadBytes + padBytes) / 16u, std::memory_order_relaxed);
                }
                else
                {
                    uint64_t skippedItems = 0ull;
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (offset + 8 > sizeBytes)
                                return;
                            const uint8_t reg = regs[r];
                            if (rdIsPureVertexPackedReg(reg))
                            {
                                offset += 8;
                                ++skippedItems;
                            }
                            else
                            {
                                writeRegisterUnlocked(reg, loadLE64(data + offset));
                                offset += 8;
                            }
                        }
                    }
                    if (totalItems & 1u)
                        offset += 8;
                    if (skippedItems != 0ull)
                        s_rdFastGeomQwords.fetch_add(skippedItems / 2ull, std::memory_order_relaxed);
                }
                if (hasVertexKick && (s_rdFastGeomLastSerial != s_rdCurSerial || m_stateDirty))
                {
                    s_rdFastGeomLastSerial = s_rdCurSerial;
                    refreshDrawBatch(0);
                    queueDraw(m_drawBatch);
                }
            }
            else
            {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
            }
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }

        if (ps2xRdActive() && ((tagLo >> 15) & 1ull) != 0ull) // FEAT-RD5 // FEAT-RD49
        {
            s_rdCurSerial = 0u;
            s_rdFastGeomLastSerial = 0u; // FEAT-RD38
        }

        if (s_gifStatOn)
        {
            const uint64_t gifStatTsc1 = __rdtsc();
            const uint32_t gifStatQwords = (offset - gifStatDataOffset) / 16u;
            recordGifStatTag(tagLo, tagHi, nloop, flg, nreg, pre, gifStatQwords, gifStatTsc1 - gifStatTsc0, gifStatTsc1);
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        if (ps2xRdActive() && ((tag.lo >> 15u) & 1ull) != 0ull) // FEAT-RD5 // FEAT-RD49
            s_rdCurSerial = 0u;

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    GwScope gwS(2); // PROBE-GW regs
    if (s_gifStatOn)
    {
        switch (regDesc)
        {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x0A:
        case 0x0C:
        case 0x0D:
            s_gifStatRegCounts[regDesc]++;
            break;
        default:
            break;
        }
    }

    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        if (s_leanVerifyRecord)
        {
            s_leanVerifyRecord->push_back({!adk, vtx});
        }
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        if (s_leanVerifyRecord)
        {
            s_leanVerifyRecord->push_back({!adk, vtx});
        }
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    struct RunFlush { const GS *g; ~RunFlush() { g->flushPendingDraws(); } } runFlush{this}; // PERF-GR
    writeRegisterUnlocked(regAddr, value);
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    GwScope gwS(2); // PROBE-GW regs
    if (s_gifStatOn)
    {
        s_gifStatRegCounts[regAddr]++;
    }
#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        probe_ae::onGsWriteRegister(regAddr, value);
    }
#endif
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (isVerboseLog() && debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isVerboseLog() && isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr) // PERF-GR: anything but per-vertex data can change the draw state
    {
    case GS_REG_RGBAQ: case GS_REG_ST: case GS_REG_UV: case GS_REG_XYZF2: case GS_REG_XYZ2:
    case GS_REG_XYZF3: case GS_REG_XYZ3: case GS_REG_FOG:
        break;
    default:
        m_stateDirty = true;
        break;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        (flushPendingDraws(), m_backend.get())->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        (flushPendingDraws(), m_backend.get())->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        if ((m_trxdir == 0u || m_trxdir == 2u) && ps2xRdActive()) // FEAT-RD5b // FEAT-RD49 host->local and local->local (covers uploadImageNativeUnlocked + BeginTransfer)
        {
            RdpScope rdpS(2); // PROBE-RDP
            rdMarkDest(m_bitbltbuf.dpsm, m_bitbltbuf.dbp, m_bitbltbuf.dbw, m_trxpos.dsax, m_trxpos.dsay, m_trxreg.rrw, m_trxreg.rrh);
        }

        if (m_backend)
        {
            if (s_hasDeferredDisplayCopy)
            {
                const uint32_t defDstFbp = s_deferredDisplayCopyBatch.state.context.frame.fbp;
                const uint32_t defDstFbw = s_deferredDisplayCopyBatch.state.context.frame.fbw;
                // direction: 0 = host to local, 1 = local to host, 2 = local to local
                if (m_trxdir == 1) // local to host reads from sbp
                {
                    if (frameOverlaps(m_bitbltbuf.sbp, m_bitbltbuf.sbw, defDstFbp, defDstFbw))
                        flushDeferredOrdered();
                }
                else if (m_trxdir == 2) // local to local reads sbp, writes dbp
                {
                    if (frameOverlaps(m_bitbltbuf.sbp, m_bitbltbuf.sbw, defDstFbp, defDstFbw) ||
                        frameOverlaps(m_bitbltbuf.dbp, m_bitbltbuf.dbw, defDstFbp, defDstFbw))
                        flushDeferredOrdered();
                }
                else if (m_trxdir == 0) // host to local writes to dbp
                {
                    if (frameOverlaps(m_bitbltbuf.dbp, m_bitbltbuf.dbw, defDstFbp, defDstFbw))
                        flushDeferredOrdered();
                }
            }

            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            (flushPendingDraws(), m_backend.get())->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            (flushPendingDraws(), m_backend.get())->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_backend)
        {
            (flushPendingDraws(), m_backend.get())->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        rdNoteDispFbp(value); // FEAT-RD19
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        rdNoteDispFbp(value); // FEAT-RD19
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    case 0x7f: // FEAT-RD5
        if (ps2xRdActive()) // FEAT-RD49
        {
            RdpScope rdpS(3); // PROBE-RDP
            s_rdCurSerial = static_cast<uint32_t>(value & 0xFFFFFFFFull);
            if (s_rdCurSerial != 0u) // FEAT-RD8
                s_rdLastNonZeroSerial = s_rdCurSerial;
        }
        else
        {
            s_rdCurSerial = 0u; // FEAT-RD49
            if (ps2xRdViewOn() && (value & 0x40000000ull) != 0ull) // FEAT-RD49: keep stage-transition detector alive while forced
                s_rdFrameHasGeom.store(true, std::memory_order_relaxed);
        }
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    GwScope gwS(3); // PROBE-GW kicks
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (isVerboseLog() && debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend)
    {
        const bool lean = isGsLeanEnabled();
        const uint64_t gqT0 = lean ? 0ull : __rdtsc(); // PROBE-GQ2
        refreshDrawBatch(needed); // PERF-GR
        GSPrimitiveBatch &batch = m_drawBatch;
        if (!lean || (m_hasPreferredDisplaySource && batch.state.context.frame.fbp == m_preferredDisplayDestFbp) ||
            (batch.state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u))
        {
            if (batch.state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u) // PERF-GB: populate batch.vertices only when needed
            {
                batch.vertices[0] = m_vtxQueue[0];
                batch.vertices[1] = m_vtxQueue[1];
                batch.vertices[2] = GSVertex{};
            }
            updatePreferredDisplaySourceForDraw(batch);
        }
        if (!lean)
            g_gqBuild.fetch_add(__rdtsc() - gqT0, std::memory_order_relaxed);

        bool skipBatch = false;
        if (m_hasPreferredDisplaySource &&
            batch.state.prim.type == GS_PRIM_SPRITE &&
            batch.state.context.frame.fbp == m_preferredDisplayDestFbp &&
            batch.state.context.tex0.tbp0 == m_preferredDisplaySourceFrame.fbp &&
            isSkipDisplayCopyEnabled())
        {
            if (s_hasDeferredDisplayCopy &&
                s_deferredDisplayCopyBatch.state.context.frame.fbp != batch.state.context.frame.fbp)
            {
                flushDeferredOrdered();
            }
            if (batch.vertexCount < 2u) // PERF-GB: ensure deferred display copy batch has vertices
            {
                for (int i = 0; i < 3; ++i)
                    batch.vertices[static_cast<size_t>(i)] = (i < batch.vertexCount) ? m_vtxQueue[i] : GSVertex{};
            }
            s_deferredDisplayCopyBatch = batch;
            s_hasDeferredDisplayCopy = true;
            skipBatch = true;
            s_coSkipped.fetch_add(1u, std::memory_order_relaxed);
        }

        if (!skipBatch)
        {
            if (s_hasDeferredDisplayCopy)
            {
                const uint32_t defDstFbp = s_deferredDisplayCopyBatch.state.context.frame.fbp;
                const uint32_t defDstFbw = s_deferredDisplayCopyBatch.state.context.frame.fbw;

                // (a) another draw whose ctx.frame.fbp == the stored destination fbp
                // (b) any draw that reads the destination fbp (tex0.tbp0 overlapping defDstFbp)
                if (batch.state.context.frame.fbp == defDstFbp ||
                    (batch.state.prim.tme && frameOverlaps(batch.state.context.tex0.tbp0, batch.state.context.tex0.tbw, defDstFbp, defDstFbw)))
                {
                    flushDeferredOrdered();
                }
            }
            queueDraw(batch); // PERF-GR
        }

        if (!lean || gsDebugHistoryEnabled())
        {
            recordDrawDebugEventUnlocked(needed);
        }

        // PROBE-INTERP: per-draw log for the frame-interpolation feasibility study (PS2X_GS_INTERP_PROBE=1, file logs/interp_probe.txt)
        if (s_interpProbeActive)
        {
            static uint32_t s_ipLines = 0;
            if (s_ip && s_ipLines < 1500000u)
            {
                ++s_ipLines;
                std::fprintf(s_ip, "D %llu %u %u %u %u %u", static_cast<unsigned long long>(g_hostPresentSeq.load(std::memory_order_relaxed)),
                             static_cast<unsigned>(m_prim.type), static_cast<unsigned>(batch.state.prim.tme),
                             static_cast<unsigned>(batch.state.context.tex0.tbp0), static_cast<unsigned>(batch.state.context.frame.fbp), static_cast<unsigned>(needed));
                for (int vi = 0; vi < needed && vi < 3; ++vi)
                    std::fprintf(s_ip, " %.2f %.2f %.0f", m_vtxQueue[vi].x, m_vtxQueue[vi].y, m_vtxQueue[vi].z); // PERF-GB
                std::fputc('\n', s_ip);
                if ((s_ipLines & 0x3FFFu) == 0)
                    std::fflush(s_ip);
            }
        }
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    GwScope gwS(4); // PROBE-GW image
    if (g_gwOn) { ps2_gw_count(1, sizeBytes); ps2_iu_xfer(sizeBytes); } // PROBE-GW PROBE-IU
    if (m_backend)
        (flushPendingDraws(), m_backend.get())->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && (flushPendingDraws(), m_backend.get())->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && (flushPendingDraws(), m_backend.get())->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? (flushPendingDraws(), m_backend.get())->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = makeDefaultRasterBackend();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        (flushPendingDraws(), m_backend.get())->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_stateDirty = true;
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? (flushPendingDraws(), m_backend.get())->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        (flushPendingDraws(), m_backend.get())->WriteVram(psm, base, bw, x, y, value);
}

namespace // PERF-GB
{
bool isGsRedundOptEnabled()
{
    static const bool s_enabled = []() {
        const char *e = std::getenv("PS2X_GS_REDUND");
        return !e || e[0] != '0';
    }();
    return s_enabled;
}

bool drawStateEqual(const GSDrawState &a, const GSDrawState &b)
{
    const GSContext &ca = a.context;
    const GSContext &cb = b.context;
    return a.prim.type == b.prim.type &&
           a.prim.iip == b.prim.iip &&
           a.prim.tme == b.prim.tme &&
           a.prim.fge == b.prim.fge &&
           a.prim.abe == b.prim.abe &&
           a.prim.aa1 == b.prim.aa1 &&
           a.prim.fst == b.prim.fst &&
           a.prim.ctxt == b.prim.ctxt &&
           a.prim.fix == b.prim.fix &&
           ca.frame.fbp == cb.frame.fbp &&
           ca.frame.fbw == cb.frame.fbw &&
           ca.frame.psm == cb.frame.psm &&
           ca.frame.fbmsk == cb.frame.fbmsk &&
           ca.zbuf.zbp == cb.zbuf.zbp &&
           ca.zbuf.psm == cb.zbuf.psm &&
           ca.zbuf.zmask == cb.zbuf.zmask &&
           ca.scissor.x0 == cb.scissor.x0 &&
           ca.scissor.x1 == cb.scissor.x1 &&
           ca.scissor.y0 == cb.scissor.y0 &&
           ca.scissor.y1 == cb.scissor.y1 &&
           ca.xyoffset.ofx == cb.xyoffset.ofx &&
           ca.xyoffset.ofy == cb.xyoffset.ofy &&
           ca.tex0.tbp0 == cb.tex0.tbp0 &&
           ca.tex0.tbw == cb.tex0.tbw &&
           ca.tex0.psm == cb.tex0.psm &&
           ca.tex0.tw == cb.tex0.tw &&
           ca.tex0.th == cb.tex0.th &&
           ca.tex0.tcc == cb.tex0.tcc &&
           ca.tex0.tfx == cb.tex0.tfx &&
           ca.tex0.cbp == cb.tex0.cbp &&
           ca.tex0.cpsm == cb.tex0.cpsm &&
           ca.tex0.csm == cb.tex0.csm &&
           ca.tex0.csa == cb.tex0.csa &&
           ca.tex0.cld == cb.tex0.cld &&
           ca.tex1 == cb.tex1 &&
           ca.miptbp1 == cb.miptbp1 &&
           ca.miptbp2 == cb.miptbp2 &&
           ca.clamp == cb.clamp &&
           ca.alpha == cb.alpha &&
           ca.test == cb.test &&
           ca.fba == cb.fba &&
           a.texa.ta0 == b.texa.ta0 &&
           a.texa.aem == b.texa.aem &&
           a.texa.ta1 == b.texa.ta1 &&
           a.texclut.cbw == b.texclut.cbw &&
           a.texclut.cou == b.texclut.cou &&
           a.texclut.cov == b.texclut.cov &&
           a.pabe == b.pabe &&
           a.scanmsk == b.scanmsk &&
           a.dimx == b.dimx &&
           a.dthe == b.dthe &&
           a.colclamp == b.colclamp &&
           a.fogR == b.fogR &&
           a.fogG == b.fogG &&
           a.fogB == b.fogB &&
           a.textureWidth == b.textureWidth &&
           a.textureHeight == b.textureHeight &&
           a.linearFilter == b.linearFilter;
}
} // namespace

// PERF-GR: the draw state is rebuilt only after a state register write; per prim only the vertices change
void GS::refreshDrawBatch(int vertexCount)
{
    GwScope gwS(5); // PROBE-GW primbuild
    GSPrimitiveBatch &batch = m_drawBatch;
    if (m_stateDirty)
    {
        m_stateDirty = false;
        GSDrawState nextState{}; // PERF-GB: compare before bumping m_stateSerial
        nextState.context = m_ctx[m_prim.ctxt ? 1 : 0];
        nextState.prim = m_prim;
        nextState.texa = m_texa;
        nextState.texclut = m_texclut;
        nextState.pabe = m_pabe;
        nextState.scanmsk = m_scanmsk;
        nextState.dimx = m_dimx;
        nextState.dthe = m_dthe;
        nextState.colclamp = m_colclamp;
        nextState.fogR = m_fogR;
        nextState.fogG = m_fogG;
        nextState.fogB = m_fogB;
        nextState.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(nextState.context.tex0.tw, 10u));
        nextState.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(nextState.context.tex0.th, 10u));
        const uint64_t tex1 = nextState.context.tex1;
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
        nextState.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
        if (isGsRedundOptEnabled() && drawStateEqual(batch.state, nextState))
        {
            if (g_gwOn && !m_runPrims.empty())
                ps2_gw_count(4, 1); // PROBE-GW redundant state break skipped
        }
        else
        {
            ++m_stateSerial;
            batch.state = nextState;
        }
    }
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
}

static bool rdNoteKickBookkeeping(const GSContext &ctx, const GSPrimReg &prim, const GSTexaReg &texa, const uint8_t *localMem) // FEAT-RD42
{
    if (!ps2xRdActive()) // FEAT-RD49: zero bookkeeping while forced classic
        return false;
    if (ps2xRdActive()) // FEAT-RD13 // FEAT-RD49
    {
        const uint32_t maskedS = s_rdCurSerial & 0x3FFFFFFFu;
        if (maskedS != 0u)
        {
            std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
            if (s_rd2dGMin == 0u || maskedS < s_rd2dGMin)
                s_rd2dGMin = maskedS;
            if (maskedS > s_rd2dGMax)
                s_rd2dGMax = maskedS;
        }
    }
    const bool isGeomKick = (s_rdCurSerial & 0x40000000u) != 0u; // FEAT-RD10
    if (isGeomKick) // FEAT-RD8b // FEAT-RD10 // FEAT-RD18
    {
        if (ps2xRdViewOn()) // FEAT-RD33: record (serial masked, FBP, XYOFFSET) once per serial
        {
            const uint32_t maskedS = s_rdCurSerial & 0x3FFFFFFFu;
            if (maskedS != 0u)
            {
                const uint32_t fbp = ctx.frame.fbp & 0x1FFu;
                const uint64_t xyoff = static_cast<uint64_t>(ctx.xyoffset.ofx & 0xFFFFu) |
                                       (static_cast<uint64_t>(ctx.xyoffset.ofy & 0xFFFFu) << 32);
                std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
                if (maskedS != s_rdLastKickRecSerial)
                {
                    s_rdLastKickRecSerial = maskedS;
                    const GSScissorReg &sc = ctx.scissor; // FEAT-RD33b
                    s_rdKickRecs.push_back({maskedS, fbp, xyoff, static_cast<uint32_t>(sc.x1) + 1u - sc.x0, static_cast<uint32_t>(sc.y1) + 1u - sc.y0});
                }
            }
        }
        s_rdMainFbp = ctx.frame.fbp;
        rdNotePassKick(ctx.frame.fbp, static_cast<uint64_t>(ctx.xyoffset.ofx), static_cast<uint64_t>(ctx.xyoffset.ofy)); // FEAT-RD23
        s_rdFrameHasGeom.store(true, std::memory_order_relaxed);
        if (!ps2xRdHasDisplayFbp() || ps2xRdIsDisplayFbp(ctx.frame.fbp)) // FEAT-RD19
            s_rdFrameGeomFbp.store(ctx.frame.fbp, std::memory_order_relaxed); // FEAT-RD18b
    }
    if (ps2xRdViewOn() && s_rdCurSerial != 0u) // FEAT-RD17
    {
        const uint64_t rawFrame = rdPackFrame(ctx.frame);
        if (s_rdCurSerial != s_rdLastStateSerial)
        {
            s_rdLastStateSerial = s_rdCurSerial;
            s_rdKickStateChanged = false;
            s_rdFirstAlpha = ctx.alpha;
            s_rdFirstFrame = rawFrame;
            s_rdFirstTest = ctx.test;
            ps2xRdNoteState(s_rdCurSerial,
                            rdPackPrim(prim),
                            ctx.alpha,
                            ctx.test,
                            rawFrame,
                            rdPackZbuf(ctx.zbuf),
                            rdPackTex0(ctx.tex0),
                            ctx.tex1,
                            ctx.clamp,
                            rdPackTexa(texa),
                            ctx.fba);
            ps2xRdNoteXyoff(s_rdCurSerial, static_cast<uint64_t>(ctx.xyoffset.ofx) | (static_cast<uint64_t>(ctx.xyoffset.ofy) << 32)); // FEAT-RD21
            ps2xRdNoteScissor(s_rdCurSerial, rdPackScissor(ctx)); // FEAT-RD22
        }
        else if (!s_rdKickStateChanged &&
                 (ctx.alpha != s_rdFirstAlpha || rawFrame != s_rdFirstFrame || ctx.test != s_rdFirstTest))
        {
            s_rdKickStateChanged = true;
            g_rdStateChangesInKick.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    if (ps2xRdViewOn() && isGeomKick) // FEAT-RD5 // FEAT-RD10
    {
        if (s_rdCurSerial != s_rdLastNotedSerial && prim.tme)
        {
            s_rdLastNotedSerial = s_rdCurSerial;
            const uint64_t rawTex0 = rdPackTex0(ctx.tex0); // FEAT-RD8
            const uint64_t rawTexa = rdPackTexa(texa); // FEAT-RD8
            RdpScope rdpS(0); // PROBE-RDP
            ps2xRdNoteDraw(s_rdCurSerial, rawTex0, rawTexa, localMem, ps2xRdActiveClut());
        }
    }
    return isGeomKick;
}

void GS::queueDraw(const GSPrimitiveBatch &batch)
{
    const bool isGeomKick = rdNoteKickBookkeeping(batch.state.context, batch.state.prim, batch.state.texa, m_localMemoryStorage); // FEAT-RD42
    if (ps2xRdActive() && !isGeomKick) // FEAT-RD8 // FEAT-RD10 // FEAT-RD18 // FEAT-RD49: capture all 2D candidates; FBP filtered at flip time
    {
        const GSContext &ctx = batch.state.context;
        const GSPrimReg &prim = batch.state.prim;
        if (prim.type == GS_PRIM_POINT || prim.type == GS_PRIM_LINE || prim.type == GS_PRIM_LINESTRIP)
        {
            std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
            ++s_rd2dPointLineSkipped;
        }
        else if ((prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u) ||
                 ((prim.type == GS_PRIM_TRIANGLE || prim.type == GS_PRIM_TRISTRIP || prim.type == GS_PRIM_TRIFAN) && batch.vertexCount >= 3u))
        {
            const float ofx = static_cast<float>(ctx.xyoffset.ofx) * (1.0f / 16.0f);
            const float ofy = static_cast<float>(ctx.xyoffset.ofy) * (1.0f / 16.0f);
            const float texW = static_cast<float>(1u << std::min<uint32_t>(ctx.tex0.tw, 10u));
            const float texH = static_cast<float>(1u << std::min<uint32_t>(ctx.tex0.th, 10u));
            auto makeVert = [&](const GSVertex &v, const GSVertex &colSrc) -> RdVert {
                RdVert out{};
                out.pos[0] = v.x - ofx;
                out.pos[1] = v.y - ofy;
                out.pos[2] = static_cast<float>(v.z); // FEAT-RD11
                out.pos[3] = 1.0f;
                out.rgba[0] = colSrc.r;
                out.rgba[1] = colSrc.g;
                out.rgba[2] = colSrc.b;
                out.rgba[3] = colSrc.a;
                if (prim.tme)
                {
                    if (prim.fst)
                    {
                        out.uv[0] = (static_cast<float>(v.u) / 16.0f) / texW;
                        out.uv[1] = (static_cast<float>(v.v) / 16.0f) / texH;
                    }
                    else
                    {
                        const float invQ = (v.q != 0.0f) ? (1.0f / v.q) : 1.0f;
                        out.uv[0] = v.s * invQ;
                        out.uv[1] = v.t * invQ;
                    }
                }
                return out;
            };

            RdVert triVerts[6]{};
            uint32_t addCount = 0u;
            if (prim.type == GS_PRIM_SPRITE)
            {
                const GSVertex &v0 = m_vtxQueue[0];
                const GSVertex &v1 = m_vtxQueue[1];
                RdVert c00 = makeVert(v0, v1); // FEAT-RD11
                const RdVert c11 = makeVert(v1, v1);
                c00.pos[2] = c11.pos[2]; // FEAT-RD11: GS sprite Z comes from v1
                RdVert c10 = c00;
                c10.pos[0] = c11.pos[0];
                c10.uv[0] = c11.uv[0];
                RdVert c01 = c00;
                c01.pos[1] = c11.pos[1];
                c01.uv[1] = c11.uv[1];
                triVerts[0] = c00; triVerts[1] = c10; triVerts[2] = c11;
                triVerts[3] = c00; triVerts[4] = c11; triVerts[5] = c01;
                addCount = 6u;
            }
            else
            {
                const GSVertex &flatCol = prim.iip ? m_vtxQueue[0] : m_vtxQueue[2];
                triVerts[0] = makeVert(m_vtxQueue[0], prim.iip ? m_vtxQueue[0] : flatCol);
                triVerts[1] = makeVert(m_vtxQueue[1], prim.iip ? m_vtxQueue[1] : flatCol);
                triVerts[2] = makeVert(m_vtxQueue[2], prim.iip ? m_vtxQueue[2] : flatCol);
                addCount = 3u;
            }

            const uint64_t rawTex0 = rdPackTex0(ctx.tex0);
            const uint64_t rawTexa = rdPackTexa(batch.state.texa);
            uint32_t texSerial = 0u;
            if (prim.tme)
            {
                texSerial = (s_rdCurSerial != 0u) ? s_rdCurSerial : ((s_rd2dTexCounter++ & 0x7FFFFFFFu) | 0x80000000u); // FEAT-RD10
                ps2xRdNoteDraw(texSerial, rawTex0, rawTexa, m_localMemoryStorage, ps2xRdActiveClut());
            }

            Rd2DBatch b2d{};
            b2d.afterSerial = (s_rdCurSerial != 0u) ? s_rdCurSerial : s_rdLastNonZeroSerial; // FEAT-RD10
            b2d.texSerial = texSerial;
            b2d.prim = rdPackPrim(prim);
            b2d.tex0 = rawTex0;
            b2d.texa = rawTexa;
            b2d.clamp = ctx.clamp;
            b2d.tex1 = ctx.tex1;
            b2d.alpha = ctx.alpha;
            b2d.test = ctx.test;
            b2d.frame = rdPackFrame(ctx.frame);
            b2d.zbuf = rdPackZbuf(ctx.zbuf);
            b2d.scissor = rdPackScissor(ctx); // FEAT-RD22

            std::lock_guard<std::mutex> capLock(s_rd2dCapMutex);
            b2d.first = static_cast<uint32_t>(s_rd2dVerts.size());
            b2d.count = addCount;
            s_rd2dVerts.insert(s_rd2dVerts.end(), triVerts, triVerts + addCount);
            s_rd2dBatches.push_back(b2d);
        }
    }
    if (g_rdpOn) { ++s_rdpQueueDraws; if ((s_rdpQueueDraws & 1023u) == 0u) rdpMaybeReport(); } // PROBE-RDP
    GwScope gwS(5); // PROBE-GW primbuild
    s_rdLatchPrims.fetch_add(1u, std::memory_order_relaxed); // FEAT-RD50
    if (g_gwOn) ps2_gw_count(0, 1); // PROBE-GW prims
    if (!m_runPrims.empty() && (m_runSerial != m_stateSerial || s_rdRunIsGeom != isGeomKick)) // FEAT-RD18
    {
        if (g_gwOn) ps2_gw_count(3, 1); // PROBE-GW state-serial break
        flushPendingDraws();
    }
    if (m_runPrims.empty())
    {
        m_runState = batch.state;
        m_runSerial = m_stateSerial;
        s_rdRunIsGeom = isGeomKick; // FEAT-RD18
        if (m_runPrims.capacity() < 512u)
            m_runPrims.reserve(512u);
    }
    m_runPrims.emplace_back(); // PERF-GB: single-copy vertices straight into GSRunPrim slot
    GSRunPrim &rp = m_runPrims.back();
    const uint8_t vc = batch.vertexCount;
    rp.vertexCount = vc;
    for (uint8_t i = 0; i < vc; ++i)
        rp.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    if (m_runPrims.size() >= 512u)
        flushPendingDraws();
}

void GS::flushPendingDraws() const
{
    if (m_runPrims.empty())
        return;
    GwScope gwS(6); // PROBE-GW statechg (run flush); backend SubmitRun re-scopes to vkrec
    if (g_gwOn) ps2_gw_count(2, 1); // PROBE-GW
    if (m_backend)
    {
        g_rdCurDrawIsGeom = ps2xRdActive() && s_rdRunIsGeom && (!ps2xRdHasDisplayFbp() || ps2xRdIsDisplayFbp(m_runState.context.frame.fbp)) && // FEAT-RD48 // FEAT-RD49
            rdScissorQualifies(static_cast<uint32_t>(m_runState.context.scissor.x1) + 1u - m_runState.context.scissor.x0, static_cast<uint32_t>(m_runState.context.scissor.y1) + 1u - m_runState.context.scissor.y0) && // FEAT-RD33b
            ps2xRdIsMainPass(m_runState.context.frame.fbp, static_cast<uint64_t>(m_runState.context.xyoffset.ofx) | (static_cast<uint64_t>(m_runState.context.xyoffset.ofy) << 32)); // FEAT-RD23 // FEAT-RD18 // FEAT-RD19: off-screen RT draws still rasterised
        const uint64_t gqS0 = __rdtsc(); // PROBE-GQ2
        m_backend->SubmitRun(m_runState, m_runPrims.data(), m_runPrims.size());
        g_gqSubmit.fetch_add(__rdtsc() - gqS0, std::memory_order_relaxed);
        g_gqDraws.fetch_add(m_runPrims.size(), std::memory_order_relaxed);
    }
    m_runPrims.clear();
}

void GS::flushDeferredOrdered()
{
    flushPendingDraws();
    flushDeferredDisplayCopy(m_backend.get());
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
    {
        m_hasPreferredDisplaySource = false;
        flushDeferredOrdered();
    }
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int spanW = std::max(1, x1 - x0);
    const int spanH = std::max(1, y1 - y0);
    const int xEnd = x0 + spanW - 1;
    const int yEnd = y0 + spanH - 1;

    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);

    const bool covers = (x0 <= ctx.scissor.x0 && y0 <= ctx.scissor.y0 &&
                         xEnd >= ctx.scissor.x1 && yEnd >= ctx.scissor.y1 &&
                         spanW >= 320 && spanH >= 224);

    const bool isColIdentity = (v1.r == 0x80u && v1.g == 0x80u && v1.b == 0x80u);

    // Alpha blending condition:
    // Existing rule: alphaMode == 0x64 && (alphaFix == 0x60 || alphaFix == 0x80)
    // Relaxed rule: alphaFix in range 0x40..0x80 with standard blend OR abe==1 with colour modulation 0x80
    const bool alphaAccept = (alphaMode == 0x64u && (alphaFix == 0x60u || alphaFix == 0x80u)) ||
                             (alphaMode == 0x64u && (alphaFix >= 0x40u && alphaFix <= 0x80u)) ||
                             isColIdentity;

    // Task CO: the 512x448 movie display copy is drawn through context 1 (prim.ctxt == 0), so ctxt is no longer required
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && covers && alphaAccept;

    if (displayCopy)
    {
        s_coRecognised.fetch_add(1u, std::memory_order_relaxed);
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }

    checkPeriodicCoLog();
    uint32_t loggedCount = covers ? s_coAlphaLogged.fetch_add(1u, std::memory_order_relaxed) : 1000u;
    if (loggedCount < 6u)
    {
        probe_ae::bhLog("[probe] co disp alphaReg=0x%llx mode=0x%x fix=0x%x covers=%u displayCopy=%u tme=%u abe=%u fst=%u ctxt=%u fbp=0x%x tbp0=0x%x\n",
                        (unsigned long long)ctx.alpha, (unsigned)alphaMode, (unsigned)alphaFix,
                        covers ? 1u : 0u, displayCopy ? 1u : 0u,
                        (unsigned)state.prim.tme, (unsigned)state.prim.abe, (unsigned)state.prim.fst,
                        (unsigned)state.prim.ctxt, (unsigned)ctx.frame.fbp, (unsigned)ctx.tex0.tbp0);
    }
}
